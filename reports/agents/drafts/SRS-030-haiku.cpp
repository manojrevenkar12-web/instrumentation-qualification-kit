// SRS-030 module supervision: a module without a heartbeat for aam.heartbeat_timeout_ms is reported lost (an
// aam_lost fault, logged once per loss) and fails the module built-in test (Bit::aam, DAU-UDP diag_status.bit).
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "iqk/can.hpp"
#include "iqk/config.hpp"
#include "iqk/dau.hpp"
#include "iqk/faultlog.hpp"
#include "iqk/plan.hpp"
#include "support.hpp"

namespace {

using iqk_test::kMs;
using iqk_test::kS;

constexpr std::int64_t kT0 = 10 * kS;
constexpr std::int64_t kTimeout = iqk::cfg::aam::heartbeat_timeout_ms * kMs;
constexpr std::uint32_t kAamBit = 0x02U;  // Bit::aam in DAU-UDP diag_status.bit

constexpr std::string_view kOneModule =
    "iqk-plan 1\ncampaign sup\nchannel a0 aam:1:0 rate=100 range_mv=1000\n";
constexpr std::string_view kTwoModules =
    "iqk-plan 1\ncampaign sup\nchannel a0 aam:1:0 rate=100 range_mv=1000\n"
    "channel b0 aam:2:0 rate=100 range_mv=1000\n";

const iqk::Tx kSink = [](iqk::Port, std::span<const std::uint8_t>) {};

// Loads a plan that uses the modules it names; the loader's module list is what supervision covers.
bool load(iqk::Dau& d, std::string_view text, std::int64_t now) {
  std::vector<iqk::plan::Finding> findings;
  return d.load_plan(text, now, findings);
}

// Delivers one CAN frame from the bus to the unit at local time `now`.
void feed(iqk::Dau& d, const iqk::CanFrame& f, std::int64_t now) {
  std::array<std::uint8_t, iqk::kCanUdpSize> bytes{};
  ASSERT_EQ(iqk::encode_can_udp(f, bytes), iqk::kCanUdpSize);
  d.on_input(iqk::Port::can, bytes, now, kSink);
}

// Runs the unit's periodic work at every `step` from `from` to `to`, inclusive.
void tick_every(iqk::Dau& d, std::int64_t from, std::int64_t to, std::int64_t step) {
  for (std::int64_t t = from; t <= to; t += step) {
    d.tick(t, kSink);
  }
}

// Detail (the node number) of the first aam_lost record, or -1 when there is none.
std::int64_t lost_detail(const iqk::FaultLog& log) {
  for (std::size_t i = 0; i < log.size(); ++i) {
    if (log[i].code == iqk::Fault::aam_lost) {
      return log[i].detail;
    }
  }
  return -1;
}

}  // namespace

TEST(ModuleSupervision, ModuleIsAliveUpToAndIncludingTheTimeout) {
  IQK_VERIFIES("SRS-030");
  iqk::Dau d{0x1234U};
  ASSERT_TRUE(load(d, kOneModule, kT0));
  feed(d, iqk_test::heartbeat(1, 2), kT0);

  d.tick(kT0 + kTimeout - kMs, kSink);
  EXPECT_TRUE(d.bit(kT0 + kTimeout - kMs).aam);

  // Exactly the timeout since the last heartbeat is not yet a loss; the requirement is "without a heartbeat for".
  d.tick(kT0 + kTimeout, kSink);
  EXPECT_TRUE(d.bit(kT0 + kTimeout).aam);
  EXPECT_EQ(d.faults().count(iqk::Fault::aam_lost), 0U);
}

TEST(ModuleSupervision, ModuleIsReportedLostOnceTheTimeoutIsExceeded) {
  IQK_VERIFIES("SRS-030");
  iqk::Dau d{0x1234U};
  ASSERT_TRUE(load(d, kOneModule, kT0));
  feed(d, iqk_test::heartbeat(1, 2), kT0);

  const std::int64_t late = kT0 + kTimeout + 1;  // 1 ns past the timeout
  d.tick(late, kSink);
  EXPECT_FALSE(d.bit(late).aam);
  EXPECT_EQ(d.faults().count(iqk::Fault::aam_lost), 1U);
  EXPECT_EQ(lost_detail(d.faults()), 1);
}

TEST(ModuleSupervision, TimeoutIsMeasuredFromTheLastHeartbeat) {
  IQK_VERIFIES("SRS-030");
  iqk::Dau d{0x1234U};
  ASSERT_TRUE(load(d, kOneModule, kT0));
  feed(d, iqk_test::heartbeat(1, 2), kT0);
  feed(d, iqk_test::heartbeat(1, 2), kT0 + 2 * kS);

  // Past the timeout of the first heartbeat, but not of the second: still alive.
  const std::int64_t mid = kT0 + kTimeout + 1;
  d.tick(mid, kSink);
  EXPECT_TRUE(d.bit(mid).aam);
  EXPECT_EQ(d.faults().count(iqk::Fault::aam_lost), 0U);

  // Past the timeout of the second heartbeat: lost.
  const std::int64_t late = kT0 + 2 * kS + kTimeout + 1;
  d.tick(late, kSink);
  EXPECT_FALSE(d.bit(late).aam);
  EXPECT_EQ(d.faults().count(iqk::Fault::aam_lost), 1U);
}

TEST(ModuleSupervision, LossIsLoggedOnceNotOncePerTick) {
  IQK_VERIFIES("SRS-030");
  iqk::Dau d{0x1234U};
  ASSERT_TRUE(load(d, kOneModule, kT0));
  feed(d, iqk_test::heartbeat(1, 2), kT0);

  // Ticks every 100 ms for 20 s, well past the timeout the whole time.
  tick_every(d, kT0, kT0 + 20 * kS, 100 * kMs);
  EXPECT_EQ(d.faults().count(iqk::Fault::aam_lost), 1U);
  EXPECT_EQ(lost_detail(d.faults()), 1);
  EXPECT_FALSE(d.bit(kT0 + 20 * kS).aam);
}

TEST(ModuleSupervision, EachLossIsLoggedAgainAfterRecovery) {
  IQK_VERIFIES("SRS-030");
  iqk::Dau d{0x1234U};
  ASSERT_TRUE(load(d, kOneModule, kT0));
  feed(d, iqk_test::heartbeat(1, 2), kT0);

  const std::int64_t lost1 = kT0 + kTimeout + 1;
  d.tick(lost1, kSink);
  ASSERT_EQ(d.faults().count(iqk::Fault::aam_lost), 1U);

  // The module comes back: its built-in test passes again.
  const std::int64_t back = lost1 + 100 * kMs;
  feed(d, iqk_test::heartbeat(1, 2), back);
  d.tick(back, kSink);
  EXPECT_TRUE(d.bit(back).aam);
  EXPECT_EQ(d.faults().count(iqk::Fault::aam_lost), 1U);

  // A second silence is a second loss, and is logged.
  const std::int64_t lost2 = back + kTimeout + 1;
  d.tick(lost2, kSink);
  EXPECT_FALSE(d.bit(lost2).aam);
  EXPECT_EQ(d.faults().count(iqk::Fault::aam_lost), 2U);
}

TEST(ModuleSupervision, OnlyTheSilentModuleIsReportedLost) {
  IQK_VERIFIES("SRS-030");
  iqk::Dau d{0x1234U};
  ASSERT_TRUE(load(d, kTwoModules, kT0));
  feed(d, iqk_test::heartbeat(1, 2), kT0);
  feed(d, iqk_test::heartbeat(2, 2), kT0);

  // Node 1 keeps its heartbeat every second; node 2 falls silent after the first one.
  for (int k = 1; k <= 3; ++k) {
    const std::int64_t t = kT0 + k * kS;
    feed(d, iqk_test::heartbeat(1, 2), t);
    d.tick(t, kSink);
  }
  const std::int64_t late = kT0 + kTimeout + 1;  // node 2 is 1 ns over; node 1 is 0.5 s since its last
  d.tick(late, kSink);
  EXPECT_FALSE(d.bit(late).aam);
  EXPECT_EQ(d.faults().count(iqk::Fault::aam_lost), 1U);
  EXPECT_EQ(lost_detail(d.faults()), 2);
}

TEST(ModuleSupervision, BuiltInTestFailsOnlyWhileTheModuleIsLost) {
  IQK_VERIFIES("SRS-030");
  iqk::Dau d{0x1234U};
  ASSERT_TRUE(load(d, kOneModule, kT0));
  feed(d, iqk_test::heartbeat(1, 2), kT0);

  const std::int64_t ok = kT0 + kTimeout;
  EXPECT_TRUE(d.bit(ok).aam);
  EXPECT_NE(static_cast<std::uint32_t>(d.status(ok).bit) & kAamBit, 0U);

  const std::int64_t late = kT0 + kTimeout + 1;
  EXPECT_FALSE(d.bit(late).aam);
  EXPECT_EQ(static_cast<std::uint32_t>(d.status(late).bit) & kAamBit, 0U);
}

TEST(ModuleSupervision, MalformedHeartbeatDoesNotKeepTheModuleAlive) {
  IQK_VERIFIES("SRS-030");
  iqk::Dau d{0x1234U};
  ASSERT_TRUE(load(d, kOneModule, kT0));
  feed(d, iqk_test::heartbeat(1, 2), kT0);
  // State 4 is outside the ICD range 0..3: the frame is an error, not a heartbeat.
  feed(d, iqk_test::heartbeat(1, 4), kT0 + 2 * kS);

  const std::int64_t late = kT0 + kTimeout + 1;
  d.tick(late, kSink);
  EXPECT_FALSE(d.bit(late).aam);
  EXPECT_EQ(d.faults().count(iqk::Fault::aam_lost), 1U);
}

TEST(ModuleSupervision, ModuleThatNeverSentAHeartbeatIsReportedLost) {
  IQK_VERIFIES("SRS-030");
  iqk::Dau d{0x1234U};
  ASSERT_TRUE(load(d, kOneModule, kT0));

  // Silent from the start: after many timeouts it is failed, and the loss is logged once.
  tick_every(d, kT0, kT0 + 10 * kS, 100 * kMs);
  EXPECT_FALSE(d.bit(kT0 + 10 * kS).aam);
  EXPECT_EQ(d.faults().count(iqk::Fault::aam_lost), 1U);
}
