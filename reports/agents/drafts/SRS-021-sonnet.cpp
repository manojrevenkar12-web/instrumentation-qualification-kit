#include "support.hpp"

#include "iqk/drivers.hpp"

namespace {

using iqk::Fault;

struct Rig {
  iqk::ImuDriver drv;
  iqk::FaultLog log;
  std::int64_t now{0};
  int readings{0};

  void feed(std::uint16_t seq) {
    now += 2 * iqk_test::kS;  // spaced so the fault log never folds records
    const auto f = iqk_test::imu_frame(iqk_test::imu_sample(seq, static_cast<std::uint32_t>(now / 1000)));
    drv.on_bytes(f, now, log, [&](const iqk::ImuReading&) { ++readings; });
  }
  void feed_all(std::initializer_list<std::uint16_t> seqs) {
    for (auto s : seqs) feed(s);
  }
};

TEST(ImuGaps, ContiguousSamplesHaveNoGaps) {
  IQK_VERIFIES("SRS-021");
  Rig r;
  r.feed_all({100, 101, 102, 103});
  EXPECT_EQ(r.drv.gaps(), 0U);
  EXPECT_EQ(r.log.count(Fault::imu_gap), 0U);
  EXPECT_EQ(r.readings, 4);
}

TEST(ImuGaps, SingleLostSampleIsCountedButNotFaulted) {
  IQK_VERIFIES("SRS-021");
  Rig r;
  r.feed_all({100, 102});
  EXPECT_EQ(r.drv.gaps(), 1U);
  EXPECT_EQ(r.log.count(Fault::imu_gap), 0U);
}

TEST(ImuGaps, GapsAccumulateAcrossEvents) {
  IQK_VERIFIES("SRS-021");
  Rig r;
  r.feed_all({10, 13, 14, 16, 20});  // missing 2, 0, 1, 3
  EXPECT_EQ(r.drv.gaps(), 6U);
  EXPECT_EQ(r.log.count(Fault::imu_gap), 0U);
}

TEST(ImuGaps, GapOfExactlyMaxIsNotFaulted) {
  IQK_VERIFIES("SRS-021");
  Rig r;
  r.feed_all({100, 105});  // 4 missing == max_gap_samples
  EXPECT_EQ(r.drv.gaps(), 4U);
  EXPECT_EQ(r.log.count(Fault::imu_gap), 0U);
}

TEST(ImuGaps, GapOneOverMaxIsLoggedWithMissingCount) {
  IQK_VERIFIES("SRS-021");
  Rig r;
  r.feed_all({100, 106});  // 5 missing
  EXPECT_EQ(r.drv.gaps(), 5U);
  ASSERT_EQ(r.log.count(Fault::imu_gap), 1U);
  ASSERT_EQ(r.log.size(), 1U);
  EXPECT_EQ(r.log[0].code, Fault::imu_gap);
  EXPECT_EQ(r.log[0].detail, 5);
}

TEST(ImuGaps, LargeGapIsLoggedOncePerGap) {
  IQK_VERIFIES("SRS-021");
  Rig r;
  r.feed_all({0, 1000, 1001, 2001});
  EXPECT_EQ(r.drv.gaps(), 999U + 999U);
  EXPECT_EQ(r.log.count(Fault::imu_gap), 2U);
}

TEST(ImuGaps, ContiguousAcrossWrapHasNoGap) {
  IQK_VERIFIES("SRS-021");
  Rig r;
  r.feed_all({65534, 65535, 0, 1});
  EXPECT_EQ(r.drv.gaps(), 0U);
  EXPECT_EQ(r.log.count(Fault::imu_gap), 0U);
}

TEST(ImuGaps, LostSampleAtWrapIsCounted) {
  IQK_VERIFIES("SRS-021");
  Rig r;
  r.feed_all({65535, 1});  // sample 0 lost
  EXPECT_EQ(r.drv.gaps(), 1U);
  EXPECT_EQ(r.log.count(Fault::imu_gap), 0U);
}

TEST(ImuGaps, GapSpanningWrapAtMaxIsCountedNotFaulted) {
  IQK_VERIFIES("SRS-021");
  Rig r;
  r.feed_all({65533, 2});  // 65534, 65535, 0, 1 missing = 4
  EXPECT_EQ(r.drv.gaps(), 4U);
  EXPECT_EQ(r.log.count(Fault::imu_gap), 0U);
}

TEST(ImuGaps, GapSpanningWrapOverMaxIsFaulted) {
  IQK_VERIFIES("SRS-021");
  Rig r;
  r.feed_all({65533, 3});  // 5 missing
  EXPECT_EQ(r.drv.gaps(), 5U);
  ASSERT_EQ(r.log.count(Fault::imu_gap), 1U);
  EXPECT_EQ(r.log[0].detail, 5);
}

}  // namespace
