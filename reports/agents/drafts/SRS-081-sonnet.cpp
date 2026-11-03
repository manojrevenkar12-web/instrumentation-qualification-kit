#include "support.hpp"

#include "iqk/faultlog.hpp"

namespace {
using iqk::Fault;
using iqk::FaultLog;
using namespace iqk_test;

// Logs n non-foldable records (same code, 2 s apart). detail = index.
void fill(FaultLog& l, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) {
    l.log(Fault::imu_gap, static_cast<std::int64_t>(i) * 2 * kS, static_cast<std::int64_t>(i));
  }
}
}  // namespace

TEST(FaultLogSRS081, CapacityMatchesConfig) {
  IQK_VERIFIES("SRS-081");
  EXPECT_EQ(FaultLog::kCapacity, 256U);
  EXPECT_EQ(FaultLog::kCapacity, static_cast<std::size_t>(iqk::cfg::diag::fault_log_capacity));
}

TEST(FaultLogSRS081, FillToCapacityKeepsAllWithoutOverwrite) {
  IQK_VERIFIES("SRS-081");
  FaultLog l;
  fill(l, FaultLog::kCapacity);
  EXPECT_EQ(l.size(), FaultLog::kCapacity);
  EXPECT_EQ(l.overwritten(), 0U);
  EXPECT_EQ(l.total(), FaultLog::kCapacity);
  EXPECT_EQ(l[0].detail, 0);
  EXPECT_EQ(l[FaultLog::kCapacity - 1].detail, static_cast<std::int64_t>(FaultLog::kCapacity - 1));
}

TEST(FaultLogSRS081, OneBeyondCapacityOverwritesOldestAndCounts) {
  IQK_VERIFIES("SRS-081");
  FaultLog l;
  fill(l, FaultLog::kCapacity + 1);
  EXPECT_EQ(l.size(), FaultLog::kCapacity);
  EXPECT_EQ(l.overwritten(), 1U);
  EXPECT_EQ(l.total(), FaultLog::kCapacity + 1);
  EXPECT_EQ(l[0].detail, 1);
  EXPECT_EQ(l[FaultLog::kCapacity - 1].detail, static_cast<std::int64_t>(FaultLog::kCapacity));
}

TEST(FaultLogSRS081, ManyOverwritesStayBoundedOldestFirstOrder) {
  IQK_VERIFIES("SRS-081");
  FaultLog l;
  const std::size_t extra = FaultLog::kCapacity * 2 + 7;
  fill(l, FaultLog::kCapacity + extra);
  EXPECT_EQ(l.size(), FaultLog::kCapacity);
  EXPECT_EQ(l.overwritten(), extra);
  for (std::size_t i = 0; i < l.size(); ++i) {
    EXPECT_EQ(l[i].detail, static_cast<std::int64_t>(extra + i));
    EXPECT_EQ(l[i].count, 1U);
  }
}

TEST(FaultLogSRS081, RepeatWithinOneSecondIsFolded) {
  IQK_VERIFIES("SRS-081");
  FaultLog l;
  l.log(Fault::imu_crc, 5 * kS, 1);
  l.log(Fault::imu_crc, 5 * kS + 500 * kMs, 2);
  l.log(Fault::imu_crc, 5 * kS + 999 * kMs, 3);
  ASSERT_EQ(l.size(), 1U);
  EXPECT_EQ(l[0].count, 3U);
  EXPECT_EQ(l[0].detail, 3);
  EXPECT_EQ(l[0].local_ns, 5 * kS);
  EXPECT_EQ(l.total(), 3U);
  EXPECT_EQ(l.count(Fault::imu_crc), 3U);
  EXPECT_EQ(l.overwritten(), 0U);
}

TEST(FaultLogSRS081, RepeatExactlyOneSecondIsFolded) {
  IQK_VERIFIES("SRS-081");
  FaultLog l;
  l.log(Fault::imu_crc, 5 * kS, 1);
  l.log(Fault::imu_crc, 6 * kS, 2);
  ASSERT_EQ(l.size(), 1U);
  EXPECT_EQ(l[0].count, 2U);
}

TEST(FaultLogSRS081, RepeatAfterMoreThanOneSecondIsNewRecord) {
  IQK_VERIFIES("SRS-081");
  FaultLog l;
  l.log(Fault::imu_crc, 5 * kS, 1);
  l.log(Fault::imu_crc, 6 * kS + 1, 2);
  ASSERT_EQ(l.size(), 2U);
  EXPECT_EQ(l[0].count, 1U);
  EXPECT_EQ(l[1].count, 1U);
  EXPECT_EQ(l[1].detail, 2);
}

TEST(FaultLogSRS081, DifferentCodesAreNotFolded) {
  IQK_VERIFIES("SRS-081");
  FaultLog l;
  l.log(Fault::imu_crc, kS, 1);
  l.log(Fault::imu_gap, kS, 2);
  l.log(Fault::imu_crc, kS + 10 * kMs, 3);  // folds into the first record, not the latest
  ASSERT_EQ(l.size(), 2U);
  EXPECT_EQ(l[0].code, Fault::imu_crc);
  EXPECT_EQ(l[0].count, 2U);
  EXPECT_EQ(l[1].code, Fault::imu_gap);
  EXPECT_EQ(l[1].count, 1U);
}

TEST(FaultLogSRS081, FoldWindowIsAnchoredAtFirstOccurrence) {
  IQK_VERIFIES("SRS-081");
  FaultLog l;
  l.log(Fault::aam_gap, 0, 0);
  l.log(Fault::aam_gap, 800 * kMs, 0);   // folded
  l.log(Fault::aam_gap, 1600 * kMs, 0);  // >1 s after record start: new record
  ASSERT_EQ(l.size(), 2U);
  EXPECT_EQ(l[0].count, 2U);
  EXPECT_EQ(l[1].count, 1U);
}

TEST(FaultLogSRS081, FoldedBurstDoesNotConsumeCapacityOrOverwrite) {
  IQK_VERIFIES("SRS-081");
  FaultLog l;
  fill(l, FaultLog::kCapacity);
  const std::int64_t last_t = static_cast<std::int64_t>(FaultLog::kCapacity - 1) * 2 * kS;
  for (int i = 0; i < 1000; ++i) {
    l.log(Fault::imu_gap, last_t + 1 * kMs, 99);
  }
  EXPECT_EQ(l.size(), FaultLog::kCapacity);
  EXPECT_EQ(l.overwritten(), 0U);
  EXPECT_EQ(l[0].detail, 0);
  EXPECT_EQ(l[FaultLog::kCapacity - 1].count, 1001U);
  EXPECT_EQ(l.total(), FaultLog::kCapacity + 1000U);
}

TEST(FaultLogSRS081, FullLogNewDistinctFaultOverwritesOldestOnce) {
  IQK_VERIFIES("SRS-081");
  FaultLog l;
  fill(l, FaultLog::kCapacity);
  l.log(Fault::imu_crc, 10000 * kS, 7);
  EXPECT_EQ(l.size(), FaultLog::kCapacity);
  EXPECT_EQ(l.overwritten(), 1U);
  EXPECT_EQ(l[FaultLog::kCapacity - 1].code, Fault::imu_crc);
  EXPECT_EQ(l[0].detail, 1);
}
