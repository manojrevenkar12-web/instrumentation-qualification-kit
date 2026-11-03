#include <gtest/gtest.h>

#include <algorithm>

#include "iqk/plan.hpp"
#include "support.hpp"

namespace plan = iqk::plan;

namespace {
const char* kGood = R"(iqk-plan 1
campaign GVT-1
channel strain_a aam:1:0 rate=500 range_mv=10000 filter=lp_100hz scale=250 unit=ue alarm=-2000,2000
channel strain_b aam:1:1 rate=500 range_mv=10000
channel tc_bay   aam:1:3 rate=10 range_mv=100 filter=lp_10hz scale=2500 unit=degC
channel gyro_z   imu:gyro_z rate=400
)";

std::vector<std::string> rules(const std::string& text) {
  std::vector<plan::Finding> f;
  const auto p = plan::parse(text, f);
  if (p) {
    (void)plan::validate(*p, f);
  }
  std::vector<std::string> out;
  for (const auto& x : f) {
    out.push_back(x.rule);
  }
  return out;
}

bool has(const std::vector<std::string>& v, const char* r) { return std::find(v.begin(), v.end(), r) != v.end(); }

bool accepted(const std::string& text) {
  std::vector<plan::Finding> f;
  const auto p = plan::parse(text, f);
  return p.has_value() && plan::validate(*p, f);
}

std::string with(const std::string& extra) { return std::string{kGood} + extra + "\n"; }
}  // namespace

TEST(Plan, AGoodPlanIsAccepted) {
  IQK_VERIFIES("SRS-040");
  EXPECT_TRUE(rules(kGood).empty());
}

TEST(Plan, SyntaxIsStrict) {
  IQK_VERIFIES("SRS-040");
  EXPECT_TRUE(has(rules("iqk-plan 2\ncampaign x\n"), "PLN-01"));
  EXPECT_TRUE(has(rules("campaign x\n"), "PLN-01"));
  EXPECT_TRUE(has(rules(with("channel x aam:1:4 rate=10 range_mv=100 colour=red")), "PLN-02"));
  EXPECT_TRUE(has(rules(with("channel x aam:1:4 rate=10 rate=20 range_mv=100")), "PLN-02"));
  EXPECT_TRUE(has(rules(with("channel x aam:1:4 rate=10Hz range_mv=100")), "PLN-02"));
  EXPECT_TRUE(has(rules(with("channel x aam:1:4 rate=nan range_mv=100")), "PLN-02"));
  EXPECT_TRUE(has(rules(with("channel x aam:1:4 rate=10")), "PLN-02"));       // range_mv missing
  EXPECT_TRUE(has(rules(with("channel x imu:temp rate=50 range_mv=100")), "PLN-02"));  // not an IMU key
  EXPECT_TRUE(has(rules(with("campaign again")), "PLN-02"));
  EXPECT_TRUE(has(rules(with("sample everything")), "PLN-02"));
  // A finding is not enough: the plan must be refused.
  EXPECT_FALSE(accepted(with("channel x aam:1:4 rate=10 range_mv=100 colour=red")));
  EXPECT_FALSE(accepted(with("channel x aam:1:4 rate=10 rate=20 range_mv=100")));
  EXPECT_FALSE(accepted(with("channel x aam:1:4 rate=10Hz range_mv=100")));
  EXPECT_TRUE(accepted(with("channel x aam:1:4 rate=10 range_mv=100")));
}

TEST(Plan, NamesAndCount) {
  IQK_VERIFIES("SRS-041");
  EXPECT_TRUE(has(rules(with("channel strain_a aam:1:4 rate=10 range_mv=100")), "PLN-03"));
  EXPECT_TRUE(has(rules(with("channel Strain aam:1:4 rate=10 range_mv=100")), "PLN-03"));
  EXPECT_TRUE(has(rules(with("channel a_name_that_is_far_too_long aam:1:4 rate=10 range_mv=100")), "PLN-03"));
  std::string many = "iqk-plan 1\ncampaign big\n";
  for (int n = 1; n <= 5; ++n) {
    for (int i = 0; i < 12; ++i) {
      many += "channel c" + std::to_string(n) + "_" + std::to_string(i) + " aam:" + std::to_string(n) + ":" +
              std::to_string(i) + " rate=10 range_mv=100\n";
    }
  }
  EXPECT_TRUE(has(rules(many), "PLN-04"));  // 60 channels > 48
}

TEST(Plan, SourcesMustExist) {
  IQK_VERIFIES("SRS-042");
  EXPECT_TRUE(has(rules(with("channel x aam:33:0 rate=10 range_mv=100")), "PLN-05"));
  EXPECT_TRUE(has(rules(with("channel x aam:2:12 rate=10 range_mv=100")), "PLN-05"));
  EXPECT_TRUE(has(rules(with("channel x imu:mag_x rate=50")), "PLN-05"));
  EXPECT_FALSE(has(rules(with("channel x aam:32:11 rate=10 range_mv=100")), "PLN-05"));
}

TEST(Plan, RatesMustBeAchievable) {
  IQK_VERIFIES("SRS-043");
  EXPECT_TRUE(has(rules(with("channel x aam:2:0 rate=300 range_mv=100")), "PLN-06"));   // 1000/300 is not a divisor
  EXPECT_FALSE(has(rules(with("channel x aam:2:0 rate=200 range_mv=100")), "PLN-06"));
  EXPECT_FALSE(has(rules(with("channel x aam:2:0 rate=10 range_mv=100")), "PLN-06"));
  EXPECT_TRUE(has(rules(with("channel x imu:temp rate=25")), "PLN-06"));
  EXPECT_TRUE(has(rules(with("channel x imu:temp rate=100.5")), "PLN-06"));
  EXPECT_FALSE(has(rules(with("channel x imu:temp rate=50")), "PLN-06"));
}

TEST(Plan, RangesAndFiltersMustExist) {
  IQK_VERIFIES("SRS-044");
  EXPECT_TRUE(has(rules(with("channel x aam:2:0 rate=10 range_mv=2000")), "PLN-07"));
  EXPECT_TRUE(has(rules(with("channel x aam:2:0 rate=10 range_mv=100 filter=lp_1khz")), "PLN-07"));
  EXPECT_FALSE(has(rules(with("channel x aam:2:0 rate=10 range_mv=5000 filter=lp_10hz")), "PLN-07"));
}

TEST(Plan, InputsSharingADataFrameShareARate) {
  IQK_VERIFIES("SRS-045");
  // Inputs 0..2 of a module travel in one frame; strain_a and strain_b already run at 500 Hz.
  EXPECT_TRUE(has(rules(with("channel x aam:1:2 rate=100 range_mv=100")), "PLN-08"));
  EXPECT_FALSE(has(rules(with("channel x aam:1:2 rate=500 range_mv=100")), "PLN-08"));
  EXPECT_FALSE(has(rules(with("channel x aam:1:5 rate=10 range_mv=100")), "PLN-08"));  // same group as tc_bay, same rate
}

TEST(Plan, AnInputIsUsedOnce) {
  IQK_VERIFIES("SRS-046");
  EXPECT_TRUE(has(rules(with("channel strain_c aam:1:0 rate=500 range_mv=10000")), "PLN-09"));
  EXPECT_TRUE(has(rules(with("channel gyro_again imu:gyro_z rate=400")), "PLN-09"));
}

TEST(Plan, AlarmsMustLieInsideWhatTheChannelMeasures) {
  IQK_VERIFIES("SRS-047");
  // +/-10 V at 250 ue/V measures +/-2500 ue.
  EXPECT_FALSE(has(rules(with("channel x aam:2:0 rate=10 range_mv=10000 scale=250 alarm=-2500,2500")), "PLN-10"));
  EXPECT_TRUE(has(rules(with("channel x aam:2:0 rate=10 range_mv=10000 scale=250 alarm=-2600,2500")), "PLN-10"));
  EXPECT_TRUE(has(rules(with("channel x aam:2:0 rate=10 range_mv=10000 scale=-250 alarm=-2500,2501")), "PLN-10"));
  EXPECT_FALSE(has(rules(with("channel x aam:2:0 rate=10 range_mv=10000 scale=-250 alarm=-2500,2500")), "PLN-10"));
  EXPECT_FALSE(has(rules(with("channel x aam:2:0 rate=10 range_mv=1000 scale=10 offset=50 alarm=40,60")), "PLN-10"));
  EXPECT_TRUE(has(rules(with("channel x aam:2:0 rate=10 range_mv=1000 scale=10 offset=50 alarm=39,60")), "PLN-10"));
  EXPECT_TRUE(has(rules(with("channel x aam:2:0 rate=10 range_mv=10000 alarm=5,5")), "PLN-10"));
  EXPECT_TRUE(has(rules(with("channel x imu:temp rate=50 alarm=-50,60")), "PLN-10"));  // IMU temperature: -40..85
  EXPECT_FALSE(has(rules(with("channel x imu:temp rate=50 alarm=-40,85")), "PLN-10"));
}

TEST(Plan, ScaleMustNotBeZero) {
  IQK_VERIFIES("SRS-047");
  EXPECT_TRUE(has(rules(with("channel x aam:2:0 rate=10 range_mv=100 scale=0")), "PLN-13"));
}

TEST(Plan, CanFrameBitsIncludeWorstCaseStuffing) {
  IQK_VERIFIES("SRS-048");
  EXPECT_EQ(plan::can_frame_bits(8), 135);
  EXPECT_EQ(plan::can_frame_bits(4), 95);
  EXPECT_EQ(plan::can_frame_bits(0), 55);
}

TEST(Plan, BusLoadIsWorstCase) {
  IQK_VERIFIES("SRS-048");
  // Two groups at 1 kHz on module 1 and one at 500 Hz on module 2: 67.5 % with stuffing (55.5 % without).
  const std::string text = R"(iqk-plan 1
campaign load
channel a aam:1:0 rate=1000 range_mv=100
channel b aam:1:3 rate=1000 range_mv=100
channel c aam:2:0 rate=500 range_mv=100
)";
  std::vector<plan::Finding> f;
  const auto p = plan::parse(text, f);
  ASSERT_TRUE(p);
  EXPECT_NEAR(plan::can_bus_load(*p), (2 * 135'000.0 + 67'500.0 + 2 * 95.0) / 500'000.0, 1e-9);
  EXPECT_TRUE(has(rules(text), "PLN-11"));
  EXPECT_FALSE(has(rules(kGood), "PLN-11"));
}

TEST(Plan, TelemetryMustFitTheLink) {
  IQK_VERIFIES("SRS-049");
  std::string text = "iqk-plan 1\ncampaign fast\n";
  // 16 modules x one 1 kHz group: 48 k samples/s over the link (the bus rule fails this plan; this test is the link).
  for (int n = 1; n <= 16; ++n) {
    for (int i = 0; i < 3; ++i) {
      text += "channel m" + std::to_string(n) + "_" + std::to_string(i) + " aam:" + std::to_string(n) + ":" +
              std::to_string(i) + " rate=1000 range_mv=100\n";
    }
  }
  std::vector<plan::Finding> f;
  const auto p = plan::parse(text, f);
  ASSERT_TRUE(p);
  EXPECT_GT(plan::telemetry_bps(*p), 3'000'000.0);
  EXPECT_TRUE(has(rules(text), "PLN-12"));  // the 2 Mbit/s radio cannot carry it
  EXPECT_FALSE(has(rules(kGood), "PLN-12"));
  const double expected = 8.0 * (48'000.0 * 9 + 100.0 * 10 * (22 + 4 + 66));  // 480 samples/frame: 10 packets
  EXPECT_NEAR(plan::telemetry_bps(*p), expected, 1.0);
}

TEST(Plan, CanonicalFormIgnoresLayout) {
  IQK_VERIFIES("SRS-054");
  std::vector<plan::Finding> f;
  const auto a = plan::parse(kGood, f);
  const auto b = plan::parse(
      "  iqk-plan 1   # header\n\ncampaign GVT-1\nchannel strain_a aam:1:0 alarm=-2000,2000 unit=ue scale=250 "
      "filter=lp_100hz range_mv=10000 rate=500.0\nchannel strain_b aam:1:1 rate=5e2 range_mv=10000\n"
      "channel tc_bay   aam:1:3 rate=10 range_mv=100 filter=lp_10hz scale=2500 unit=degC\nchannel gyro_z imu:gyro_z rate=400\n",
      f);
  ASSERT_TRUE(a && b);
  EXPECT_EQ(plan::canonical(*a), plan::canonical(*b));
  EXPECT_EQ(plan::crc(*a), plan::crc(*b));
  auto c = *a;
  c.channels[0].alarm->second = 1999;
  EXPECT_NE(plan::crc(*a), plan::crc(c));
}

TEST(Plan, RegistersConfigureEveryInputOfAUsedModule) {
  IQK_VERIFIES("SRS-052");
  std::vector<plan::Finding> f;
  const auto p = plan::parse(kGood, f);
  ASSERT_TRUE(p);
  const auto regs = plan::registers(*p);
  EXPECT_EQ(regs.size(), 12U * 4U);  // module 1 only
  const auto find = [&](std::uint16_t idx, std::uint8_t sub) {
    return std::find_if(regs.begin(), regs.end(), [&](const plan::RegWrite& r) { return r.index == idx && r.sub == sub; })->value;
  };
  EXPECT_EQ(find(0x2000, 1), 1U);  // input 0 enabled
  EXPECT_EQ(find(0x2000, 2), 3U);  // 10000 mV is range code 3
  EXPECT_EQ(find(0x2000, 3), 2U);  // 500 Hz is divisor 2
  EXPECT_EQ(find(0x2000, 4), 1U);  // lp_100hz
  EXPECT_EQ(find(0x2002, 1), 0U);  // input 2 disabled
  EXPECT_EQ(find(0x2003, 3), 100U);
}
