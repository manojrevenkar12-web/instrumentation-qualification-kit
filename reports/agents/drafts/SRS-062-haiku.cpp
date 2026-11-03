#include "support.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "iqk/icd/gnss.hpp"
#include "iqk/timesync.hpp"

namespace {

using iqk::icd::gnss::TimePulse;
using iqk::timesync::Event;
using iqk::timesync::TimeSync;
using iqk::timesync::gps_to_utc_ns;
using iqk_test::kMs;
using iqk_test::kS;

constexpr std::uint16_t kWeek = 2300;
constexpr std::uint32_t kTow = 345'600'000;  // ms
constexpr std::int8_t kLeap = 18;
constexpr std::int64_t kEdge = 100 * kS;  // local monotonic time of the PPS edge

// A time message that passes every check; each test changes one field at a time.
TimePulse good_msg(std::uint32_t tow_ms = kTow) {
  TimePulse m{};
  m.week = kWeek;
  m.tow_ms = tow_ms;
  m.leap_s = kLeap;
  m.fix = 3;
  m.acc_ns = 50;
  m.set_time_valid(true);
  m.set_leap_valid(true);
  m.set_raim_ok(true);
  return m;
}

// Events since the last drain.
std::vector<Event> drain(TimeSync& ts) {
  std::vector<Event> ev;
  ts.drain_events([&](const Event& e) { ev.push_back(e); });
  return ev;
}

std::size_t n_kind(const std::vector<Event>& ev, Event::Kind k) {
  std::size_t n = 0;
  for (const Event& e : ev) {
    n += (e.kind == k) ? 1U : 0U;
  }
  return n;
}

// PPS edge at kEdge, then the time message arriving at local time `arrival`.
void feed(TimeSync& ts, const TimePulse& m, std::int64_t arrival) {
  ts.on_pps(kEdge);
  ts.on_gnss_time(m, arrival);
}

// On a fresh TimeSync, a used message sets the clock: one stepped event, and the edge is stamped with the
// UTC of the message, not with the arrival time.
void expect_used(TimeSync& ts, std::uint32_t tow_ms) {
  const auto ev = drain(ts);
  EXPECT_TRUE(ts.has_time());
  EXPECT_EQ(n_kind(ev, Event::Kind::gnss_rejected), 0U);
  EXPECT_EQ(n_kind(ev, Event::Kind::stepped), 1U);
  EXPECT_EQ(ts.utc_ns(kEdge), gps_to_utc_ns(kWeek, tow_ms, kLeap));
}

// The message is not used: the clock is untouched and a gnss_rejected event is reported.
void expect_rejected(TimeSync& ts) {
  const auto ev = drain(ts);
  EXPECT_FALSE(ts.has_time());
  EXPECT_EQ(n_kind(ev, Event::Kind::gnss_rejected), 1U);
  EXPECT_EQ(n_kind(ev, Event::Kind::stepped), 0U);
}

}  // namespace

TEST(GnssTimeSrs062, ValidMessageIsUsedAndStampsPpsEdge) {
  IQK_VERIFIES("SRS-062");
  TimeSync ts;
  // Arrives 500 ms after the edge: the clock must be anchored at the edge, not at the arrival.
  feed(ts, good_msg(), kEdge + 500 * kMs);
  expect_used(ts, kTow);
}

TEST(GnssTimeSrs062, TimeValidFlagIsRequired) {
  IQK_VERIFIES("SRS-062");
  {
    TimeSync ts;
    TimePulse m = good_msg();
    m.set_time_valid(false);
    feed(ts, m, kEdge + kMs);
    expect_rejected(ts);
  }
  {
    TimeSync ts;
    feed(ts, good_msg(), kEdge + kMs);
    expect_used(ts, kTow);
  }
}

TEST(GnssTimeSrs062, LeapValidFlagIsRequired) {
  IQK_VERIFIES("SRS-062");
  {
    TimeSync ts;
    TimePulse m = good_msg();
    m.set_leap_valid(false);
    feed(ts, m, kEdge + kMs);
    expect_rejected(ts);
  }
  {
    TimeSync ts;
    feed(ts, good_msg(), kEdge + kMs);
    expect_used(ts, kTow);
  }
}

TEST(GnssTimeSrs062, FixMustBe2DOr3D) {
  IQK_VERIFIES("SRS-062");
  // No fix (0), 1 (not a 2D or 3D fix), and values outside the ICD range (4, 255) are not used.
  for (const std::uint8_t fix : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{4}, std::uint8_t{255}}) {
    SCOPED_TRACE(::testing::Message() << "fix=" << static_cast<int>(fix));
    TimeSync ts;
    TimePulse m = good_msg();
    m.fix = fix;
    feed(ts, m, kEdge + kMs);
    expect_rejected(ts);
  }
  // 2D (2) and 3D (3) are used.
  for (const std::uint8_t fix : {std::uint8_t{2}, std::uint8_t{3}}) {
    SCOPED_TRACE(::testing::Message() << "fix=" << static_cast<int>(fix));
    TimeSync ts;
    TimePulse m = good_msg();
    m.fix = fix;
    feed(ts, m, kEdge + kMs);
    expect_used(ts, kTow);
  }
}

TEST(GnssTimeSrs062, AccuracyBoundaryIsGnssMaxAcc200Ns) {
  IQK_VERIFIES("SRS-062");
  // timing.gnss_max_acc_ns = 200: 200 ns is within, 201 ns is not.
  for (const std::uint32_t acc : {0U, 199U, 200U}) {
    SCOPED_TRACE(::testing::Message() << "acc_ns=" << acc);
    TimeSync ts;
    TimePulse m = good_msg();
    m.acc_ns = acc;
    feed(ts, m, kEdge + kMs);
    expect_used(ts, kTow);
  }
  for (const std::uint32_t acc : {201U, 1000U, 0xFFFFFFFFU}) {
    SCOPED_TRACE(::testing::Message() << "acc_ns=" << acc);
    TimeSync ts;
    TimePulse m = good_msg();
    m.acc_ns = acc;
    feed(ts, m, kEdge + kMs);
    expect_rejected(ts);
  }
}

TEST(GnssTimeSrs062, MessageDelayBoundaryIsGnssMaxMsgDelay900Ms) {
  IQK_VERIFIES("SRS-062");
  // timing.gnss_max_msg_delay_ms = 900: arrival up to 900 ms after the edge is used, one nanosecond more is not.
  {
    TimeSync ts;
    feed(ts, good_msg(), kEdge + 900 * kMs);
    expect_used(ts, kTow);
  }
  {
    TimeSync ts;
    feed(ts, good_msg(), kEdge + 900 * kMs + 1);
    expect_rejected(ts);
  }
  // A message stamped before its own edge does not belong to that edge.
  {
    TimeSync ts;
    feed(ts, good_msg(), kEdge - 1);
    expect_rejected(ts);
  }
  {
    TimeSync ts;
    feed(ts, good_msg(), kEdge);
    expect_used(ts, kTow);
  }
}

TEST(GnssTimeSrs062, MessageWithoutPpsEdgeIsNotUsed) {
  IQK_VERIFIES("SRS-062");
  TimeSync ts;
  ts.on_gnss_time(good_msg(), kEdge + kMs);
  expect_rejected(ts);
}

TEST(GnssTimeSrs062, EachEdgeIsUsedOnce) {
  IQK_VERIFIES("SRS-062");
  TimeSync ts;
  feed(ts, good_msg(kTow), kEdge + 100 * kMs);
  expect_used(ts, kTow);

  // A second message for the same edge (no new PPS) is not used. If it were, it would be measured against the
  // same edge and show up as a 1 ms offset in last_offset_ns(), which stays 0 here.
  ts.on_gnss_time(good_msg(kTow + 1), kEdge + 200 * kMs);
  const auto ev = drain(ts);
  EXPECT_EQ(n_kind(ev, Event::Kind::gnss_rejected), 1U);
  EXPECT_EQ(n_kind(ev, Event::Kind::stepped), 0U);
  EXPECT_EQ(ts.last_offset_ns(), 0);

  // A new edge with its own message is used: the message is 1 ms ahead of the clock, which is measured as 1 ms
  // offset (within the step threshold, so not stepped).
  const std::int64_t edge2 = kEdge + kS;
  ts.on_pps(edge2);
  ts.on_gnss_time(good_msg(kTow + 1001), edge2 + 100 * kMs);
  const auto ev2 = drain(ts);
  EXPECT_EQ(n_kind(ev2, Event::Kind::gnss_rejected), 0U);
  EXPECT_EQ(ts.last_offset_ns(), kMs);
}
