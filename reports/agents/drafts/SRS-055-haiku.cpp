// SRS-055: an answer to an earlier request is never taken as the answer to the current one.
//
// Exercised through the plan loader, which sends one register request at a time and waits for its answer. The
// answers are matched on module (CAN id), register index, sub-index and command, and only while a request is
// outstanding. The tests deliver answers late, early, from another module, or to an earlier register, and check
// that the loader neither advances nor fails because of them.
#include <cstdint>
#include <map>
#include <optional>
#include <string_view>
#include <vector>

#include "fake_module.hpp"
#include "iqk/icd/aam.hpp"
#include "iqk/loader.hpp"
#include "iqk/plan.hpp"
#include "support.hpp"

namespace {

namespace aam = iqk::icd::aam;
using iqk::CanFrame;
using iqk::PlanLoader;
using iqk_test::FakeModule;
using iqk_test::kMs;

constexpr std::uint8_t kReadCmd = 0x40;
constexpr std::uint8_t kReadValueCmd = 0x43;
constexpr std::uint8_t kWriteAckCmd = 0x60;

constexpr std::string_view kOneModule = "iqk-plan 1\ncampaign srs055\nchannel a aam:1:0 rate=100 range_mv=1000\n";
constexpr std::string_view kTwoModules =
    "iqk-plan 1\ncampaign srs055\n"
    "channel a aam:1:0 rate=100 range_mv=1000\n"
    "channel b aam:2:0 rate=100 range_mv=1000\n";

iqk::plan::Plan plan_from(std::string_view text) {
  std::vector<iqk::plan::Finding> findings;
  auto p = iqk::plan::parse(text, findings);
  if (!p) {
    ADD_FAILURE() << "the test plan does not parse";
    return {};
  }
  return *p;
}

aam::AamRegRequest request_of(const CanFrame& f) {
  aam::AamRegRequest r{};
  (void)aam::decode(f.payload(), r);
  return r;
}

int node_of(const CanFrame& f) { return static_cast<int>(f.id - aam::AamRegRequest::kId); }

// An answer as a module sends it, built by hand for answers that arrive late or out of turn.
CanFrame response(int node, std::uint8_t cmd, std::uint16_t index, std::uint8_t sub, std::uint32_t value) {
  aam::AamRegResponse r{};
  r.cmd = cmd;
  r.index = index;
  r.sub = sub;
  r.value = value;
  CanFrame f{};
  f.id = aam::AamRegResponse::can_id(static_cast<std::uint8_t>(node));
  f.dlc = static_cast<std::uint8_t>(aam::encode(r, f.data));
  return f;
}

// A loader with the modules of the plan on the other end of the bus.
struct Bench {
  PlanLoader loader;
  std::map<int, FakeModule> modules;
  std::int64_t now{0};

  // The answer the module addressed by the request sends.
  CanFrame answer(const CanFrame& req) {
    const int node = node_of(req);
    FakeModule& m = modules[node];
    m.node = node;
    return m.on_frame(req).value();
  }

  // Polls the loader and answers each request in turn, until a request matches. That request is left
  // unanswered, so the test decides what arrives next.
  template <class Stop>
  CanFrame send_until(Stop stop) {
    for (int i = 0; i < 100000; ++i) {
      const auto req = loader.poll(now);
      if (!req) {
        ADD_FAILURE() << "the loader stopped before a matching request";
        return {};
      }
      if (stop(*req)) {
        return *req;
      }
      loader.on_frame(answer(*req), now);
    }
    ADD_FAILURE() << "no matching request";
    return {};
  }
};

bool same_register(const CanFrame& a, const CanFrame& b) {
  const auto ra = request_of(a);
  const auto rb = request_of(b);
  return node_of(a) == node_of(b) && ra.index == rb.index && ra.sub == rb.sub;
}

}  // namespace

TEST(LateAnswers, LateAckForEarlierWriteDoesNotCompleteCurrentWrite) {
  IQK_VERIFIES("SRS-055");
  Bench b;
  b.loader.start(plan_from(kOneModule), b.now);

  const CanFrame first = *b.loader.poll(b.now);
  const CanFrame first_ans = b.answer(first);
  b.loader.on_frame(first_ans, b.now);
  ASSERT_EQ(b.loader.progress(), 1u);

  const CanFrame second = *b.loader.poll(b.now);
  ASSERT_FALSE(same_register(first, second)) << "the next request must address another register";

  // The answer to the earlier write arrives again while the next write is outstanding.
  b.loader.on_frame(first_ans, b.now);
  EXPECT_EQ(b.loader.progress(), 1u);
  EXPECT_EQ(b.loader.status(), PlanLoader::Status::busy);
  EXPECT_EQ(b.loader.failure(), PlanLoader::Failure::none);
  EXPECT_FALSE(b.loader.poll(b.now)) << "the loader must keep waiting for its own answer, not resend";

  b.loader.on_frame(b.answer(second), b.now);
  EXPECT_EQ(b.loader.progress(), 2u);
}

TEST(LateAnswers, AnswerThatArrivesBeforeItsRequestIsSentIsIgnored) {
  IQK_VERIFIES("SRS-055");
  Bench b;
  const auto plan = plan_from(kOneModule);
  b.loader.start(plan, b.now);
  const CanFrame first = *b.loader.poll(b.now);
  const CanFrame old_answer = b.answer(first);
  b.loader.on_frame(old_answer, b.now);
  ASSERT_EQ(b.loader.progress(), 1u);

  // A new run of the same plan starts. The answer to the previous run's request for this register arrives
  // before the new run has sent anything.
  b.loader.start(plan, b.now);
  b.loader.on_frame(old_answer, b.now);
  EXPECT_EQ(b.loader.progress(), 0u);
  EXPECT_EQ(b.loader.status(), PlanLoader::Status::busy);

  const CanFrame again = *b.loader.poll(b.now);
  EXPECT_TRUE(same_register(first, again));
  EXPECT_EQ(b.loader.progress(), 0u);
}

TEST(LateAnswers, AnswerFromAnotherModuleForTheSameRegisterIsNotTakenForCurrent) {
  IQK_VERIFIES("SRS-055");
  Bench b;
  b.loader.start(plan_from(kTwoModules), b.now);

  // Module 1's answer to its first write, which addresses the same register as module 2's first write.
  const CanFrame first = *b.loader.poll(b.now);
  const CanFrame first_ans = b.answer(first);
  b.loader.on_frame(first_ans, b.now);
  ASSERT_EQ(b.loader.progress(), 1u);

  const auto first_reg = request_of(first);
  const CanFrame other = b.send_until([&](const CanFrame& f) {
    const auto r = request_of(f);
    return node_of(f) == 2 && r.index == first_reg.index && r.sub == first_reg.sub;
  });
  ASSERT_EQ(node_of(other), 2);
  const std::size_t before = b.loader.progress();

  b.loader.on_frame(first_ans, b.now);
  EXPECT_EQ(b.loader.progress(), before);
  EXPECT_EQ(b.loader.status(), PlanLoader::Status::busy);
  EXPECT_FALSE(b.loader.poll(b.now)) << "module 2's request must still be outstanding";

  b.loader.on_frame(b.answer(other), b.now);
  EXPECT_EQ(b.loader.progress(), before + 1);
}

TEST(LateAnswers, LateReadValueOfEarlierReadIsNotTakenForCurrentRead) {
  IQK_VERIFIES("SRS-055");
  Bench b;
  b.loader.start(plan_from(kOneModule), b.now);

  const CanFrame read0 = b.send_until([](const CanFrame& f) { return request_of(f).cmd == kReadCmd; });
  b.loader.on_frame(b.answer(read0), b.now);
  const CanFrame read1 = b.send_until([](const CanFrame& f) { return request_of(f).cmd == kReadCmd; });
  ASSERT_FALSE(same_register(read0, read1));
  const std::size_t before = b.loader.progress();

  // A value answered for the earlier read, which does not match what the current register should hold.
  const auto r0 = request_of(read0);
  b.loader.on_frame(response(node_of(read0), kReadValueCmd, r0.index, r0.sub, 0xDEADBEEFU), b.now);
  EXPECT_EQ(b.loader.progress(), before);
  EXPECT_EQ(b.loader.status(), PlanLoader::Status::busy);
  EXPECT_EQ(b.loader.failure(), PlanLoader::Failure::none);

  b.loader.on_frame(b.answer(read1), b.now);
  EXPECT_EQ(b.loader.progress(), before + 1);
}

TEST(LateAnswers, LateWriteAckForRegisterIsNotTakenAsReadValueOfSameRegister) {
  IQK_VERIFIES("SRS-055");
  Bench b;
  b.loader.start(plan_from(kOneModule), b.now);

  // The first read addresses the register the first write configured; that write's acknowledgement is late.
  const CanFrame read0 = b.send_until([](const CanFrame& f) { return request_of(f).cmd == kReadCmd; });
  const auto r0 = request_of(read0);
  const CanFrame real = b.answer(read0);
  aam::AamRegResponse real_fields{};
  (void)aam::decode(real.payload(), real_fields);
  ASSERT_NE(real_fields.value, 0u) << "the register must hold a non-zero value for this test to mean anything";

  const std::size_t before = b.loader.progress();
  b.loader.on_frame(response(node_of(read0), kWriteAckCmd, r0.index, r0.sub, 0), b.now);
  EXPECT_EQ(b.loader.progress(), before);
  EXPECT_EQ(b.loader.status(), PlanLoader::Status::busy);
  EXPECT_EQ(b.loader.failure(), PlanLoader::Failure::none);

  b.loader.on_frame(real, b.now);
  EXPECT_EQ(b.loader.progress(), before + 1);
}

TEST(LateAnswers, AnswerArrivingAfterTimeoutFailureDoesNotRevive) {
  IQK_VERIFIES("SRS-055");
  Bench b;
  b.loader.start(plan_from(kOneModule), 0);

  const CanFrame first = *b.loader.poll(0);
  const CanFrame late = b.answer(first);  // answered too late: the loader never sees it in time
  ASSERT_TRUE(b.loader.poll(50 * kMs));
  ASSERT_TRUE(b.loader.poll(100 * kMs));
  ASSERT_TRUE(b.loader.poll(150 * kMs));
  ASSERT_FALSE(b.loader.poll(200 * kMs));
  ASSERT_EQ(b.loader.status(), PlanLoader::Status::failed);
  ASSERT_EQ(b.loader.failure(), PlanLoader::Failure::timeout);

  b.loader.on_frame(late, 210 * kMs);
  EXPECT_EQ(b.loader.status(), PlanLoader::Status::failed);
  EXPECT_EQ(b.loader.failure(), PlanLoader::Failure::timeout);
  EXPECT_EQ(b.loader.progress(), 0u);
}

TEST(LateAnswers, DuplicateAnswerToRetriedRequestIsNotTakenForNextRegister) {
  IQK_VERIFIES("SRS-055");
  Bench b;
  b.loader.start(plan_from(kOneModule), 0);

  const CanFrame first = *b.loader.poll(0);
  const CanFrame first_ans = b.answer(first);  // the first attempt's answer, delayed
  EXPECT_FALSE(b.loader.poll(50 * kMs - 1)) << "no retransmission before the timeout";

  const auto retry = b.loader.poll(50 * kMs);  // timeout reached: the same request is sent again
  ASSERT_TRUE(retry);
  ASSERT_TRUE(same_register(first, *retry));
  const CanFrame retry_ans = b.answer(*retry);
  b.loader.on_frame(retry_ans, 55 * kMs);
  ASSERT_EQ(b.loader.progress(), 1u);

  const auto next = b.loader.poll(55 * kMs);
  ASSERT_TRUE(next);
  ASSERT_FALSE(same_register(first, *next));

  // The earlier attempt's answer finally arrives, while the next register is outstanding.
  b.loader.on_frame(first_ans, 60 * kMs);
  EXPECT_EQ(b.loader.progress(), 1u);
  EXPECT_EQ(b.loader.status(), PlanLoader::Status::busy);
  EXPECT_FALSE(b.loader.poll(60 * kMs));

  b.loader.on_frame(b.answer(*next), 61 * kMs);
  EXPECT_EQ(b.loader.progress(), 2u);
}
