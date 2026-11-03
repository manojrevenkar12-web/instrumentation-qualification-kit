// SRS-050 Register writes are acknowledged: every register request is answered within plan.reg_timeout_ms or
// retried, at most plan.reg_retries times; then the load fails.
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <utility>
#include <vector>

#include "fake_module.hpp"
#include "iqk/config.hpp"
#include "iqk/dau.hpp"
#include "iqk/faultlog.hpp"
#include "iqk/icd/aam.hpp"
#include "iqk/loader.hpp"
#include "iqk/plan.hpp"
#include "support.hpp"

namespace {

namespace aam = iqk::icd::aam;
using iqk::CanFrame;
using iqk::PlanLoader;

constexpr std::int64_t kTimeout = iqk::cfg::plan::reg_timeout_ms * iqk_test::kMs;
constexpr int kRetries = static_cast<int>(iqk::cfg::plan::reg_retries);
constexpr int kNode = 1;

constexpr std::string_view kPlanText =
    "iqk-plan 1\ncampaign srs050\nchannel ch0 aam:1:0 rate=100 range_mv=1000\n";

// One channel on module 1: the loader writes all 12 inputs of the module, commits, reads back, checks the CRC.
iqk::plan::Plan one_input_plan() {
  iqk::plan::Plan p;
  p.campaign = "srs050";
  iqk::plan::Channel c;
  c.name = "ch0";
  c.kind = iqk::plan::Kind::aam;
  c.node = kNode;
  c.input = 0;
  c.rate_hz = 100;
  c.range_mv = 1000;
  p.channels.push_back(c);
  return p;
}

bool same(const CanFrame& a, const CanFrame& b) {
  return a.id == b.id && a.dlc == b.dlc && a.data == b.data;
}

bool keep(const CanFrame& /*f*/, int /*nth*/) {
  return false;
}

struct Trace {
  std::vector<CanFrame> sent;     // every transmission, in order
  std::vector<std::int64_t> at;   // when it was sent
};

// Runs the loader from t=0 in 1 ms steps against the module until the load is no longer busy or `end` passes.
// `drop` decides whether a transmitted request is lost on the bus; nth counts earlier transmissions of the same frame.
Trace run_load(PlanLoader& l, iqk_test::FakeModule& m, const iqk::plan::Plan& p,
               const std::function<bool(const CanFrame&, int)>& drop, std::int64_t end) {
  Trace t;
  std::map<std::pair<std::uint32_t, std::array<std::uint8_t, 8>>, int> seen;
  l.start(p, 0);
  for (std::int64_t now = 0; now <= end && l.status() == PlanLoader::Status::busy; now += iqk_test::kMs) {
    while (const auto f = l.poll(now)) {
      t.sent.push_back(*f);
      t.at.push_back(now);
      const int nth = seen[std::pair{f->id, f->data}]++;
      if (drop(*f, nth)) {
        continue;
      }
      if (const auto r = m.on_frame(*f)) {
        l.on_frame(*r, now);
      }
    }
  }
  return t;
}

struct DauTrace {
  std::vector<CanFrame> frames;  // every frame the unit put on the CAN port
  std::vector<std::int64_t> at;
};

// Ticks the unit every 1 ms from t=0 to `end`. Frames it sends on the CAN port reach the module, and its answers go
// back in unless `answer` is false (a silent module).
DauTrace run_dau(iqk::Dau& dau, iqk_test::FakeModule& m, bool answer, std::int64_t end) {
  DauTrace t;
  for (std::int64_t now = 0; now <= end; now += iqk_test::kMs) {
    std::vector<CanFrame> out;
    dau.tick(now, [&out](iqk::Port p, std::span<const std::uint8_t> b) {
      CanFrame f{};
      if (p == iqk::Port::can && iqk::decode_can_udp(b, f)) {
        out.push_back(f);
      }
    });
    for (const CanFrame& f : out) {
      t.frames.push_back(f);
      t.at.push_back(now);
      if (!answer) {
        continue;
      }
      if (const auto r = m.on_frame(f)) {
        std::array<std::uint8_t, iqk::kCanUdpSize> b{};
        (void)iqk::encode_can_udp(*r, b);
        dau.on_input(iqk::Port::can, b, now, [](iqk::Port, std::span<const std::uint8_t>) {});
      }
    }
  }
  return t;
}

}  // namespace

TEST(RegisterAck, AnsweredInTimeIsNotRetransmitted) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  iqk_test::FakeModule m;
  const Trace t = run_load(l, m, one_input_plan(), keep, 10 * iqk_test::kS);
  ASSERT_EQ(l.status(), PlanLoader::Status::done);
  EXPECT_EQ(t.sent.size(), l.total()) << "every answered request must be sent exactly once";
}

TEST(RegisterAck, AnswerBeforeTimeoutIsAcceptedWithoutRetry) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  iqk_test::FakeModule m;
  l.start(one_input_plan(), 0);
  const auto first = l.poll(0);
  ASSERT_TRUE(first.has_value());
  EXPECT_FALSE(l.poll(kTimeout - 1).has_value()) << "retransmitted before reg_timeout_ms had elapsed";
  const auto ack = m.on_frame(*first);
  ASSERT_TRUE(ack.has_value());
  l.on_frame(*ack, kTimeout - 1);
  EXPECT_EQ(l.progress(), 1U);
  const auto next = l.poll(kTimeout - 1);
  ASSERT_TRUE(next.has_value());
  EXPECT_FALSE(same(*next, *first)) << "an answered request was sent again";
}

TEST(RegisterAck, UnansweredRequestIsRetransmittedExactlyAtTimeoutAndTimerRestarts) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  l.start(one_input_plan(), 0);
  const auto first = l.poll(0);
  ASSERT_TRUE(first.has_value());
  EXPECT_FALSE(l.poll(kTimeout - 1).has_value());
  const auto retry = l.poll(kTimeout);
  ASSERT_TRUE(retry.has_value()) << "no retransmission at reg_timeout_ms";
  EXPECT_TRUE(same(*retry, *first)) << "a retry must repeat the request unchanged";
  EXPECT_FALSE(l.poll(2 * kTimeout - 1).has_value()) << "timeout is measured from the last transmission";
  EXPECT_TRUE(l.poll(2 * kTimeout).has_value());
  EXPECT_EQ(l.status(), PlanLoader::Status::busy);
}

TEST(RegisterAck, SilentModuleGetsRegRetriesRetriesThenLoadFails) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  l.start(one_input_plan(), 0);
  std::vector<CanFrame> sent;
  for (int k = 0; k <= kRetries; ++k) {
    const std::int64_t t = k * kTimeout;
    const auto f = l.poll(t);
    ASSERT_TRUE(f.has_value()) << "transmission " << k << " missing at " << t << " ns";
    sent.push_back(*f);
    EXPECT_FALSE(l.poll(t + kTimeout - 1).has_value());
    EXPECT_EQ(l.status(), PlanLoader::Status::busy) << "failed before the retry budget was used up";
  }
  const std::int64_t give_up = (kRetries + 1) * kTimeout;
  EXPECT_FALSE(l.poll(give_up).has_value());
  EXPECT_EQ(l.status(), PlanLoader::Status::failed);
  EXPECT_EQ(l.failure(), PlanLoader::Failure::timeout);
  EXPECT_EQ(l.failed_node(), kNode);
  EXPECT_FALSE(l.poll(give_up + 10 * kTimeout).has_value()) << "a failed load must not transmit again";
  ASSERT_EQ(sent.size(), static_cast<std::size_t>(kRetries) + 1U);
  for (const CanFrame& f : sent) {
    EXPECT_TRUE(same(f, sent.front()));
  }
  EXPECT_EQ(sent.front().id, aam::AamRegRequest::can_id(kNode));
}

TEST(RegisterAck, LastPermittedRetryAnsweredLoadProceeds) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  iqk_test::FakeModule m;
  l.start(one_input_plan(), 0);
  const auto first = l.poll(0);
  ASSERT_TRUE(first.has_value());
  for (int k = 1; k < kRetries; ++k) {
    ASSERT_TRUE(l.poll(k * kTimeout).has_value()) << "retry " << k << " missing";
  }
  const std::int64_t last = kRetries * kTimeout;
  const auto final_try = l.poll(last);
  ASSERT_TRUE(final_try.has_value()) << "retry number reg_retries missing";
  const auto ack = m.on_frame(*final_try);
  ASSERT_TRUE(ack.has_value());
  l.on_frame(*ack, last);
  EXPECT_NE(l.status(), PlanLoader::Status::failed) << "the last permitted retry was answered but the load failed";
  EXPECT_EQ(l.progress(), 1U);
  const auto next = l.poll(last);
  ASSERT_TRUE(next.has_value());
  EXPECT_FALSE(same(*next, *first)) << "the answered request was sent again";
}

TEST(RegisterAck, RetryBudgetAppliesToEachRegisterRequest) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  iqk_test::FakeModule m;
  // Every register request loses its first reg_retries transmissions and is answered on the last permitted retry.
  const Trace t = run_load(
      l, m, one_input_plan(), [](const CanFrame&, int nth) { return nth < kRetries; }, 100 * iqk_test::kS);
  ASSERT_EQ(l.status(), PlanLoader::Status::done) << "a request used up its retries and the load failed";
  EXPECT_EQ(t.sent.size(), l.total() * (static_cast<std::size_t>(kRetries) + 1U));
}

TEST(RegisterAck, AbortAnswerFailsTheLoadWithoutRetry) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  iqk_test::FakeModule m;
  m.abort_all = true;
  const Trace t = run_load(l, m, one_input_plan(), keep, 10 * iqk_test::kS);
  EXPECT_EQ(l.status(), PlanLoader::Status::failed);
  EXPECT_EQ(l.failure(), PlanLoader::Failure::abort);
  EXPECT_EQ(l.failed_node(), kNode);
  EXPECT_EQ(t.sent.size(), 1U) << "the module answered (abort), so the request was not retried";
}

TEST(RegisterAck, AnswerToAnotherRegisterDoesNotAcknowledgeTheRequest) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  l.start(one_input_plan(), 0);
  const auto first = l.poll(0);
  ASSERT_TRUE(first.has_value());
  aam::AamRegResponse wrong{};
  wrong.cmd = 0x60;
  wrong.index = static_cast<std::uint16_t>(aam::kRegChEnableIndex + 1U);
  wrong.sub = static_cast<std::uint8_t>(aam::kRegChEnableSub);
  CanFrame w{};
  w.id = aam::AamRegResponse::can_id(kNode);
  w.dlc = static_cast<std::uint8_t>(aam::encode(wrong, w.data));
  l.on_frame(w, 0);
  EXPECT_EQ(l.progress(), 0U);
  EXPECT_EQ(l.status(), PlanLoader::Status::busy);
  const auto retry = l.poll(kTimeout);
  ASSERT_TRUE(retry.has_value()) << "an answer for another register stopped the retries";
  EXPECT_TRUE(same(*retry, *first));
}

TEST(RegisterAck, UnansweredPlanLeavesUnitUnarmedAfterRetryBudget) {
  IQK_VERIFIES("SRS-050");
  iqk::Dau dau{0x1234};
  std::vector<iqk::plan::Finding> findings;
  ASSERT_TRUE(dau.load_plan(kPlanText, 0, findings)) << "test plan rejected: " << findings.size() << " finding(s)";
  iqk_test::FakeModule m;
  const DauTrace t = run_dau(dau, m, /*answer=*/false, (kRetries + 2) * kTimeout);

  std::vector<std::int64_t> req_times;
  for (std::size_t i = 0; i < t.frames.size(); ++i) {
    if (t.frames[i].id == aam::AamRegRequest::can_id(kNode)) {
      req_times.push_back(t.at[i]);
    }
  }
  std::vector<std::int64_t> expected;
  for (int k = 0; k <= kRetries; ++k) {
    expected.push_back(k * kTimeout);
  }
  EXPECT_EQ(req_times, expected) << "requests must go out at 0 and after each reg_timeout_ms, reg_retries times";

  EXPECT_EQ(dau.loader().status(), PlanLoader::Status::failed);
  EXPECT_FALSE(dau.armed());
  std::vector<std::int64_t> failed_at;
  for (std::size_t i = 0; i < dau.faults().size(); ++i) {
    if (dau.faults()[i].code == iqk::Fault::plan_load_failed) {
      failed_at.push_back(dau.faults()[i].local_ns);
    }
  }
  EXPECT_EQ(failed_at, std::vector<std::int64_t>{(kRetries + 1) * kTimeout})
      << "the load must fail when the retry budget runs out, not before";
}

TEST(RegisterAck, AnsweredPlanArmsUnit) {
  IQK_VERIFIES("SRS-050");
  iqk::Dau dau{0x1234};
  std::vector<iqk::plan::Finding> findings;
  ASSERT_TRUE(dau.load_plan(kPlanText, 0, findings));
  iqk_test::FakeModule m;
  (void)run_dau(dau, m, /*answer=*/true, 1000 * iqk_test::kMs);
  EXPECT_EQ(dau.loader().status(), PlanLoader::Status::done);
  EXPECT_TRUE(dau.armed());
  EXPECT_EQ(dau.faults().count(iqk::Fault::plan_load_failed), 0U);
}
