#include "support.hpp"

#include <cstdint>
#include <vector>

#include "iqk/drivers.hpp"

namespace {

using namespace iqk_test;

// Feeds samples at the given device times; arrival = true device time + fixed latency, so the recovered
// acquisition time must advance exactly with the true (unwrapped) elapsed device time.
struct Run {
  iqk::ImuDriver drv;
  iqk::FaultLog log;
  std::vector<std::int64_t> local;
  std::uint16_t seq{0};
  std::int64_t true_us{0};  // unwrapped device time

  void feed(std::uint32_t start_us, std::int64_t step_us, int n) {
    true_us = start_us;
    for (int i = 0; i < n; ++i) {
      push(static_cast<std::uint32_t>(true_us & 0xFFFFFFFF));
      true_us += step_us;
    }
  }
  void push(std::uint32_t t_us) {
    const auto f = imu_frame(imu_sample(seq++, t_us));
    drv.on_bytes(f, 5 * kS + true_us * 1000, log, [&](const iqk::ImuReading& r) { local.push_back(r.local_ns); });
  }
};

void expect_steps(const std::vector<std::int64_t>& v, std::int64_t step_ns) {
  for (std::size_t i = 1; i < v.size(); ++i) {
    EXPECT_EQ(v[i] - v[i - 1], step_ns) << "at index " << i;
  }
}

TEST(DeviceTimeWrap, ContinuousAcrossWrap) {
  IQK_VERIFIES("SRS-022");
  Run r;
  r.feed(0xFFFFFFFFU - 10 * 2500U, 2500, 21);  // wraps in the middle
  ASSERT_EQ(r.local.size(), 21U);
  expect_steps(r.local, 2'500'000);
}

TEST(DeviceTimeWrap, WrapExactlyToZero) {
  IQK_VERIFIES("SRS-022");
  Run r;
  r.feed(0xFFFFFFFFU - 999U, 1000, 4);  // 0xFFFFFC18.. then 0x...
  ASSERT_EQ(r.local.size(), 4U);
  expect_steps(r.local, 1'000'000);
}

TEST(DeviceTimeWrap, OneMicrosecondStepAcrossWrap) {
  IQK_VERIFIES("SRS-022");
  Run r;
  r.feed(0xFFFFFFFFU - 2U, 1, 6);  // FFFFFFFD, FFFFFFFE, FFFFFFFF, 0, 1, 2
  ASSERT_EQ(r.local.size(), 6U);
  expect_steps(r.local, 1000);
}

TEST(DeviceTimeWrap, LargeStepAcrossWrap) {
  IQK_VERIFIES("SRS-022");
  Run r;
  r.feed(0xFFFFFFFFU - 1000U, 0x40000000LL, 4);
  ASSERT_EQ(r.local.size(), 4U);
  expect_steps(r.local, 0x40000000LL * 1000);
}

TEST(DeviceTimeWrap, MultipleWrapsMonotonic) {
  IQK_VERIFIES("SRS-022");
  Run r;
  const std::int64_t step = 0x30000000LL;
  r.feed(0xFFFFFF00U, step, 30);
  ASSERT_EQ(r.local.size(), 30U);
  expect_steps(r.local, step * 1000);
  EXPECT_GT(r.local.back() - r.local.front(), (std::int64_t{1} << 32) * 1000 * 5);
}

TEST(DeviceTimeWrap, NoWrapUnaffected) {
  IQK_VERIFIES("SRS-022");
  Run r;
  r.feed(1'000'000U, 2500, 20);
  ASSERT_EQ(r.local.size(), 20U);
  expect_steps(r.local, 2'500'000);
}

TEST(DeviceTimeWrap, TimeAfterWrapMatchesArrival) {
  IQK_VERIFIES("SRS-022");
  Run r;
  r.feed(0xFFFFFFFFU - 5 * 2500U, 2500, 12);
  ASSERT_EQ(r.local.size(), 12U);
  // with constant latency, acquisition time equals arrival time, before and after the wrap
  const std::int64_t first_rx = 5 * kS + std::int64_t{0xFFFFFFFFU - 5 * 2500U} * 1000;
  for (std::size_t i = 0; i < r.local.size(); ++i) {
    EXPECT_EQ(r.local[i], first_rx + static_cast<std::int64_t>(i) * 2'500'000) << i;
  }
}

}  // namespace
