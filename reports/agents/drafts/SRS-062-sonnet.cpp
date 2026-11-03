#include "support.hpp"

#include "iqk/config.hpp"
#include "iqk/timesync.hpp"

namespace {
using iqk::icd::gnss::TimePulse;
using iqk::timesync::Event;
using iqk::timesync::TimeSync;
using iqk_test::kMs;
using iqk_test::kS;

constexpr std::int64_t kEdge = 10 * kS;

TimePulse good() {
  TimePulse m{};
  m.week = 2300;
  m.tow_ms = 345000;
  m.leap_s = 18;
  m.fix = 3;
  m.set_time_valid(true);
  m.set_leap_valid(true);
  m.acc_ns = 50;
  return m;
}

std::int64_t expected_utc(const TimePulse& m) { return iqk::timesync::gps_to_utc_ns(m.week, m.tow_ms, m.leap_s); }

struct Counts {
  int rejected{0};
  int stepped{0};
  std::int64_t last_reason{0};
};

Counts drain(TimeSync& ts) {
  Counts c;
  ts.drain_events([&](const Event& e) {
    if (e.kind == Event::Kind::gnss_rejected) {
      ++c.rejected;
      c.last_reason = e.value;
    }
    if (e.kind == Event::Kind::stepped) {
      ++c.stepped;
    }
  });
  return c;
}

// Edge at kEdge, message `delay_ns` later. True when the message was used to set the clock.
bool used(const TimePulse& m, std::int64_t delay_ns) {
  TimeSync ts;
  ts.on_pps(kEdge);
  ts.on_gnss_time(m, kEdge + delay_ns);
  const Counts c = drain(ts);
  if (ts.has_time()) {
    EXPECT_EQ(c.rejected, 0);
    EXPECT_EQ(ts.utc_ns(kEdge), expected_utc(m));  // time is referenced to the PPS edge
    return true;
  }
  EXPECT_EQ(c.rejected, 1);
  EXPECT_EQ(c.stepped, 0);
  return false;
}
}  // namespace

TEST(Srs062, ValidMessageIsUsed) {
  IQK_VERIFIES("SRS-062");
  EXPECT_TRUE(used(good(), 100 * kMs));
}

TEST(Srs062, TimeValidFlagRequired) {
  IQK_VERIFIES("SRS-062");
  TimePulse m = good();
  m.set_time_valid(false);
  EXPECT_FALSE(used(m, 100 * kMs));
}

TEST(Srs062, LeapValidFlagRequired) {
  IQK_VERIFIES("SRS-062");
  TimePulse m = good();
  m.set_leap_valid(false);
  EXPECT_FALSE(used(m, 100 * kMs));
}

TEST(Srs062, RaimNotRequired) {
  IQK_VERIFIES("SRS-062");
  TimePulse m = good();
  m.set_raim_ok(false);
  EXPECT_TRUE(used(m, 100 * kMs));
  m.set_raim_ok(true);
  EXPECT_TRUE(used(m, 100 * kMs));
}

TEST(Srs062, FixMustBe2DOr3D) {
  IQK_VERIFIES("SRS-062");
  TimePulse m = good();
  m.fix = 0;
  EXPECT_FALSE(used(m, 100 * kMs));
  m.fix = 1;
  EXPECT_FALSE(used(m, 100 * kMs));
  m.fix = 2;
  EXPECT_TRUE(used(m, 100 * kMs));
  m.fix = 3;
  EXPECT_TRUE(used(m, 100 * kMs));
}

TEST(Srs062, AccuracyBoundary) {
  IQK_VERIFIES("SRS-062");
  TimePulse m = good();
  const auto lim = static_cast<std::uint32_t>(iqk::cfg::timing::gnss_max_acc_ns);
  m.acc_ns = 0;
  EXPECT_TRUE(used(m, 100 * kMs));
  m.acc_ns = lim - 1;
  EXPECT_TRUE(used(m, 100 * kMs));
  m.acc_ns = lim;
  EXPECT_TRUE(used(m, 100 * kMs));
  m.acc_ns = lim + 1;
  EXPECT_FALSE(used(m, 100 * kMs));
  m.acc_ns = 100000;
  EXPECT_FALSE(used(m, 100 * kMs));
}

TEST(Srs062, MessageDelayBoundary) {
  IQK_VERIFIES("SRS-062");
  const std::int64_t lim = iqk::cfg::timing::gnss_max_msg_delay_ms * kMs;
  EXPECT_TRUE(used(good(), 0));
  EXPECT_TRUE(used(good(), lim - 1));
  EXPECT_TRUE(used(good(), lim));
  EXPECT_FALSE(used(good(), lim + 1));
  EXPECT_FALSE(used(good(), lim + 100 * kMs));
}

TEST(Srs062, MessageBeforeEdgeRejected) {
  IQK_VERIFIES("SRS-062");
  EXPECT_FALSE(used(good(), -1));
  EXPECT_FALSE(used(good(), -100 * kMs));
}

TEST(Srs062, MessageWithoutEdgeRejected) {
  IQK_VERIFIES("SRS-062");
  TimeSync ts;
  ts.on_gnss_time(good(), kEdge);
  EXPECT_FALSE(ts.has_time());
  EXPECT_EQ(drain(ts).rejected, 1);
}

TEST(Srs062, EachEdgeUsedOnce) {
  IQK_VERIFIES("SRS-062");
  TimeSync ts;
  ts.on_pps(kEdge);
  TimePulse m = good();
  ts.on_gnss_time(m, kEdge + 100 * kMs);
  ASSERT_TRUE(ts.has_time());
  Counts c = drain(ts);
  EXPECT_EQ(c.rejected, 0);
  EXPECT_EQ(c.stepped, 1);

  // A second message for the same edge, even a different/valid one, is not used.
  m.tow_ms += 5000;
  ts.on_gnss_time(m, kEdge + 200 * kMs);
  c = drain(ts);
  EXPECT_EQ(c.rejected, 1);
  EXPECT_EQ(c.last_reason, 4);
  EXPECT_EQ(ts.utc_ns(kEdge), expected_utc(good()));
  EXPECT_EQ(ts.last_offset_ns(), 0);

  // A fresh edge makes the next message usable again.
  ts.on_pps(kEdge + kS);
  TimePulse next = good();
  next.tow_ms += 1000;
  ts.on_gnss_time(next, kEdge + kS + 100 * kMs);
  c = drain(ts);
  EXPECT_EQ(c.rejected, 0);
  EXPECT_EQ(ts.source(), iqk::timesync::Source::gnss);
}

TEST(Srs062, RejectedMessageDoesNotConsumeEdge) {
  IQK_VERIFIES("SRS-062");
  TimeSync ts;
  ts.on_pps(kEdge);
  TimePulse bad = good();
  bad.fix = 1;
  ts.on_gnss_time(bad, kEdge + 100 * kMs);
  EXPECT_FALSE(ts.has_time());
  EXPECT_EQ(drain(ts).rejected, 1);
  ts.on_gnss_time(good(), kEdge + 200 * kMs);
  EXPECT_TRUE(ts.has_time());
  EXPECT_EQ(drain(ts).rejected, 0);
  EXPECT_EQ(ts.utc_ns(kEdge), expected_utc(good()));
}

TEST(Srs062, LateMessageUsesLatestEdgeOnly) {
  IQK_VERIFIES("SRS-062");
  TimeSync ts;
  ts.on_pps(kEdge);
  ts.on_pps(kEdge + kS);  // newer edge replaces the older
  ts.on_gnss_time(good(), kEdge + kS + 100 * kMs);
  ASSERT_TRUE(ts.has_time());
  EXPECT_EQ(ts.utc_ns(kEdge + kS), expected_utc(good()));
}
