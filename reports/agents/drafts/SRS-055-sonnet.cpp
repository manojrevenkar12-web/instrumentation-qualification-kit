#include "fake_module.hpp"
#include "iqk/icd/aam.hpp"
#include "iqk/loader.hpp"
#include "iqk/plan.hpp"
#include "support.hpp"

namespace {

namespace aam = iqk::icd::aam;
using iqk::PlanLoader;

iqk::plan::Plan make_plan(std::initializer_list<int> nodes) {
  iqk::plan::Plan p;
  p.campaign = "c";
  for (const int n : nodes) {
    for (int in = 0; in < 2; ++in) {
      iqk::plan::Channel c;
      c.name = "ch" + std::to_string(n) + "_" + std::to_string(in);
      c.kind = iqk::plan::Kind::aam;
      c.node = n;
      c.input = in;
      c.rate_hz = 100;
      c.range_mv = 1000;
      p.channels.push_back(c);
    }
  }
  return p;
}

iqk::CanFrame response(int node, std::uint8_t cmd, std::uint16_t index, std::uint8_t sub, std::uint32_t value) {
  aam::AamRegResponse a{};
  a.cmd = cmd;
  a.index = index;
  a.sub = sub;
  a.value = value;
  iqk::CanFrame f{};
  f.id = aam::AamRegResponse::can_id(static_cast<std::uint8_t>(node));
  f.dlc = static_cast<std::uint8_t>(aam::encode(a, f.data));
  return f;
}

aam::AamRegRequest decode_req(const iqk::CanFrame& f) {
  aam::AamRegRequest r{};
  EXPECT_TRUE(aam::decode(f.payload(), r));
  return r;
}

int req_node(const iqk::CanFrame& f) { return static_cast<int>(f.id - aam::AamRegRequest::can_id(0)); }

// The correct answer to a request, as a healthy module gives it.
iqk::CanFrame good_answer(const iqk::CanFrame& req, std::uint32_t read_value = 0) {
  const auto r = decode_req(req);
  return response(req_node(req), r.cmd == 0x23 ? 0x60 : 0x43, r.index, r.sub, r.cmd == 0x23 ? 0U : read_value);
}

struct Rig {
  iqk::plan::Plan plan;
  PlanLoader loader;
  std::int64_t now{0};
  explicit Rig(iqk::plan::Plan p) : plan(std::move(p)) { loader.start(plan, now); }
  iqk::CanFrame next() {
    now += iqk_test::kMs;
    auto f = loader.poll(now);
    EXPECT_TRUE(f.has_value());
    return f.value_or(iqk::CanFrame{});
  }
  void feed(const iqk::CanFrame& f) {
    now += iqk_test::kMs;
    loader.on_frame(f, now);
  }
};

}  // namespace

TEST(Agent, AnswerToPreviousRequestDoesNotCompleteCurrentOne) {
  IQK_VERIFIES("SRS-055");
  Rig rig(make_plan({1}));
  const auto first = rig.next();
  const auto first_answer = good_answer(first);
  rig.feed(first_answer);
  ASSERT_EQ(rig.loader.progress(), 1U);
  const auto second = rig.next();
  ASSERT_NE(decode_req(first).sub, decode_req(second).sub);

  rig.feed(first_answer);  // late duplicate of the earlier answer
  EXPECT_EQ(rig.loader.progress(), 1U);
  EXPECT_EQ(rig.loader.status(), PlanLoader::Status::busy);

  rig.feed(good_answer(second));
  EXPECT_EQ(rig.loader.progress(), 2U);
}

TEST(Agent, LateAnswerArrivingBeforeNextRequestIsSentIsDiscarded) {
  IQK_VERIFIES("SRS-055");
  Rig rig(make_plan({1}));
  const auto first = rig.next();
  rig.feed(good_answer(first));
  ASSERT_EQ(rig.loader.progress(), 1U);
  rig.feed(good_answer(first));  // nothing outstanding now
  EXPECT_EQ(rig.loader.progress(), 1U);
  const auto second = rig.next();
  rig.feed(good_answer(first));  // and again while the second is outstanding
  EXPECT_EQ(rig.loader.progress(), 1U);
  rig.feed(good_answer(second));
  EXPECT_EQ(rig.loader.progress(), 2U);
}

TEST(Agent, AnswerMustMatchIndexAndSubOfCurrentRequest) {
  IQK_VERIFIES("SRS-055");
  Rig rig(make_plan({1}));
  const auto req = rig.next();
  const auto r = decode_req(req);
  const int node = req_node(req);
  rig.feed(response(node, 0x60, r.index, static_cast<std::uint8_t>(r.sub + 1), 0));   // other sub
  rig.feed(response(node, 0x60, static_cast<std::uint16_t>(r.index + 1), r.sub, 0));  // other index
  rig.feed(response(node, 0x60, aam::kRegCommitIndex, aam::kRegCommitSub, 0));        // far away register
  EXPECT_EQ(rig.loader.progress(), 0U);
  EXPECT_EQ(rig.loader.status(), PlanLoader::Status::busy);
  rig.feed(good_answer(req));
  EXPECT_EQ(rig.loader.progress(), 1U);
}

TEST(Agent, AnswerFromAnotherNodeIsNotTakenForCurrentRequest) {
  IQK_VERIFIES("SRS-055");
  Rig rig(make_plan({1, 2}));
  const auto req = rig.next();
  const auto r = decode_req(req);
  const int node = req_node(req);
  const int other = node == 1 ? 2 : 1;
  rig.feed(response(other, 0x60, r.index, r.sub, 0));  // same register, wrong module
  EXPECT_EQ(rig.loader.progress(), 0U);
  rig.feed(good_answer(req));
  EXPECT_EQ(rig.loader.progress(), 1U);
}

TEST(Agent, WriteAckIsNotTakenAsAnswerToReadOfSameRegister) {
  IQK_VERIFIES("SRS-055");
  const auto plan = make_plan({1});
  Rig rig(plan);
  iqk_test::FakeModule mod;
  mod.node = 1;
  const auto reads_start = iqk::plan::registers(plan).size() + 1;  // writes + commit
  while (rig.loader.progress() < reads_start) {
    const auto req = rig.next();
    const auto ans = mod.on_frame(req);
    ASSERT_TRUE(ans.has_value());
    rig.feed(*ans);
  }
  const auto read_req = rig.next();  // first read-back: same register as the first write
  const auto r = decode_req(read_req);
  ASSERT_EQ(r.cmd, 0x40);
  rig.feed(response(1, 0x60, r.index, r.sub, 0));  // late write ack, same index/sub
  EXPECT_EQ(rig.loader.progress(), reads_start);
  EXPECT_EQ(rig.loader.status(), PlanLoader::Status::busy);
}

TEST(Agent, StaleReadValueDoesNotFailCurrentReadback) {
  IQK_VERIFIES("SRS-055");
  const auto plan = make_plan({1});
  Rig rig(plan);
  iqk_test::FakeModule mod;
  mod.node = 1;
  const auto regs = iqk::plan::registers(plan);
  const auto reads_start = regs.size() + 1;
  while (rig.loader.progress() < reads_start + 2) {
    const auto req = rig.next();
    const auto ans = mod.on_frame(req);
    ASSERT_TRUE(ans.has_value());
    if (rig.loader.progress() == reads_start + 1) {
      // Second read-back outstanding: a stale answer to the first read arrives with a bogus value.
      rig.feed(response(1, 0x43, regs[0].index, regs[0].sub, 0xDEADBEEF));
      EXPECT_EQ(rig.loader.status(), PlanLoader::Status::busy);
      EXPECT_EQ(rig.loader.progress(), reads_start + 1);
    }
    rig.feed(*ans);
  }
  EXPECT_EQ(rig.loader.status(), PlanLoader::Status::busy);
  EXPECT_EQ(rig.loader.failure(), PlanLoader::Failure::none);
}

TEST(Agent, StaleAbortFromEarlierRequestDoesNotFailCurrentRequest) {
  IQK_VERIFIES("SRS-055");
  Rig rig(make_plan({1}));
  const auto first = rig.next();
  rig.feed(good_answer(first));
  const auto second = rig.next();
  const auto f = decode_req(first);
  rig.feed(response(1, 0x80, f.index, f.sub, 0x06020000));  // abort belonging to the earlier request
  EXPECT_EQ(rig.loader.status(), PlanLoader::Status::busy);
  rig.feed(good_answer(second));
  EXPECT_EQ(rig.loader.progress(), 2U);
}

TEST(Agent, LoadCompletesWithReplayedStaleAnswersAndRetries) {
  IQK_VERIFIES("SRS-055");
  const auto plan = make_plan({1, 2});
  Rig rig(plan);
  iqk_test::FakeModule m1, m2;
  m1.node = 1;
  m2.node = 2;
  m1.ignore_next = 1;  // forces a retry of the first request
  std::optional<iqk::CanFrame> prev;
  int guard = 0;
  while (rig.loader.status() == PlanLoader::Status::busy && ++guard < 2000) {
    rig.now += 60 * iqk_test::kMs;  // beyond the request timeout, so lost requests are retried
    const auto req = rig.loader.poll(rig.now);
    if (!req) continue;
    auto& mod = req_node(*req) == 1 ? m1 : m2;
    const auto ans = mod.on_frame(*req);
    if (prev) {
      rig.loader.on_frame(*prev, rig.now);  // late answer to the previous request
    }
    if (ans) {
      rig.loader.on_frame(*ans, rig.now);
      prev = *ans;
      rig.loader.on_frame(*ans, rig.now);  // and immediately duplicated
    }
  }
  EXPECT_EQ(rig.loader.status(), PlanLoader::Status::done);
  EXPECT_EQ(rig.loader.failure(), PlanLoader::Failure::none);
  EXPECT_EQ(rig.loader.progress(), rig.loader.total());
}
