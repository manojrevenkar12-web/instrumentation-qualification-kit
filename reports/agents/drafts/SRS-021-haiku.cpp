#include "support.hpp"

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "iqk/config.hpp"
#include "iqk/drivers.hpp"
#include "iqk/faultlog.hpp"

namespace {

// Feeds IMU samples to a driver one frame at a time, 400 Hz apart on the local clock.
struct ImuRig {
  iqk::ImuDriver drv;
  iqk::FaultLog log;
  std::int64_t now{0};

  void push(std::uint16_t seq) {
    now += 2'500'000;  // 2.5 ms
    const std::vector<std::uint8_t> bytes =
        iqk_test::imu_frame(iqk_test::imu_sample(seq, static_cast<std::uint32_t>(now / 1000)));
    drv.on_bytes(bytes, now, log, [](const iqk::ImuReading&) {});
  }

  void push_all(std::initializer_list<std::uint16_t> seqs) {
    for (const std::uint16_t s : seqs) {
      push(s);
    }
  }

  [[nodiscard]] std::uint32_t gap_faults() const { return log.count(iqk::Fault::imu_gap); }

  // Detail (missing sample count) of each imu_gap record, oldest first.
  [[nodiscard]] std::vector<std::int64_t> gap_details() const {
    std::vector<std::int64_t> out;
    for (std::size_t i = 0; i < log.size(); ++i) {
      if (log[i].code == iqk::Fault::imu_gap) {
        out.push_back(log[i].detail);
      }
    }
    return out;
  }
};

}  // namespace

// --- Gaps are counted, including across the 16-bit wrap ---

TEST(ImuSampleCounter, ContiguousSequenceAcrossWrapCountsNoGap) {
  IQK_VERIFIES("SRS-021");
  ImuRig r;
  r.push_all({65533, 65534, 65535, 0, 1, 2});
  EXPECT_EQ(r.drv.gaps(), 0U);
  EXPECT_EQ(r.gap_faults(), 0U);
  EXPECT_EQ(r.log.size(), 0U);
  EXPECT_EQ(r.drv.samples(), 6U);
}

TEST(ImuSampleCounter, SingleMissingSampleAcrossWrapIsCounted) {
  IQK_VERIFIES("SRS-021");
  ImuRig r;
  r.push_all({65534, 0});  // 65535 missing
  EXPECT_EQ(r.drv.gaps(), 1U);
  EXPECT_EQ(r.gap_faults(), 0U);  // one sample is within the limit
}

TEST(ImuSampleCounter, MissingSamplesSpanningWrapAreCounted) {
  IQK_VERIFIES("SRS-021");
  ImuRig r;
  r.push_all({65534, 1});  // 65535, 0 missing
  EXPECT_EQ(r.drv.gaps(), 2U);
  EXPECT_EQ(r.gap_faults(), 0U);
}

TEST(ImuSampleCounter, GapsAccumulateAcrossSeveralWraps) {
  IQK_VERIFIES("SRS-021");
  ImuRig r;
  // Step of 2 for 70000 samples covers more than two wraps; each step loses exactly one sample.
  constexpr std::uint32_t kFrames = 70000;
  for (std::uint32_t i = 0; i < kFrames; ++i) {
    r.push(static_cast<std::uint16_t>(i * 2U));
  }
  EXPECT_EQ(r.drv.gaps(), kFrames - 1U);
  EXPECT_EQ(r.drv.samples(), kFrames);
  EXPECT_EQ(r.gap_faults(), 0U);
}

// --- Gaps longer than imu.max_gap_samples are logged as faults; the boundary is inclusive ---

TEST(ImuSampleCounter, GapAtMaxSamplesIsCountedButNotLogged) {
  IQK_VERIFIES("SRS-021");
  ASSERT_EQ(iqk::cfg::imu::max_gap_samples, 4) << "test assumes imu.max_gap_samples == 4";
  ImuRig r;
  r.push_all({100, 105});  // 101..104 missing: exactly max_gap_samples
  EXPECT_EQ(r.drv.gaps(), 4U);
  EXPECT_EQ(r.gap_faults(), 0U);
}

TEST(ImuSampleCounter, GapOneOverMaxSamplesIsLogged) {
  IQK_VERIFIES("SRS-021");
  ASSERT_EQ(iqk::cfg::imu::max_gap_samples, 4) << "test assumes imu.max_gap_samples == 4";
  ImuRig r;
  r.push_all({100, 106});  // 101..105 missing: one more than max_gap_samples
  EXPECT_EQ(r.drv.gaps(), 5U);
  EXPECT_EQ(r.gap_faults(), 1U);
  EXPECT_EQ(r.gap_details(), (std::vector<std::int64_t>{5}));
}

TEST(ImuSampleCounter, GapAtMaxSamplesAcrossWrapIsNotLogged) {
  IQK_VERIFIES("SRS-021");
  ImuRig r;
  r.push_all({65534, 3});  // 65535, 0, 1, 2 missing: 4 samples, across the wrap
  EXPECT_EQ(r.drv.gaps(), 4U);
  EXPECT_EQ(r.gap_faults(), 0U);
}

TEST(ImuSampleCounter, GapOneOverMaxSamplesAcrossWrapIsLogged) {
  IQK_VERIFIES("SRS-021");
  ImuRig r;
  r.push_all({65533, 3});  // 65534, 65535, 0, 1, 2 missing: 5 samples, across the wrap
  EXPECT_EQ(r.drv.gaps(), 5U);
  EXPECT_EQ(r.gap_faults(), 1U);
  EXPECT_EQ(r.gap_details(), (std::vector<std::int64_t>{5}));
}

TEST(ImuSampleCounter, LongGapAcrossWrapIsLoggedWithItsLength) {
  IQK_VERIFIES("SRS-021");
  ImuRig r;
  r.push_all({65520, 16});  // 31 samples missing across the wrap
  EXPECT_EQ(r.drv.gaps(), 31U);
  EXPECT_EQ(r.gap_faults(), 1U);
  EXPECT_EQ(r.gap_details(), (std::vector<std::int64_t>{31}));
}

TEST(ImuSampleCounter, EveryOversizedGapAcrossSeveralWrapsIsLogged) {
  IQK_VERIFIES("SRS-021");
  ImuRig r;
  // Step of 6 (five samples missing each time) for 25000 frames covers more than two wraps.
  constexpr std::uint32_t kFrames = 25000;
  for (std::uint32_t i = 0; i < kFrames; ++i) {
    r.push(static_cast<std::uint16_t>(i * 6U));
  }
  EXPECT_EQ(r.drv.gaps(), 5U * (kFrames - 1U));
  EXPECT_EQ(r.gap_faults(), kFrames - 1U);
}
