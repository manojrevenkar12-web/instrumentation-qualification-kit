#include "fake_module.hpp"
#include "iqk/config.hpp"
#include "iqk/loader.hpp"
#include "support.hpp"

namespace {

using iqk::PlanLoader;
using iqk_test::kMs;

constexpr std::int64_t kTimeout = iqk::cfg::plan::reg_timeout_ms;
constexpr int kRetries = static_cast<int>(iqk::cfg::plan::reg_retries);

iqk::plan::Plan make_plan() {
  std::vector<iqk::plan::Finding> f;
  auto p = iqk::plan::parse("iqk-plan 1\ncampaign c1\nchannel a1 aam:1:0 rate=100 range_mv=1000\n", f);
  EXPECT_TRUE(p.has_value());
  EXPECT_TRUE(iqk::plan::validate(*p, f));
  return *p;
}

bool same(const iqk::CanFrame& a, const iqk::CanFrame& b) {
  return a.id == b.id && a.dlc == b.dlc && a.data == b.data;
}

// Runs the loader against a module that ignores the first `drop` transmissions of every request
// (drop < 0: never answers). Returns the number of transmissions of the busiest request.
struct Run {
  int max_tx{0};
  std::int64_t end{0};
};

Run run(PlanLoader& l, iqk_test::FakeModule& m, int drop) {
  Run r;
  std::int64_t now = 0;
  std::size_t last_pos = static_cast<std::size_t>(-1);
  int tx = 0;
  for (; now < 100'000 * kMs && l.status() == PlanLoader::Status::busy; now += kMs) {
    auto q = l.poll(now);
    if (!q) continue;
    if (l.progress() != last_pos) {
      last_pos = l.progress();
      tx = 0;
    }
    ++tx;
    r.max_tx = std::max(r.max_tx, tx);
    if (drop >= 0 && tx > drop) {
      if (auto a = m.on_frame(*q)) l.on_frame(*a, now);
    }
  }
  r.end = now;
  return r;
}

}  // namespace

TEST(RegAck, UnansweredRequestIsRetransmittedExactlyAtTimeout) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  l.start(make_plan(), 0);
  auto first = l.poll(0);
  ASSERT_TRUE(first.has_value());
  EXPECT_FALSE(l.poll(1).has_value());
  EXPECT_FALSE(l.poll((kTimeout - 1) * kMs).has_value());
  EXPECT_EQ(l.status(), PlanLoader::Status::busy);
  auto retry = l.poll(kTimeout * kMs);
  ASSERT_TRUE(retry.has_value());
  EXPECT_TRUE(same(*first, *retry));
  EXPECT_EQ(l.progress(), 0U);
}

TEST(RegAck, EachRetryWaitsAFullTimeoutFromThePreviousTransmission) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  l.start(make_plan(), 1000 * kMs);
  std::int64_t t = 1000 * kMs;
  ASSERT_TRUE(l.poll(t).has_value());
  for (int i = 0; i < kRetries; ++i) {
    EXPECT_FALSE(l.poll(t + (kTimeout - 1) * kMs).has_value()) << "retry " << i;
    t += kTimeout * kMs;
    EXPECT_TRUE(l.poll(t).has_value()) << "retry " << i;
  }
}

TEST(RegAck, LoadFailsWithTimeoutAfterInitialTryPlusAllRetries) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  l.start(make_plan(), 0);
  int tx = 0;
  std::int64_t t = 0;
  ASSERT_TRUE(l.poll(t).has_value());
  ++tx;
  for (int i = 0; i < kRetries; ++i) {
    t += kTimeout * kMs;
    ASSERT_TRUE(l.poll(t).has_value());
    ++tx;
  }
  EXPECT_EQ(tx, 1 + kRetries);
  // Still within the last timeout window: not yet failed.
  EXPECT_FALSE(l.poll(t + (kTimeout - 1) * kMs).has_value());
  EXPECT_EQ(l.status(), PlanLoader::Status::busy);
  // The last attempt times out: the load fails, with no further transmission.
  EXPECT_FALSE(l.poll(t + kTimeout * kMs).has_value());
  EXPECT_EQ(l.status(), PlanLoader::Status::failed);
  EXPECT_EQ(l.failure(), PlanLoader::Failure::timeout);
  EXPECT_EQ(l.failed_node(), 1);
  EXPECT_EQ(l.detail(), 0U);
  // Stays failed and silent.
  EXPECT_FALSE(l.poll(t + 10'000 * kMs).has_value());
  EXPECT_EQ(l.status(), PlanLoader::Status::failed);
}

TEST(RegAck, SilentModuleNeverLoads) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  iqk_test::FakeModule m;
  l.start(make_plan(), 0);
  auto r = run(l, m, -1);
  EXPECT_EQ(l.status(), PlanLoader::Status::failed);
  EXPECT_EQ(l.failure(), PlanLoader::Failure::timeout);
  EXPECT_EQ(r.max_tx, 1 + kRetries);
  EXPECT_EQ(m.requests, 0);
}

TEST(RegAck, ModuleAnsweringOnLastRetrySucceeds) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  iqk_test::FakeModule m;
  l.start(make_plan(), 0);
  auto r = run(l, m, kRetries);  // first `kRetries` transmissions of every request lost
  EXPECT_EQ(l.status(), PlanLoader::Status::done);
  EXPECT_EQ(l.failure(), PlanLoader::Failure::none);
  EXPECT_EQ(r.max_tx, 1 + kRetries);
}

TEST(RegAck, ModuleAnsweringOnePastLastRetryFails) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  iqk_test::FakeModule m;
  l.start(make_plan(), 0);
  auto r = run(l, m, kRetries + 1);
  EXPECT_EQ(l.status(), PlanLoader::Status::failed);
  EXPECT_EQ(l.failure(), PlanLoader::Failure::timeout);
  EXPECT_EQ(r.max_tx, 1 + kRetries);
}

TEST(RegAck, AnsweredRequestIsNotRetransmittedAndNextOneIsSent) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  iqk_test::FakeModule m;
  l.start(make_plan(), 0);
  auto q = l.poll(0);
  ASSERT_TRUE(q.has_value());
  auto a = m.on_frame(*q);
  ASSERT_TRUE(a.has_value());
  l.on_frame(*a, 10 * kMs);
  EXPECT_EQ(l.progress(), 1U);
  auto q2 = l.poll(10 * kMs);
  ASSERT_TRUE(q2.has_value());
  EXPECT_FALSE(same(*q, *q2));
  // Healthy module: every request sent exactly once.
  PlanLoader l2;
  iqk_test::FakeModule m2;
  l2.start(make_plan(), 0);
  auto r = run(l2, m2, 0);
  EXPECT_EQ(l2.status(), PlanLoader::Status::done);
  EXPECT_EQ(r.max_tx, 1);
  EXPECT_EQ(m2.requests, static_cast<int>(l2.total()));
}

TEST(RegAck, RetryBudgetIsPerRequestNotPerLoad) {
  IQK_VERIFIES("SRS-050");
  // Every request needs all retries; the whole load still succeeds, so the budget restarts per request.
  PlanLoader l;
  iqk_test::FakeModule m;
  l.start(make_plan(), 0);
  (void)run(l, m, kRetries);
  EXPECT_EQ(l.status(), PlanLoader::Status::done);
  EXPECT_GT(l.total(), 2U);
  EXPECT_EQ(m.requests, static_cast<int>(l.total()));
}

TEST(RegAck, FailureReportsTheRequestThatWasNotAcknowledged) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  iqk_test::FakeModule m;
  l.start(make_plan(), 0);
  // Acknowledge the first two requests, then go silent.
  std::int64_t t = 0;
  for (int i = 0; i < 2; ++i) {
    auto q = l.poll(t);
    ASSERT_TRUE(q.has_value());
    l.on_frame(*m.on_frame(*q), t);
  }
  ASSERT_EQ(l.progress(), 2U);
  (void)l.poll(t);
  for (int i = 0; i < kRetries + 1; ++i) {
    t += kTimeout * kMs;
    (void)l.poll(t);
  }
  EXPECT_EQ(l.status(), PlanLoader::Status::failed);
  EXPECT_EQ(l.failure(), PlanLoader::Failure::timeout);
  EXPECT_EQ(l.detail(), 2U);
  EXPECT_EQ(l.progress(), 2U);
}

TEST(RegAck, LateAnswerToARetriedRequestIsAccepted) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  iqk_test::FakeModule m;
  l.start(make_plan(), 0);
  auto q = l.poll(0);
  ASSERT_TRUE(q.has_value());
  auto retry = l.poll(kTimeout * kMs);
  ASSERT_TRUE(retry.has_value());
  auto a = m.on_frame(*q);
  ASSERT_TRUE(a.has_value());
  l.on_frame(*a, (kTimeout + 5) * kMs);
  EXPECT_EQ(l.progress(), 1U);
  EXPECT_EQ(l.status(), PlanLoader::Status::busy);
}

TEST(RegAck, AnswerAfterFailureDoesNotRevive) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  iqk_test::FakeModule m;
  l.start(make_plan(), 0);
  auto q = l.poll(0);
  ASSERT_TRUE(q.has_value());
  std::int64_t t = 0;
  for (int i = 0; i < kRetries + 1; ++i) {
    t += kTimeout * kMs;
    (void)l.poll(t);
  }
  ASSERT_EQ(l.status(), PlanLoader::Status::failed);
  l.on_frame(*m.on_frame(*q), t + kMs);
  EXPECT_EQ(l.status(), PlanLoader::Status::failed);
  EXPECT_EQ(l.failure(), PlanLoader::Failure::timeout);
  EXPECT_EQ(l.progress(), 0U);
}
