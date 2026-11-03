#include <gtest/gtest.h>

#include <random>

#include "iqk/drivers.hpp"
#include "support.hpp"

using iqk::AamDriver;
using iqk::Fault;
using iqk::FaultLog;
using iqk::ImuDriver;
using iqk::ImuReading;
using iqk_test::kMs;
using iqk_test::kS;

namespace {
struct Collected {
  std::vector<ImuReading> r;
};
void send(ImuDriver& d, FaultLog& log, Collected& c, const iqk::icd::imu::ImuSample& s, std::int64_t rx) {
  const auto f = iqk_test::imu_frame(s);
  d.on_bytes(f, rx, log, [&](const ImuReading& x) { c.r.push_back(x); });
}
}  // namespace

TEST(Imu, ArrivalJitterDoesNotMoveTimeStamps) {
  IQK_VERIFIES("SRS-020");
  ImuDriver d;
  FaultLog log;
  Collected c;
  std::mt19937 rng{3};
  std::uniform_int_distribution<std::int64_t> jitter(0, 3 * kMs);
  const std::int64_t base_latency = 400'000;  // 0.4 ms transport
  for (std::int64_t i = 0; i < 2000; ++i) {
    const std::int64_t taken = 10 * kS + i * 2'500'000;
    send(d, log, c, iqk_test::imu_sample(static_cast<std::uint16_t>(i), static_cast<std::uint32_t>(i * 2500)),
         taken + base_latency + jitter(rng));
  }
  ASSERT_EQ(c.r.size(), 2000U);
  // After the filter has seen the fastest arrival, every stamp is the acquisition time plus the minimum latency.
  for (std::size_t i = 500; i < c.r.size(); ++i) {
    const std::int64_t taken = 10 * kS + static_cast<std::int64_t>(i) * 2'500'000;
    EXPECT_NEAR(static_cast<double>(c.r[i].local_ns - taken), static_cast<double>(base_latency), 30'000.0) << i;
    EXPECT_NEAR(static_cast<double>(c.r[i].local_ns - c.r[i - 1].local_ns), 2'500'000.0, 30'000.0) << i;
  }
}

TEST(Imu, DeviceTimeWrapsAt32Bits) {
  IQK_VERIFIES("SRS-022");
  ImuDriver d;
  FaultLog log;
  Collected c;
  const std::uint32_t start = 0xFFFFFFFFU - 5000U;
  for (std::int64_t i = 0; i < 8; ++i) {
    send(d, log, c, iqk_test::imu_sample(static_cast<std::uint16_t>(i), start + static_cast<std::uint32_t>(i) * 2500U),
         kS + i * 2'500'000);
  }
  ASSERT_EQ(c.r.size(), 8U);
  for (std::size_t i = 1; i < c.r.size(); ++i) {
    EXPECT_EQ(c.r[i].local_ns - c.r[i - 1].local_ns, 2'500'000);
  }
}

TEST(Imu, SequenceGapsAreCountedAcrossTheWrap) {
  IQK_VERIFIES("SRS-021");
  ImuDriver d;
  FaultLog log;
  Collected c;
  std::uint32_t t = 0;
  auto at = [&](std::uint16_t seq) {
    t += 2500;
    send(d, log, c, iqk_test::imu_sample(seq, t), kS + static_cast<std::int64_t>(t) * 1000);
  };
  at(65534);
  at(65535);
  at(0);  // wrap: not a gap
  at(1);
  EXPECT_EQ(d.gaps(), 0U);
  at(3);  // one missing: counted, below the logging threshold
  EXPECT_EQ(d.gaps(), 1U);
  EXPECT_EQ(log.count(Fault::imu_gap), 0U);
  at(13);  // nine missing: logged
  EXPECT_EQ(d.gaps(), 10U);
  EXPECT_EQ(log.count(Fault::imu_gap), 1U);
}

TEST(Imu, OutOfRangeSamplesAreDroppedAndLogged) {
  IQK_VERIFIES("SRS-003");
  ImuDriver d;
  FaultLog log;
  Collected c;
  auto s = iqk_test::imu_sample(1, 100);
  s.gyro_x = 9'000'000;  // 9 rad/s, beyond the 8.727 rad/s the IMU can measure
  send(d, log, c, s, kS);
  EXPECT_TRUE(c.r.empty());
  EXPECT_EQ(d.range_errors(), 1U);
  EXPECT_EQ(log.count(Fault::imu_range), 1U);
}

TEST(Imu, SelfTestResultDrivesTheBuiltInTest) {
  IQK_VERIFIES("SRS-023");
  ImuDriver d;
  FaultLog log;
  Collected c;
  EXPECT_FALSE(d.bit_passed());  // no result yet
  auto result = [&](std::uint8_t r) {
    iqk::icd::imu::ImuBitResult m{};
    m.result = r;
    std::array<std::uint8_t, iqk::icd::imu::ImuBitResult::kSize> p{};
    (void)iqk::icd::imu::encode(m, p);
    std::array<std::uint8_t, 16> f{};
    const auto n = iqk::encode_frame(static_cast<std::uint8_t>(iqk::icd::imu::ImuBitResult::kId), p, f);
    d.on_bytes(std::span{f}.first(n), kS, log, [&](const ImuReading& x) { c.r.push_back(x); });
  };
  result(1);
  EXPECT_FALSE(d.bit_passed());
  result(0);
  EXPECT_TRUE(d.bit_passed());
}

TEST(Imu, LivenessFollowsTheStream) {
  IQK_VERIFIES("SRS-023");
  ImuDriver d;
  FaultLog log;
  Collected c;
  EXPECT_FALSE(d.alive(kS));
  send(d, log, c, iqk_test::imu_sample(0, 0), kS);
  EXPECT_TRUE(d.alive(kS + 40 * kMs));
  EXPECT_FALSE(d.alive(kS + 60 * kMs));
}

TEST(Aam, AModuleWithoutHeartbeatIsLostOnce) {
  IQK_VERIFIES("SRS-030");
  AamDriver d;
  FaultLog log;
  d.on_frame(iqk_test::heartbeat(3, 2), kS, log, [](const iqk::AamReading&) {});
  d.supervise(kS + 3400 * kMs, log);
  EXPECT_TRUE(d.alive(3, kS + 3400 * kMs));
  EXPECT_EQ(log.count(Fault::aam_lost), 0U);
  d.supervise(kS + 3600 * kMs, log);
  d.supervise(kS + 9000 * kMs, log);
  EXPECT_FALSE(d.alive(3, kS + 3600 * kMs));
  EXPECT_EQ(log.count(Fault::aam_lost), 1U);
  EXPECT_FALSE(d.alive(4, kS));  // never heard of
}

TEST(Aam, CountersArePerGroup) {
  IQK_VERIFIES("SRS-031");
  AamDriver d;
  FaultLog log;
  int readings = 0;
  auto on = [&](const iqk::AamReading&) { ++readings; };
  for (int k = 0; k < 300; ++k) {  // groups 0 and 1 interleaved, each with its own counter, wrapping at 255
    d.on_frame(iqk_test::data_frame(1, 0, static_cast<std::uint8_t>(k), 1, 2, 3), kS, log, on);
    d.on_frame(iqk_test::data_frame(1, 1, static_cast<std::uint8_t>(k + 100), 1, 2, 3), kS, log, on);
  }
  EXPECT_EQ(d.gaps(), 0U);
  EXPECT_EQ(readings, 600);
  d.on_frame(iqk_test::data_frame(1, 1, static_cast<std::uint8_t>(300 + 100 + 1), 0, 0, 0), kS, log, on);
  EXPECT_EQ(d.gaps(), 1U);
  EXPECT_EQ(log.count(Fault::aam_gap), 1U);
}

TEST(Aam, BadFramesAreCountedNotUsed) {
  IQK_VERIFIES("SRS-003");
  AamDriver d;
  FaultLog log;
  int readings = 0;
  d.on_frame(iqk_test::data_frame(1, 7, 0, 0, 0, 0), kS, log, [&](const iqk::AamReading&) { ++readings; });  // mux 7
  EXPECT_EQ(readings, 0);
  EXPECT_EQ(d.errors(), 1U);
}
