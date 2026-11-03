// SRS-081 Bounded fault log: capacity is diag.fault_log_capacity; repeats within one second are folded into one
// record; when full, the oldest record is overwritten and counted.
#include <cstddef>
#include <cstdint>

#include "iqk/config.hpp"
#include "iqk/faultlog.hpp"
#include "support.hpp"

namespace {

using iqk::Fault;
using iqk::FaultLog;
using iqk_test::kMs;
using iqk_test::kS;

constexpr std::size_t kCap = FaultLog::kCapacity;
constexpr std::int64_t kApart = 2 * kS;  // spacing that keeps records of one code from folding into each other

std::int64_t time_of(std::size_t i) { return static_cast<std::int64_t>(i) * kApart; }

// Logs records first .. first+n-1 of one fault code, record i at time_of(i) with detail i.
void log_spaced(FaultLog& log, std::size_t first, std::size_t n, Fault code) {
  for (std::size_t i = first; i < first + n; ++i) {
    log.log(code, time_of(i), static_cast<std::int64_t>(i));
  }
}

}  // namespace

TEST(Srs081FaultLog, CapacityMatchesConfiguredValue) {
  IQK_VERIFIES("SRS-081");
  EXPECT_EQ(static_cast<std::int64_t>(kCap), iqk::cfg::diag::fault_log_capacity);
  EXPECT_EQ(kCap, 256U);
}

TEST(Srs081FaultLog, HoldsCapacityRecordsWithoutEviction) {
  IQK_VERIFIES("SRS-081");
  FaultLog log;
  log_spaced(log, 0, kCap, Fault::imu_gap);
  EXPECT_EQ(log.size(), kCap);
  EXPECT_EQ(log.overwritten(), 0U);
  EXPECT_EQ(log.total(), kCap);
  EXPECT_EQ(log[0].local_ns, time_of(0));
  EXPECT_EQ(log[kCap - 1].local_ns, time_of(kCap - 1));
}

TEST(Srs081FaultLog, OneMoreRecordOverwritesOldestAndCountsIt) {
  IQK_VERIFIES("SRS-081");
  FaultLog log;
  log_spaced(log, 0, kCap + 1, Fault::imu_gap);
  EXPECT_EQ(log.size(), kCap);
  EXPECT_EQ(log.overwritten(), 1U);
  EXPECT_EQ(log.total(), kCap + 1);
  EXPECT_EQ(log[0].local_ns, time_of(1));  // record 0 is gone, record 1 is now the oldest
  EXPECT_EQ(log[0].detail, 1);
  EXPECT_EQ(log[kCap - 1].local_ns, time_of(kCap));
}

TEST(Srs081FaultLog, EvictionKeepsTheNewestCapacityRecordsInOrderAcrossWraps) {
  IQK_VERIFIES("SRS-081");
  constexpr std::size_t kN = 3 * kCap + 7;
  FaultLog log;
  log_spaced(log, 0, kN, Fault::imu_gap);
  ASSERT_EQ(log.size(), kCap);
  EXPECT_EQ(log.overwritten(), kN - kCap);
  EXPECT_EQ(log.total(), kN);
  for (std::size_t i = 0; i < kCap; ++i) {
    EXPECT_EQ(log[i].local_ns, time_of(kN - kCap + i)) << "index " << i;
    EXPECT_EQ(log[i].count, 1U) << "index " << i;
  }
}

TEST(Srs081FaultLog, EvictedRecordIsNoLongerCounted) {
  IQK_VERIFIES("SRS-081");
  FaultLog log;
  log.log(Fault::aam_lost, 0, 1);  // oldest record; overwritten by the fill below
  log_spaced(log, 1, kCap, Fault::imu_gap);
  EXPECT_EQ(log.overwritten(), 1U);
  EXPECT_EQ(log.count(Fault::aam_lost), 0U);
  EXPECT_EQ(log.count(Fault::imu_gap), kCap);
}

TEST(Srs081FaultLog, RepeatsWithinOneSecondAreFolded) {
  IQK_VERIFIES("SRS-081");
  FaultLog log;
  log.log(Fault::imu_gap, 0, 1);
  log.log(Fault::imu_gap, kS / 4, 2);
  log.log(Fault::imu_gap, kS / 2, 3);
  log.log(Fault::imu_gap, 3 * kS / 4, 4);
  log.log(Fault::imu_gap, kS, 5);  // exactly one second after the first sighting: still folded
  EXPECT_EQ(log.size(), 1U);
  EXPECT_EQ(log[0].count, 5U);
  EXPECT_EQ(log[0].local_ns, 0);
  EXPECT_EQ(log.count(Fault::imu_gap), 5U);
  EXPECT_EQ(log.total(), 5U);
  EXPECT_EQ(log.overwritten(), 0U);
}

TEST(Srs081FaultLog, RepeatJustOverOneSecondIsANewRecord) {
  IQK_VERIFIES("SRS-081");
  FaultLog log;
  log.log(Fault::imu_gap, 0, 1);
  log.log(Fault::imu_gap, kS + 1, 2);  // one nanosecond beyond the fold window
  ASSERT_EQ(log.size(), 2U);
  EXPECT_EQ(log[0].count, 1U);
  EXPECT_EQ(log[1].count, 1U);
  EXPECT_EQ(log[1].local_ns, kS + 1);
  EXPECT_EQ(log.overwritten(), 0U);
}

TEST(Srs081FaultLog, FoldWindowRunsFromFirstSightingNotFromLastRepeat) {
  IQK_VERIFIES("SRS-081");
  // docs/icd/tlm.md: a record's local_ms is when the fault was first seen, so the window is measured from there.
  FaultLog log;
  log.log(Fault::imu_gap, 0, 1);
  log.log(Fault::imu_gap, 6 * kS / 10, 2);   // 0.6 s: folded
  log.log(Fault::imu_gap, 12 * kS / 10, 3);  // 1.2 s after the first sighting: a new record
  ASSERT_EQ(log.size(), 2U);
  EXPECT_EQ(log[0].count, 2U);
  EXPECT_EQ(log[0].local_ns, 0);
  EXPECT_EQ(log[1].count, 1U);
  EXPECT_EQ(log[1].local_ns, 12 * kS / 10);
}

TEST(Srs081FaultLog, DifferentFaultsAtTheSameTimeAreNotFolded) {
  IQK_VERIFIES("SRS-081");
  FaultLog log;
  log.log(Fault::imu_gap, 0, 1);
  log.log(Fault::imu_crc, 0, 2);
  ASSERT_EQ(log.size(), 2U);
  EXPECT_EQ(log[0].count, 1U);
  EXPECT_EQ(log[1].count, 1U);
  EXPECT_EQ(log.count(Fault::imu_gap), 1U);
  EXPECT_EQ(log.count(Fault::imu_crc), 1U);
}

TEST(Srs081FaultLog, RepeatFoldsAcrossAnInterleavedOtherFault) {
  IQK_VERIFIES("SRS-081");
  FaultLog log;
  log.log(Fault::imu_gap, 0, 1);
  log.log(Fault::imu_crc, kS / 10, 2);
  log.log(Fault::imu_gap, 2 * kS / 10, 3);  // repeat of the first record, within one second
  ASSERT_EQ(log.size(), 2U);
  EXPECT_EQ(log[0].code, Fault::imu_gap);
  EXPECT_EQ(log[0].count, 2U);
  EXPECT_EQ(log[1].code, Fault::imu_crc);
  EXPECT_EQ(log[1].count, 1U);
  EXPECT_EQ(log.total(), 3U);
}

TEST(Srs081FaultLog, FoldedRepeatDoesNotEvictWhenFull) {
  IQK_VERIFIES("SRS-081");
  FaultLog log;
  log_spaced(log, 0, kCap, Fault::imu_gap);  // full, newest is record kCap-1
  log.log(Fault::imu_gap, time_of(kCap - 1) + kS / 2, 99);  // repeat of the newest, within one second
  EXPECT_EQ(log.size(), kCap);
  EXPECT_EQ(log.overwritten(), 0U);
  EXPECT_EQ(log.total(), kCap + 1);
  EXPECT_EQ(log[0].local_ns, time_of(0));  // oldest still present
  EXPECT_EQ(log[kCap - 1].count, 2U);
}

TEST(Srs081FaultLog, FoldStillWorksAfterWraparound) {
  IQK_VERIFIES("SRS-081");
  FaultLog log;
  log_spaced(log, 0, kCap + 5, Fault::imu_gap);  // wrapped: the ring's head has moved
  const std::int64_t newest = time_of(kCap + 4);
  log.log(Fault::imu_gap, newest + kMs, 0);  // repeat of the newest, within one second
  EXPECT_EQ(log.size(), kCap);
  EXPECT_EQ(log.overwritten(), 5U);
  EXPECT_EQ(log.total(), kCap + 6);
  EXPECT_EQ(log[0].local_ns, time_of(5));
  EXPECT_EQ(log[kCap - 1].local_ns, newest);
  EXPECT_EQ(log[kCap - 1].count, 2U);
}

TEST(Srs081FaultLog, BabblingFaultCannotPushOlderFaultsOut) {
  IQK_VERIFIES("SRS-081");
  FaultLog log;
  log_spaced(log, 0, kCap - 1, Fault::imu_gap);  // one free slot
  const std::int64_t burst = time_of(kCap);
  for (std::int64_t i = 0; i < 1000; ++i) {
    log.log(Fault::aam_fault, burst + i * kMs, i);  // 1000 repeats inside one second
  }
  EXPECT_EQ(log.size(), kCap);
  EXPECT_EQ(log.overwritten(), 0U);
  EXPECT_EQ(log.count(Fault::aam_fault), 1000U);
  EXPECT_EQ(log[0].local_ns, time_of(0));  // the oldest fault is still there
  EXPECT_EQ(log[0].count, 1U);
}
