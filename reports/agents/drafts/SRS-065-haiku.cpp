#include "support.hpp"

#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "iqk/config.hpp"
#include "iqk/icd/gnss.hpp"
#include "iqk/timesync.hpp"

namespace {

using iqk::timesync::Event;
using iqk::timesync::State;
using iqk::timesync::TimeSync;
using iqk_test::kMs;
using iqk_test::kS;

constexpr std::int64_t kUs = 1'000;
constexpr std::int64_t kHoldoverStart = iqk::cfg::timing::holdover_start_ms * kMs;
constexpr std::int64_t kHoldoverMax = iqk::cfg::timing::holdover_max_ms * kMs;
constexpr std::int64_t kPtpPeriod = 125 * kMs;  // PTP at 8 Hz
constexpr std::uint8_t kQualityFreeRunning = 0;
constexpr std::uint8_t kQualityHoldover = 1;
constexpr std::uint8_t kQualityLockedGnss = 2;
constexpr std::uint8_t kQualityLockedPtp = 3;

// A PTP master whose clock runs rate_ppb faster than the unit's monotonic clock, reached over a symmetric path.
class PtpMaster {
 public:
  explicit PtpMaster(std::int64_t rate_ppb) noexcept : rate_{static_cast<double>(rate_ppb) * 1e-9} {}

  // Master UTC at local monotonic time p. Integer arithmetic for the offset: a double cannot hold it to 1 ns.
  [[nodiscard]] std::int64_t utc(std::int64_t p) const noexcept {
    return kEpoch + p + std::llround(static_cast<double>(p) * rate_);
  }

  // One two-step exchange. Sync leaves the master at local time p; Delay_Req leaves the unit 1 ms after it arrives.
  // Returns the local time of the measurement (t2).
  std::int64_t exchange(TimeSync& ts, std::int64_t p) const noexcept {
    const std::int64_t t2 = p + kPath;
    const std::int64_t t3 = t2 + kMs;
    ts.on_ptp_exchange(utc(p), t2, t3, utc(t3 + kPath));
    return t2;
  }

  static constexpr std::int64_t kPath = 20 * kUs;  // one-way delay, each direction

 private:
  static constexpr std::int64_t kEpoch = 1'700'000'000 * kS;
  double rate_;
};

// Locks the unit to the master with 60 s of PTP measurements. Returns the local time of the last measurement.
std::int64_t lock_to_ptp(TimeSync& ts, const PtpMaster& m) {
  std::int64_t last = 0;
  for (std::int64_t p = 10 * kS; p < 70 * kS; p += kPtpPeriod) {
    last = m.exchange(ts, p);
    ts.tick(last);
  }
  return last;
}

// A GNSS time pulse naming the second that started at local time edge. The master is on GPS time and the local
// clock runs at the same rate, so the named second is edge itself (edge is a whole number of milliseconds).
iqk::icd::gnss::TimePulse gnss_pulse(std::int64_t edge) {
  iqk::icd::gnss::TimePulse m{};
  m.week = 2300;
  m.tow_ms = static_cast<std::uint32_t>(edge / kMs);
  m.leap_s = 18;
  m.fix = 3;
  m.flags = 0x07;  // time_valid, leap_valid, raim_ok
  m.acc_ns = 50;
  return m;
}

// Ticks at the unit's required cadence (every 100 ms) from `from` to `to`, inclusive.
void tick_until(TimeSync& ts, std::int64_t from, std::int64_t to) {
  for (std::int64_t t = from; t < to; t += 100 * kMs) {
    ts.tick(t);
  }
  ts.tick(to);
}

// Local time at which the unit last entered holdover, from its event log. -1 if it has not.
std::int64_t holdover_entry(TimeSync& ts) {
  std::int64_t at = -1;
  ts.drain_events([&](const Event& e) {
    if (e.kind == Event::Kind::holdover) {
      at = e.local_ns;
    }
  });
  return at;
}

}  // namespace

// SRS-065: without measurements for holdover_start_ms the unit enters holdover (quality 1).
TEST(Srs065, HoldoverStartsOnlyAfterHoldoverStartWithoutPtpMeasurement) {
  IQK_VERIFIES("SRS-065");
  TimeSync ts;
  const PtpMaster master{50'000};
  const std::int64_t last = lock_to_ptp(ts, master);
  ASSERT_EQ(ts.state(), State::locked);
  ASSERT_EQ(ts.quality(), kQualityLockedPtp);

  // One millisecond inside the window: still locked, not holdover.
  tick_until(ts, last, last + kHoldoverStart - kMs);
  EXPECT_EQ(ts.state(), State::locked);
  EXPECT_EQ(ts.quality(), kQualityLockedPtp);

  // One millisecond past the window: holdover, reported as quality 1.
  tick_until(ts, last + kHoldoverStart - kMs, last + kHoldoverStart + kMs);
  EXPECT_EQ(ts.state(), State::holdover);
  EXPECT_EQ(ts.quality(), kQualityHoldover);
}

TEST(Srs065, HoldoverStartsOnlyAfterHoldoverStartWithoutGnssMeasurement) {
  IQK_VERIFIES("SRS-065");
  TimeSync ts;
  // One time pulse per second; the first only sets the clock, then eight consecutive in-threshold ones lock it.
  std::int64_t last = 0;
  for (std::int64_t edge = kS; edge <= 15 * kS; edge += kS) {
    ts.on_pps(edge);
    ts.on_gnss_time(gnss_pulse(edge), edge + 5 * kMs);
    last = edge;
    ts.tick(edge + 5 * kMs);
  }
  ASSERT_EQ(ts.state(), State::locked);
  ASSERT_EQ(ts.quality(), kQualityLockedGnss);

  tick_until(ts, last, last + kHoldoverStart - kMs);
  EXPECT_EQ(ts.state(), State::locked);
  EXPECT_EQ(ts.quality(), kQualityLockedGnss);

  tick_until(ts, last + kHoldoverStart - kMs, last + kHoldoverStart + kMs);
  EXPECT_EQ(ts.state(), State::holdover);
  EXPECT_EQ(ts.quality(), kQualityHoldover);
}

// SRS-065: in holdover the unit keeps time at the learned rate.
TEST(Srs065, HoldoverKeepsTimeAtLearnedRate) {
  IQK_VERIFIES("SRS-065");
  const std::int64_t rate_ppb = 50'000;  // the master runs 50 ppm fast; the servo must learn it
  const PtpMaster master{rate_ppb};
  TimeSync ts;
  const std::int64_t last = lock_to_ptp(ts, master);
  ASSERT_EQ(ts.quality(), kQualityLockedPtp);
  EXPECT_NEAR(ts.rate(), static_cast<double>(rate_ppb) * 1e-9, 1e-6);
  const double learned = ts.rate();

  const std::int64_t silence_end = last + 60 * kS;
  tick_until(ts, last, silence_end);
  ASSERT_EQ(ts.state(), State::holdover);
  EXPECT_EQ(ts.quality(), kQualityHoldover);
  EXPECT_EQ(ts.rate(), learned);  // holdover does not change the rate

  // 60 s of holdover: a unit that does not apply the learned rate is 3 ms off; one that does is well under 10 us.
  const std::int64_t error = ts.utc_ns(silence_end) - master.utc(silence_end);
  EXPECT_LT(std::llabs(error), 10 * kUs);
}

// SRS-065: holdover lasts holdover_max_ms; after that the unit is free-running (quality 0).
TEST(Srs065, FreeRunningOnlyAfterHoldoverMaxInHoldover) {
  IQK_VERIFIES("SRS-065");
  TimeSync ts;
  const PtpMaster master{50'000};
  const std::int64_t last = lock_to_ptp(ts, master);
  ASSERT_EQ(ts.quality(), kQualityLockedPtp);

  tick_until(ts, last, last + kHoldoverStart + kMs);
  ASSERT_EQ(ts.state(), State::holdover);
  const std::int64_t entered = holdover_entry(ts);
  ASSERT_GE(entered, last + kHoldoverStart);

  // One millisecond inside holdover_max: still holdover.
  tick_until(ts, entered, entered + kHoldoverMax - kMs);
  EXPECT_EQ(ts.state(), State::holdover);
  EXPECT_EQ(ts.quality(), kQualityHoldover);

  // One millisecond past holdover_max: free-running.
  ts.tick(entered + kHoldoverMax + kMs);
  EXPECT_EQ(ts.state(), State::free_running);
  EXPECT_EQ(ts.quality(), kQualityFreeRunning);
}

// SRS-065 (the condition): holdover is not entered while measurements keep arriving.
TEST(Srs065, NoHoldoverWhileMeasurementsKeepArriving) {
  IQK_VERIFIES("SRS-065");
  TimeSync ts;
  const PtpMaster master{50'000};
  const std::int64_t last = lock_to_ptp(ts, master);
  ASSERT_EQ(ts.quality(), kQualityLockedPtp);
  const std::int64_t end = last + 30 * kS;

  int holdover_seen = 0;
  for (std::int64_t p = last + kPtpPeriod; p < end; p += kPtpPeriod) {
    ts.tick(master.exchange(ts, p));
    if (ts.state() == State::holdover) {
      ++holdover_seen;
    }
  }
  EXPECT_EQ(holdover_seen, 0);
  EXPECT_EQ(ts.quality(), kQualityLockedPtp);
}
