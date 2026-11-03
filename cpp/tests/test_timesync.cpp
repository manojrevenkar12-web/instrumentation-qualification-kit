#include <gtest/gtest.h>

#include <cmath>
#include <random>

#include "iqk/timesync.hpp"
#include "support.hpp"

using iqk::timesync::Source;
using iqk::timesync::State;
using iqk::timesync::TimeSync;
using iqk_test::kMs;
using iqk_test::kS;

namespace {
constexpr std::int64_t kUtc0 = 1'790'000'000LL * kS;  // 2026-09

// A local oscillator running drift_ppm fast: local(true) = true * (1 + drift).
struct Bench {
  double drift_ppm{0};
  std::int64_t local(std::int64_t true_ns) const {
    return static_cast<std::int64_t>(static_cast<double>(true_ns) * (1.0 + drift_ppm * 1e-6));
  }
  // One PTP exchange at true time t with one-way delay d: t1 master send, t2 slave rx, t3 slave tx, t4 master rx.
  void ptp(TimeSync& ts, std::int64_t t, std::int64_t d = 50'000, std::int64_t extra = 0) const {
    const std::int64_t t1 = kUtc0 + t;
    const std::int64_t t2 = local(t + d + extra);
    const std::int64_t t3 = local(t + d + 200'000);
    const std::int64_t t4 = kUtc0 + t + 2 * d + 200'000;
    ts.on_ptp_exchange(t1, t2, t3, t4);
  }
  void gnss(TimeSync& ts, std::int64_t t_second) const {
    ts.on_pps(local(t_second));
    iqk::icd::gnss::TimePulse m{};
    const std::int64_t utc_s = (kUtc0 + t_second) / kS;
    const std::int64_t gps_s = utc_s - iqk::timesync::kGpsEpochUnixS + 18;
    m.week = static_cast<std::uint16_t>(gps_s / 604800);
    m.tow_ms = static_cast<std::uint32_t>((gps_s % 604800) * 1000);
    m.leap_s = 18;
    m.fix = 3;
    m.flags = 0x7;
    m.acc_ns = 20;
    ts.on_gnss_time(m, local(t_second + 200 * kMs));
  }
  std::int64_t error(const TimeSync& ts, std::int64_t t) const { return ts.utc_ns(local(t)) - (kUtc0 + t); }
};

std::vector<iqk::timesync::Event::Kind> events(TimeSync& ts) {
  std::vector<iqk::timesync::Event::Kind> out;
  ts.drain_events([&](const iqk::timesync::Event& e) { out.push_back(e.kind); });
  return out;
}

bool has(const std::vector<iqk::timesync::Event::Kind>& v, iqk::timesync::Event::Kind k) {
  return std::find(v.begin(), v.end(), k) != v.end();
}
}  // namespace

TEST(TimeSync, PtpLocksAndRemovesOscillatorDrift) {
  IQK_VERIFIES("SRS-060", "SRS-064");
  Bench b{50.0};
  TimeSync ts;
  for (std::int64_t t = kS; t < 60 * kS; t += 125 * kMs) {
    b.ptp(ts, t);
    ts.tick(b.local(t));
  }
  EXPECT_EQ(ts.state(), State::locked);
  EXPECT_EQ(ts.source(), Source::ptp);
  EXPECT_EQ(ts.quality(), 3);
  EXPECT_LT(std::llabs(b.error(ts, 60 * kS)), 2'000);   // within 2 us
  EXPECT_NEAR(ts.rate() * 1e6, -50.0 / (1 + 50e-6), 0.5);  // learned the oscillator
}

TEST(TimeSync, LockNeedsEightConsecutiveGoodMeasurements) {
  IQK_VERIFIES("SRS-064");
  Bench b{0.0};
  TimeSync ts;
  b.ptp(ts, kS);  // first measurement sets the clock
  for (int i = 1; i <= 7; ++i) {
    b.ptp(ts, kS + i * 125 * kMs);
  }
  EXPECT_EQ(ts.state(), State::locking);
  b.ptp(ts, kS + 8 * 125 * kMs);
  EXPECT_EQ(ts.state(), State::locked);
}

TEST(TimeSync, SmallOffsetsAreSlewedLargeOnesStepped) {
  IQK_VERIFIES("SRS-063");
  Bench b{0.0};
  TimeSync ts;
  for (int i = 0; i < 20; ++i) {
    b.ptp(ts, kS + i * 125 * kMs);
  }
  (void)events(ts);
  // The master moves 0.5 ms: the clock is slewed, so time read twice a microsecond apart never goes backwards.
  std::int64_t prev = ts.utc_ns(b.local(4 * kS));
  for (int i = 0; i < 40; ++i) {
    const std::int64_t t = 4 * kS + i * 125 * kMs;
    ts.on_ptp_exchange(kUtc0 + t + 500'000, b.local(t + 50'000), b.local(t + 250'000), kUtc0 + t + 300'000 + 500'000);
    for (std::int64_t k = 0; k < 125 * kMs; k += 5 * kMs) {
      const std::int64_t now = ts.utc_ns(b.local(t + 50'000 + k));
      EXPECT_GE(now, prev);
      prev = now;
    }
  }
  EXPECT_FALSE(has(events(ts), iqk::timesync::Event::Kind::stepped));
  // A 5 ms jump is stepped, and the step is reported.
  const std::int64_t t = 10 * kS;
  ts.on_ptp_exchange(kUtc0 + t + 5 * kMs, b.local(t + 50'000), b.local(t + 250'000), kUtc0 + t + 300'000 + 5 * kMs);
  const auto ev = events(ts);
  EXPECT_TRUE(has(ev, iqk::timesync::Event::Kind::stepped));
  EXPECT_NEAR(static_cast<double>(ts.utc_ns(b.local(t + 50'000)) - (kUtc0 + t + 5 * kMs + 50'000)), 0.0, 1000.0);
}

TEST(TimeSync, AnOffsetBeyondTheUnlockThresholdDropsLockWithoutAStep) {
  IQK_VERIFIES("SRS-064");
  Bench b{0.0};
  TimeSync ts;
  for (int i = 0; i < 20; ++i) {
    b.ptp(ts, kS + i * 125 * kMs);
  }
  ASSERT_EQ(ts.state(), State::locked);
  (void)events(ts);
  const std::int64_t t = 4 * kS;
  // 0.4 ms: inside the unlock threshold, still locked
  ts.on_ptp_exchange(kUtc0 + t + 400'000, b.local(t + 50'000), b.local(t + 250'000), kUtc0 + t + 300'000 + 400'000);
  EXPECT_EQ(ts.state(), State::locked);
  // 0.7 ms more: beyond 0.5 ms, lock is lost, and the clock is slewed, not stepped
  const std::int64_t u = t + 125 * kMs;
  const std::int64_t x = ts.utc_ns(b.local(u + 50'000)) - (kUtc0 + u + 50'000) + 700'000;  // offset seen: 0.7 ms
  ts.on_ptp_exchange(kUtc0 + u + x, b.local(u + 50'000), b.local(u + 250'000), kUtc0 + u + 300'000 + x);
  EXPECT_NEAR(static_cast<double>(ts.last_offset_ns()), 700'000.0, 1000.0);
  EXPECT_EQ(ts.state(), State::locking);
  const auto ev = events(ts);
  EXPECT_TRUE(has(ev, iqk::timesync::Event::Kind::lost_lock));
  EXPECT_FALSE(has(ev, iqk::timesync::Event::Kind::stepped));
}

TEST(TimeSync, DelayOutliersAreDiscarded) {
  IQK_VERIFIES("SRS-061");
  Bench b{0.0};
  TimeSync ts;
  for (int i = 0; i < 30; ++i) {
    b.ptp(ts, kS + i * 125 * kMs);
  }
  (void)events(ts);
  const std::int64_t before = ts.last_offset_ns();
  // A Sync held in a queue for 20 ms: its path delay is far above the minimum, so it must not move the clock.
  b.ptp(ts, 5 * kS, 50'000, 20 * kMs);
  EXPECT_EQ(ts.last_offset_ns(), before);
  EXPECT_TRUE(has(events(ts), iqk::timesync::Event::Kind::ptp_outlier));
  EXPECT_LT(std::llabs(b.error(ts, 5 * kS)), 2'000);
}

TEST(TimeSync, HoldoverThenFreeRunning) {
  IQK_VERIFIES("SRS-065");
  Bench b{20.0};
  TimeSync ts;
  std::int64_t t = kS;
  for (; t < 60 * kS; t += 125 * kMs) {
    b.ptp(ts, t);
    ts.tick(b.local(t));
  }
  ASSERT_EQ(ts.state(), State::locked);
  t -= 125 * kMs;  // the last measurement
  ts.tick(b.local(t + 2400 * kMs));
  EXPECT_EQ(ts.state(), State::locked);
  ts.tick(b.local(t + 2600 * kMs));
  EXPECT_EQ(ts.state(), State::holdover);
  EXPECT_EQ(ts.quality(), 1);
  // The learned rate carries the clock through holdover: after 60 s the error stays small.
  EXPECT_LT(std::llabs(b.error(ts, t + 60 * kS)), 20'000);
  ts.tick(b.local(t + 302 * kS));
  EXPECT_EQ(ts.state(), State::holdover);
  ts.tick(b.local(t + 303 * kS));
  EXPECT_EQ(ts.state(), State::free_running);
  EXPECT_EQ(ts.quality(), 0);
  // A measurement brings it back.
  b.ptp(ts, t + 304 * kS);
  EXPECT_EQ(ts.state(), State::locking);
}

TEST(TimeSync, FailsOverAtOnceAndReturnsOnlyAfterTheHoldTime) {
  IQK_VERIFIES("SRS-066");
  Bench b{0.0};
  TimeSync ts;
  std::int64_t t = kS;
  for (; t < 20 * kS; t += 125 * kMs) {  // both sources; PTP has priority
    b.ptp(ts, t);
    if (t % kS == 0) {
      b.gnss(ts, t);
    }
    ts.tick(b.local(t));
  }
  EXPECT_EQ(ts.source(), Source::ptp);
  for (; t < 30 * kS; t += 125 * kMs) {  // PTP lost
    if (t % kS == 0) {
      b.gnss(ts, t);
    }
    ts.tick(b.local(t));
  }
  EXPECT_EQ(ts.source(), Source::gnss);
  EXPECT_NE(ts.state(), State::holdover);
  const std::int64_t back = t;
  for (; t < back + 4 * kS; t += 125 * kMs) {  // PTP returns: not trusted for 5 s
    b.ptp(ts, t);
    if (t % kS == 0) {
      b.gnss(ts, t);
    }
    ts.tick(b.local(t));
  }
  EXPECT_EQ(ts.source(), Source::gnss);
  for (; t < back + 6 * kS; t += 125 * kMs) {
    b.ptp(ts, t);
    ts.tick(b.local(t));
  }
  EXPECT_EQ(ts.source(), Source::ptp);
}

TEST(TimeSync, GnssTimeIsCheckedBeforeUse) {
  IQK_VERIFIES("SRS-062");
  Bench b{0.0};
  auto msg = [&](std::int64_t t_second) {
    iqk::icd::gnss::TimePulse m{};
    const std::int64_t gps_s = (kUtc0 + t_second) / kS - iqk::timesync::kGpsEpochUnixS + 18;
    m.week = static_cast<std::uint16_t>(gps_s / 604800);
    m.tow_ms = static_cast<std::uint32_t>((gps_s % 604800) * 1000);
    m.leap_s = 18;
    m.fix = 3;
    m.flags = 0x7;
    m.acc_ns = 20;
    return m;
  };
  auto rejected = [&](TimeSync& ts) {
    std::vector<std::int64_t> reasons;
    ts.drain_events([&](const iqk::timesync::Event& e) {
      if (e.kind == iqk::timesync::Event::Kind::gnss_rejected) {
        reasons.push_back(e.value);
      }
    });
    return reasons;
  };
  {
    TimeSync ts;
    ts.on_pps(b.local(kS));
    auto m = msg(kS);
    m.flags = 0x6;  // time not valid
    ts.on_gnss_time(m, b.local(kS + 200 * kMs));
    EXPECT_EQ(rejected(ts), std::vector<std::int64_t>{2});
    EXPECT_FALSE(ts.has_time());
  }
  {
    TimeSync ts;
    ts.on_pps(b.local(kS));
    auto m = msg(kS);
    m.acc_ns = 500;
    ts.on_gnss_time(m, b.local(kS + 200 * kMs));
    EXPECT_EQ(rejected(ts), std::vector<std::int64_t>{3});
  }
  {
    TimeSync ts;
    ts.on_gnss_time(msg(kS), b.local(kS + 200 * kMs));  // no edge
    EXPECT_EQ(rejected(ts), std::vector<std::int64_t>{4});
  }
  {
    TimeSync ts;
    ts.on_pps(b.local(kS));
    ts.on_gnss_time(msg(kS), b.local(kS + 950 * kMs));  // too late for its edge
    EXPECT_EQ(rejected(ts), std::vector<std::int64_t>{5});
  }
  {
    TimeSync ts;
    ts.on_pps(b.local(kS));
    ts.on_gnss_time(msg(kS), b.local(kS + 200 * kMs));
    EXPECT_TRUE(rejected(ts).empty());
    EXPECT_LT(std::llabs(b.error(ts, kS)), 1000);
    ts.on_gnss_time(msg(kS), b.local(kS + 300 * kMs));  // the same edge again
    EXPECT_EQ(rejected(ts), std::vector<std::int64_t>{4});
  }
}

TEST(TimeSync, GpsTimeConvertsToUtc) {
  IQK_VERIFIES("SRS-062");
  // GPS week 2400, 0 ms = 2026-01-04T00:00:18 GPS = 2026-01-04T00:00:00 UTC with 18 leap seconds.
  EXPECT_EQ(iqk::timesync::gps_to_utc_ns(2400, 0, 18), (315'964'800LL + 2400LL * 604'800 - 18) * kS);
}
