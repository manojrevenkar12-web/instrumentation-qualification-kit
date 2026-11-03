// SRS-048: the plan's worst-case CAN bus load, with bit stuffing, does not exceed plan.max_can_bus_load (PLN-11).
//
// Load model under test (plan::can_bus_load): per AAM module one heartbeat at 1 Hz, and per channel group (inputs
// 0..2, 3..5, 6..8, 9..11) one data frame at the group's rate. Each frame is costed by plan::can_frame_bits, the
// worst case with bit stuffing, on a 500 kbit/s bus. IMU traffic is RS-422 serial (docs/icd/imu.md), not CAN.
//
// Boundary arithmetic (bits per second on the bus):
//   one module, groups at 1000+1000+200+20 Hz:  95 + 135*2220           = 299795 bit/s -> 0.59959  (accepted)
//   the same plus one 10 Hz channel on a 2nd module: 2*95 + 135*2230    = 301240 bit/s -> 0.60248  (rejected)
// An exact load of 0.60 cannot be reached by any valid plan (it would need 6 modules and 2218 Hz of group rate,
// and group rates are multiples of 10 Hz), so the limit is bracketed from both sides.

#include "support.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "iqk/config.hpp"
#include "iqk/plan.hpp"

namespace {

using iqk::plan::Finding;
using iqk::plan::Plan;

// One AAM channel of a test plan: module node, input 0..11, sample rate in Hz.
struct Ch {
  int node;
  int input;
  int rate;
};

std::optional<Plan> parse_plan(const std::vector<Ch>& chs, std::vector<Finding>& findings) {
  std::string text = "iqk-plan 1\ncampaign can-load\n";
  int n = 0;
  for (const Ch& c : chs) {
    text += "channel c" + std::to_string(n++) + " aam:" + std::to_string(c.node) + ":" + std::to_string(c.input) +
            " rate=" + std::to_string(c.rate) + " range_mv=1000\n";
  }
  return iqk::plan::parse(text, findings);
}

bool has_rule(const std::vector<Finding>& findings, const char* rule) {
  for (const Finding& f : findings) {
    if (f.rule == rule) {
      return true;
    }
  }
  return false;
}

// Expected load in bit/s over the bus bit rate, from the frame sizes, so the tests do not depend on the code under test.
double expected_load(std::int64_t bits_per_s) {
  return static_cast<double>(bits_per_s) / static_cast<double>(iqk::icd::aam::kBitrate);
}

constexpr double kLoadTol = 1e-12;

}  // namespace

TEST(CanBusLoad, WorstCaseFrameBitsIncludeBitStuffing) {
  IQK_VERIFIES("SRS-048");
  // Worst case for an 11-bit identifier: 8-byte frame 135 bits, 4-byte frame 95 bits (Davis et al. 2007).
  EXPECT_EQ(iqk::plan::can_frame_bits(8), 135);
  EXPECT_EQ(iqk::plan::can_frame_bits(4), 95);
  // The stuffing term is present: without it an 8-byte frame would be 8*8+47 = 111 bits.
  EXPECT_GT(iqk::plan::can_frame_bits(8), 8 * 8 + 47);
  EXPECT_GT(iqk::plan::can_frame_bits(4), 8 * 4 + 47);
}

TEST(CanBusLoad, SingleChannelCountsHeartbeatAndOneDataFrame) {
  IQK_VERIFIES("SRS-048");
  std::vector<Finding> f;
  const auto p = parse_plan({{1, 0, 10}}, f);
  ASSERT_TRUE(p.has_value());
  // Module 1: heartbeat at 1 Hz (95 bits) plus the group data frame at 10 Hz (10 * 135 bits).
  EXPECT_NEAR(iqk::plan::can_bus_load(*p), expected_load(95 + 10 * 135), kLoadTol);
}

TEST(CanBusLoad, ChannelsOfOneGroupShareOneDataFrame) {
  IQK_VERIFIES("SRS-048");
  // Inputs 0, 1 and 2 are one group: the data frame carries all three, so the load is that of one frame.
  std::vector<Finding> f1;
  std::vector<Finding> f3;
  const auto one = parse_plan({{1, 0, 100}}, f1);
  const auto three = parse_plan({{1, 0, 100}, {1, 1, 100}, {1, 2, 100}}, f3);
  ASSERT_TRUE(one.has_value());
  ASSERT_TRUE(three.has_value());
  EXPECT_NEAR(iqk::plan::can_bus_load(*three), expected_load(95 + 100 * 135), kLoadTol);
  EXPECT_NEAR(iqk::plan::can_bus_load(*three), iqk::plan::can_bus_load(*one), kLoadTol);
}

TEST(CanBusLoad, EachGroupAndEachModuleAddsItsOwnFrames) {
  IQK_VERIFIES("SRS-048");
  // Input 0 (group 0) and input 3 (group 1) of module 1 are two data frames; module 2 adds its own heartbeat.
  std::vector<Finding> f;
  const auto p = parse_plan({{1, 0, 50}, {1, 3, 50}, {2, 0, 50}}, f);
  ASSERT_TRUE(p.has_value());
  EXPECT_NEAR(iqk::plan::can_bus_load(*p), expected_load(2 * 95 + 3 * 50 * 135), kLoadTol);
}

TEST(CanBusLoad, ImuChannelsAreNotOnTheCanBus) {
  IQK_VERIFIES("SRS-048");
  std::vector<Finding> f;
  const auto p = parse_plan({{1, 0, 10}}, f);
  ASSERT_TRUE(p.has_value());
  const double aam_only = iqk::plan::can_bus_load(*p);

  std::string text = "iqk-plan 1\ncampaign can-load\nchannel c0 aam:1:0 rate=10 range_mv=1000\n"
                     "channel i0 imu:gyro_x rate=400\nchannel i1 imu:accel_z rate=400\n";
  std::vector<Finding> fi;
  const auto mixed = iqk::plan::parse(text, fi);
  ASSERT_TRUE(mixed.has_value());
  EXPECT_NEAR(iqk::plan::can_bus_load(*mixed), aam_only, kLoadTol);
}

TEST(CanBusLoad, PlanAtTheLimitMinusMarginIsAccepted) {
  IQK_VERIFIES("SRS-048");
  ASSERT_DOUBLE_EQ(iqk::cfg::plan::max_can_bus_load, 0.60) << "boundary cases below assume the 0.60 limit";
  // One module, four groups: 1000 + 1000 + 200 + 20 Hz -> 299795 bit/s, 0.59959 of the bus.
  std::vector<Finding> f;
  const auto p = parse_plan({{1, 0, 1000}, {1, 3, 1000}, {1, 6, 200}, {1, 9, 20}}, f);
  ASSERT_TRUE(p.has_value());
  const double load = iqk::plan::can_bus_load(*p);
  EXPECT_NEAR(load, expected_load(95 + 2220 * 135), kLoadTol);
  EXPECT_LE(load, iqk::cfg::plan::max_can_bus_load);
  std::vector<Finding> v;
  (void)iqk::plan::validate(*p, v);
  EXPECT_FALSE(has_rule(v, "PLN-11")) << "a plan under the CAN limit must not raise PLN-11";
}

TEST(CanBusLoad, PlanJustAboveTheLimitIsRejectedWithPln11) {
  IQK_VERIFIES("SRS-048");
  ASSERT_DOUBLE_EQ(iqk::cfg::plan::max_can_bus_load, 0.60) << "boundary cases below assume the 0.60 limit";
  // The accepted plan above plus one 10 Hz group on a second module: 301240 bit/s, 0.60248 of the bus.
  std::vector<Finding> f;
  const auto p = parse_plan({{1, 0, 1000}, {1, 3, 1000}, {1, 6, 200}, {1, 9, 20}, {2, 0, 10}}, f);
  ASSERT_TRUE(p.has_value());
  const double load = iqk::plan::can_bus_load(*p);
  EXPECT_NEAR(load, expected_load(2 * 95 + 2230 * 135), kLoadTol);
  EXPECT_GT(load, iqk::cfg::plan::max_can_bus_load);
  std::vector<Finding> v;
  EXPECT_FALSE(iqk::plan::validate(*p, v));
  EXPECT_TRUE(has_rule(v, "PLN-11"));
}

TEST(CanBusLoad, BitStuffingIsCountedAgainstTheLimit) {
  IQK_VERIFIES("SRS-048");
  // Three groups at 1000, 1000 and 500 Hz on one module: 2500 Hz of data frames.
  // Without stuffing the load would be (79 + 111*2500)/500000 = 0.5552 (accepted);
  // with stuffing it is (95 + 135*2500)/500000 = 0.67519, which must be rejected.
  std::vector<Finding> f;
  const auto p = parse_plan({{1, 0, 1000}, {1, 3, 1000}, {1, 6, 500}}, f);
  ASSERT_TRUE(p.has_value());
  EXPECT_NEAR(iqk::plan::can_bus_load(*p), expected_load(95 + 2500 * 135), kLoadTol);
  std::vector<Finding> v;
  EXPECT_FALSE(iqk::plan::validate(*p, v));
  EXPECT_TRUE(has_rule(v, "PLN-11"));
}
