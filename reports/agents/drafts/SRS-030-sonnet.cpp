#include "support.hpp"

#include <array>

#include "iqk/dau.hpp"

namespace {

using iqk::Fault;
using iqk::Port;
using iqk_test::kMs;
using iqk_test::kS;

constexpr std::int64_t kTimeout = iqk::cfg::aam::heartbeat_timeout_ms * kMs;

const iqk::Tx kNoTx = [](Port, std::span<const std::uint8_t>) {};

struct Rig {
  iqk::Dau dau{0x1122334455667788ULL};
  explicit Rig(bool two_nodes = false) {
    std::string text = "iqk-plan 1\ncampaign c1\nchannel a aam:1:0 rate=100 range_mv=1000\n";
    if (two_nodes) {
      text += "channel b aam:2:0 rate=100 range_mv=1000\n";
    }
    std::vector<iqk::plan::Finding> f;
    EXPECT_TRUE(dau.load_plan(text, 0, f));
  }
  void send(const iqk::CanFrame& f, std::int64_t t) {
    std::array<std::uint8_t, iqk::kCanUdpSize> b{};
    (void)iqk::encode_can_udp(f, b);
    dau.on_input(Port::can, b, t, kNoTx);
  }
  void hb(int node, std::int64_t t) { send(iqk_test::heartbeat(node, 2), t); }
};

}  // namespace

TEST(AgentSupervision, NotLostAtExactlyTimeout) {
  IQK_VERIFIES("SRS-030");
  Rig r;
  const std::int64_t t0 = 10 * kS;
  r.hb(1, t0);
  r.dau.tick(t0 + kTimeout, kNoTx);
  EXPECT_EQ(r.dau.faults().count(Fault::aam_lost), 0U);
  EXPECT_TRUE(r.dau.bit(t0 + kTimeout).aam);
}

TEST(AgentSupervision, LostJustAfterTimeout) {
  IQK_VERIFIES("SRS-030");
  Rig r;
  const std::int64_t t0 = 10 * kS;
  r.hb(1, t0);
  r.dau.tick(t0 + kTimeout + 1, kNoTx);
  EXPECT_EQ(r.dau.faults().count(Fault::aam_lost), 1U);
  EXPECT_FALSE(r.dau.bit(t0 + kTimeout + 1).aam);
}

TEST(AgentSupervision, HealthyBeforeTimeoutAndFaultRecordNamesNode) {
  IQK_VERIFIES("SRS-030");
  Rig r(true);
  const std::int64_t t0 = 10 * kS;
  r.hb(1, t0);
  r.hb(2, t0);
  for (std::int64_t t = t0; t <= t0 + kTimeout; t += 500 * kMs) {
    r.dau.tick(t, kNoTx);
  }
  EXPECT_TRUE(r.dau.bit(t0 + kTimeout).aam);
  EXPECT_EQ(r.dau.faults().count(Fault::aam_lost), 0U);
  // Only node 2 keeps beating; node 1 is lost.
  const std::int64_t t1 = t0 + kTimeout + 1 * kS;
  r.hb(2, t1);
  r.dau.tick(t1, kNoTx);
  ASSERT_EQ(r.dau.faults().count(Fault::aam_lost), 1U);
  const auto& rec = r.dau.faults()[r.dau.faults().size() - 1];
  EXPECT_EQ(rec.code, Fault::aam_lost);
  EXPECT_EQ(rec.detail, 1);
  EXPECT_FALSE(r.dau.bit(t1).aam);
}

TEST(AgentSupervision, LossLoggedOnlyOnceWhileSilent) {
  IQK_VERIFIES("SRS-030");
  Rig r;
  const std::int64_t t0 = 10 * kS;
  r.hb(1, t0);
  for (std::int64_t t = t0; t <= t0 + 30 * kS; t += 100 * kMs) {
    r.dau.tick(t, kNoTx);
  }
  EXPECT_EQ(r.dau.faults().count(Fault::aam_lost), 1U);
  EXPECT_EQ(r.dau.faults().total(), 1U);
  EXPECT_FALSE(r.dau.bit(t0 + 30 * kS).aam);
}

TEST(AgentSupervision, HeartbeatRestartsTimeout) {
  IQK_VERIFIES("SRS-030");
  Rig r;
  const std::int64_t t0 = 10 * kS;
  r.hb(1, t0);
  r.hb(1, t0 + 3 * kS);
  r.dau.tick(t0 + 3 * kS + kTimeout, kNoTx);
  EXPECT_EQ(r.dau.faults().count(Fault::aam_lost), 0U);
  EXPECT_TRUE(r.dau.bit(t0 + 3 * kS + kTimeout).aam);
  r.dau.tick(t0 + 3 * kS + kTimeout + 1, kNoTx);
  EXPECT_EQ(r.dau.faults().count(Fault::aam_lost), 1U);
}

TEST(AgentSupervision, DataFramesDoNotCountAsHeartbeat) {
  IQK_VERIFIES("SRS-030");
  Rig r;
  const std::int64_t t0 = 10 * kS;
  r.hb(1, t0);
  for (int i = 0; i < 40; ++i) {
    r.send(iqk_test::data_frame(1, 0, static_cast<std::uint8_t>(i), 1, 2, 3), t0 + i * 100 * kMs);
  }
  r.dau.tick(t0 + kTimeout + 1, kNoTx);
  EXPECT_EQ(r.dau.faults().count(Fault::aam_lost), 1U);
  EXPECT_FALSE(r.dau.bit(t0 + kTimeout + 1).aam);
}

TEST(AgentSupervision, RecoveryAndSecondLossIsReportedAgain) {
  IQK_VERIFIES("SRS-030");
  Rig r;
  const std::int64_t t0 = 10 * kS;
  r.hb(1, t0);
  r.dau.tick(t0 + kTimeout + 1, kNoTx);
  ASSERT_EQ(r.dau.faults().count(Fault::aam_lost), 1U);
  const std::int64_t t1 = t0 + 20 * kS;
  r.hb(1, t1);
  EXPECT_TRUE(r.dau.bit(t1).aam);
  r.dau.tick(t1 + kTimeout, kNoTx);
  EXPECT_EQ(r.dau.faults().count(Fault::aam_lost), 1U);
  r.dau.tick(t1 + kTimeout + 1, kNoTx);
  EXPECT_EQ(r.dau.faults().count(Fault::aam_lost), 2U);
  EXPECT_FALSE(r.dau.bit(t1 + kTimeout + 1).aam);
}
