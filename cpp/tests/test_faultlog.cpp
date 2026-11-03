#include <gtest/gtest.h>

#include "iqk/faultlog.hpp"
#include "support.hpp"

using iqk::Fault;
using iqk::FaultLog;
using iqk_test::kMs;
using iqk_test::kS;

TEST(FaultLog, RepeatsWithinASecondAreFolded) {
  IQK_VERIFIES("SRS-081");
  FaultLog log;
  for (int i = 0; i < 100; ++i) {
    log.log(Fault::imu_crc, kS + i * kMs, i);
  }
  EXPECT_EQ(log.size(), 1U);
  EXPECT_EQ(log[0].count, 100U);
  EXPECT_EQ(log[0].detail, 99);
  EXPECT_EQ(log.total(), 100U);
  log.log(Fault::imu_crc, 3 * kS, 0);
  EXPECT_EQ(log.size(), 2U);
}

TEST(FaultLog, ABabblingSourceCannotFlushOlderFaults) {
  IQK_VERIFIES("SRS-081");
  FaultLog log;
  log.log(Fault::aam_lost, kS, 2);
  for (int i = 0; i < 100'000; ++i) {
    log.log(Fault::imu_crc, kS + i * 10'000, i);  // 100 000 errors within one second
  }
  EXPECT_EQ(log.size(), 2U);
  EXPECT_EQ(log[0].code, Fault::aam_lost);
}

TEST(FaultLog, WhenFullTheOldestIsOverwritten) {
  IQK_VERIFIES("SRS-081");
  FaultLog log;
  for (std::size_t i = 0; i < FaultLog::kCapacity + 10; ++i) {
    log.log(i % 2 == 0 ? Fault::imu_gap : Fault::aam_gap, static_cast<std::int64_t>(i) * 2 * kS, static_cast<std::int64_t>(i));
  }
  EXPECT_EQ(log.size(), FaultLog::kCapacity);
  EXPECT_EQ(log.overwritten(), 10U);
  EXPECT_EQ(log[0].detail, 10);
  EXPECT_EQ(log[log.size() - 1].detail, static_cast<std::int64_t>(FaultLog::kCapacity + 9));
}
