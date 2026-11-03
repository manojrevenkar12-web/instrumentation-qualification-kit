#include <gtest/gtest.h>

#include "fake_module.hpp"
#include "iqk/loader.hpp"
#include "support.hpp"

using iqk::PlanLoader;
using iqk_test::FakeModule;
using iqk_test::kMs;

namespace {
iqk::plan::Plan good_plan() {
  std::vector<iqk::plan::Finding> f;
  return *iqk::plan::parse(
      "iqk-plan 1\ncampaign L\nchannel a aam:1:0 rate=500 range_mv=10000 filter=lp_100hz\n"
      "channel b aam:1:4 rate=10 range_mv=100\n",
      f);
}

// Runs the loader against the module until it settles or the step budget is spent; 1 ms per step.
PlanLoader::Status run(PlanLoader& l, FakeModule& m, std::int64_t& now, int steps = 20000) {
  for (int i = 0; i < steps && l.status() == PlanLoader::Status::busy; ++i) {
    if (const auto f = l.poll(now)) {
      if (const auto a = m.on_frame(*f)) {
        l.on_frame(*a, now + kMs / 2);
      }
    }
    now += kMs;
  }
  return l.status();
}
}  // namespace

TEST(Loader, AGoodModuleIsConfiguredVerifiedAndCrcChecked) {
  IQK_VERIFIES("SRS-052", "SRS-053");
  PlanLoader l;
  FakeModule m;
  std::int64_t now = 0;
  l.start(good_plan(), now);
  ASSERT_EQ(run(l, m, now), PlanLoader::Status::done);
  EXPECT_EQ(m.applied[0].range, 3U);
  EXPECT_EQ(m.applied[4].divisor, 100U);
  // 48 writes, 1 commit, 48 reads, 1 CRC read
  EXPECT_EQ(m.requests, 98);
  EXPECT_EQ(l.total(), 98U);
}

TEST(Loader, AnUnansweredRequestIsRetriedThreeTimesThenFails) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  FakeModule m;
  m.ignore_next = 1000;
  std::int64_t now = 0;
  l.start(good_plan(), now);
  ASSERT_EQ(run(l, m, now), PlanLoader::Status::failed);
  EXPECT_EQ(l.failure(), PlanLoader::Failure::timeout);
  EXPECT_EQ(m.requests, 4);  // the first try and three retries
}

TEST(Loader, ALostAnswerIsRecoveredByARetry) {
  IQK_VERIFIES("SRS-050");
  PlanLoader l;
  FakeModule m;
  m.ignore_next = 3;
  std::int64_t now = 0;
  l.start(good_plan(), now);
  EXPECT_EQ(run(l, m, now), PlanLoader::Status::done);
  EXPECT_EQ(m.requests, 98 + 3);
}

TEST(Loader, AnAbortFailsTheLoad) {
  IQK_VERIFIES("SRS-051");
  PlanLoader l;
  FakeModule m;
  m.abort_all = true;
  std::int64_t now = 0;
  l.start(good_plan(), now);
  ASSERT_EQ(run(l, m, now), PlanLoader::Status::failed);
  EXPECT_EQ(l.failure(), PlanLoader::Failure::abort);
  EXPECT_EQ(l.detail(), 0x06020000U);
  EXPECT_EQ(m.requests, 1);
}

TEST(Loader, AReadBackMismatchFailsTheLoad) {
  IQK_VERIFIES("SRS-052");
  PlanLoader l;
  FakeModule m;
  m.corrupt_range_readback = true;
  std::int64_t now = 0;
  l.start(good_plan(), now);
  ASSERT_EQ(run(l, m, now), PlanLoader::Status::failed);
  EXPECT_EQ(l.failure(), PlanLoader::Failure::readback_mismatch);
}

TEST(Loader, AWrongModuleCrcFailsTheLoad) {
  IQK_VERIFIES("SRS-053");
  PlanLoader l;
  FakeModule m;
  m.wrong_crc = true;
  std::int64_t now = 0;
  l.start(good_plan(), now);
  ASSERT_EQ(run(l, m, now), PlanLoader::Status::failed);
  EXPECT_EQ(l.failure(), PlanLoader::Failure::crc_mismatch);
}

TEST(Loader, ALateAnswerToAnEarlierRequestIsNotTakenForTheCurrentOne) {
  IQK_VERIFIES("SRS-055");
  PlanLoader l;
  FakeModule m;
  std::int64_t now = 0;
  l.start(good_plan(), now);
  const auto first = l.poll(now);
  ASSERT_TRUE(first);
  const auto answer_first = m.on_frame(*first);  // held back: arrives late
  now += 60 * kMs;                               // timed out
  const auto retry = l.poll(now);
  ASSERT_TRUE(retry);
  const auto answer_retry = m.on_frame(*retry);
  l.on_frame(*answer_retry, now);  // first op done; the loader now waits for op 2
  const auto second = l.poll(now);
  ASSERT_TRUE(second);
  const std::size_t before = l.progress();
  l.on_frame(*answer_first, now);  // the stale ack for op 1 must not complete op 2
  EXPECT_EQ(l.progress(), before);
  l.on_frame(*m.on_frame(*second), now);
  EXPECT_EQ(l.progress(), before + 1);
}
