#include "support.hpp"

#include <cmath>

#include "iqk/config.hpp"
#include "iqk/timesync.hpp"

namespace {

using iqk::timesync::State;
using iqk::timesync::TimeSync;
using iqk_test::kMs;

constexpr std::int64_t kStartMs = static_cast<std::int64_t>(iqk::cfg::timing::holdover_start_ms);
constexpr std::int64_t kMaxMs = static_cast<std::int64_t>(iqk::cfg::timing::holdover_max_ms);
constexpr std::int64_t kBaseUtc = 1'700'000'000LL * iqk_test::kS;
constexpr double kMasterPpm = 100e-6;  // master runs 100 ppm fast relative to the local clock

std::int64_t master_at(std::int64_t local) {
  return kBaseUtc + local + static_cast<std::int64_t>(std::llround(static_cast<double>(local) * kMasterPpm));
}

// Feeds zero-delay PTP exchanges every 125 ms (8 Hz); returns the local time of the last one.
std::int64_t feed(TimeSync& ts, int n) {
  std::int64_t local = 1000 * kMs;
  for (int i = 0; i < n; ++i) {
    const std::int64_t master = master_at(local);
    ts.on_ptp_exchange(master, local, local, master);
    ts.tick(local);
    local += 125 * kMs;
  }
  return local - 125 * kMs;
}

TEST(Holdover, StaysLiveUpToHoldoverStart) {
  IQK_VERIFIES("SRS-065");
  TimeSync ts;
  const std::int64_t last = feed(ts, 80);
  ASSERT_EQ(ts.state(), State::locked);
  ASSERT_EQ(ts.quality(), 3);
  ts.tick(last + (kStartMs - 1) * kMs);
  EXPECT_EQ(ts.state(), State::locked);
  EXPECT_EQ(ts.quality(), 3);
  ts.tick(last + kStartMs * kMs);
  EXPECT_EQ(ts.state(), State::locked);
  EXPECT_EQ(ts.quality(), 3);
}

TEST(Holdover, EntersHoldoverWithQualityOneAfterHoldoverStart) {
  IQK_VERIFIES("SRS-065");
  TimeSync ts;
  const std::int64_t last = feed(ts, 80);
  ASSERT_EQ(ts.state(), State::locked);
  ts.tick(last + kStartMs * kMs + 1 * kMs);
  EXPECT_EQ(ts.state(), State::holdover);
  EXPECT_EQ(ts.quality(), 1);
}

TEST(Holdover, EntersHoldoverWhenTickedLate) {
  IQK_VERIFIES("SRS-065");
  TimeSync ts;
  const std::int64_t last = feed(ts, 80);
  ts.tick(last + (kStartMs + 100) * kMs);
  EXPECT_EQ(ts.state(), State::holdover);
  EXPECT_EQ(ts.quality(), 1);
}

TEST(Holdover, KeepsLearnedRateWhileInHoldover) {
  IQK_VERIFIES("SRS-065");
  TimeSync ts;
  const std::int64_t last = feed(ts, 160);
  const double rate = ts.rate();
  ASSERT_GT(rate, 50e-6);
  ASSERT_LT(rate, 150e-6);
  const std::int64_t u0 = ts.utc_ns(last);
  ts.tick(last + (kStartMs + 1) * kMs);
  ASSERT_EQ(ts.state(), State::holdover);
  EXPECT_DOUBLE_EQ(ts.rate(), rate);
  const std::int64_t later = last + 60 * iqk_test::kS;
  ts.tick(later);
  ASSERT_EQ(ts.state(), State::holdover);
  EXPECT_DOUBLE_EQ(ts.rate(), rate);
  const double expected = static_cast<double>(later - last) * (1.0 + rate);
  EXPECT_NEAR(static_cast<double>(ts.utc_ns(later) - u0), expected, 2.0);
  EXPECT_LT(std::llabs(ts.utc_ns(later) - master_at(later)), 20'000);
}

TEST(Holdover, StaysInHoldoverUpToHoldoverMax) {
  IQK_VERIFIES("SRS-065");
  TimeSync ts;
  const std::int64_t last = feed(ts, 80);
  const std::int64_t enter = last + (kStartMs + 1) * kMs;
  ts.tick(enter);
  ASSERT_EQ(ts.state(), State::holdover);
  for (std::int64_t t = enter; t < enter + kMaxMs * kMs; t += 100 * kMs) {
    ts.tick(t);
    ASSERT_EQ(ts.state(), State::holdover) << "ms into holdover: " << (t - enter) / kMs;
    ASSERT_EQ(ts.quality(), 1);
  }
  ts.tick(enter + kMaxMs * kMs);
  EXPECT_EQ(ts.state(), State::holdover);
  EXPECT_EQ(ts.quality(), 1);
}

TEST(Holdover, FreeRunningWithQualityZeroAfterHoldoverMax) {
  IQK_VERIFIES("SRS-065");
  TimeSync ts;
  const std::int64_t last = feed(ts, 80);
  const std::int64_t enter = last + (kStartMs + 1) * kMs;
  ts.tick(enter);
  ASSERT_EQ(ts.state(), State::holdover);
  ts.tick(enter + kMaxMs * kMs + 1 * kMs);
  EXPECT_EQ(ts.state(), State::free_running);
  EXPECT_EQ(ts.quality(), 0);
  ts.tick(enter + 2 * kMaxMs * kMs);
  EXPECT_EQ(ts.state(), State::free_running);
  EXPECT_EQ(ts.quality(), 0);
}

TEST(Holdover, MeasurementsResumingLeaveHoldover) {
  IQK_VERIFIES("SRS-065");
  TimeSync ts;
  const std::int64_t last = feed(ts, 80);
  const std::int64_t enter = last + (kStartMs + 1) * kMs;
  ts.tick(enter);
  ASSERT_EQ(ts.state(), State::holdover);
  const std::int64_t local = enter + 1000 * kMs;
  const std::int64_t master = master_at(local);
  ts.on_ptp_exchange(master, local, local, master);
  EXPECT_NE(ts.state(), State::holdover);
  EXPECT_NE(ts.quality(), 1);
}

TEST(Holdover, NoHoldoverWhileMeasurementsContinue) {
  IQK_VERIFIES("SRS-065");
  TimeSync ts;
  feed(ts, 400);
  EXPECT_EQ(ts.state(), State::locked);
  EXPECT_EQ(ts.quality(), 3);
}

}  // namespace
