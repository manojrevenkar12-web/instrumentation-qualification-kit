#include "support.hpp"

#include <algorithm>

#include "iqk/plan.hpp"

namespace {

using iqk::plan::Finding;

std::vector<Finding> check(const std::string& channel_line) {
  std::vector<Finding> f;
  const std::string text = "iqk-plan 1\ncampaign c1\n" + channel_line + "\n";
  auto p = iqk::plan::parse(text, f);
  EXPECT_TRUE(p.has_value());
  EXPECT_TRUE(f.empty());
  if (!p) {
    return {{"PARSE", 0, "parse failed"}};
  }
  (void)iqk::plan::validate(*p, f);
  return f;
}

bool has(const std::vector<Finding>& f, const std::string& rule) {
  return std::any_of(f.begin(), f.end(), [&](const Finding& x) { return x.rule == rule; });
}

// Default: range 1000 mV, scale 2.5, offset 1 -> measurable -1.5 .. 3.5 (exact in binary floating point)
std::string aam(const std::string& alarm, const std::string& scale = "2.5", const std::string& offset = "1",
                const std::string& range = "1000") {
  return "channel ch1 aam:1:0 rate=100 range_mv=" + range + " scale=" + scale + " offset=" + offset +
         (alarm.empty() ? "" : " alarm=" + alarm);
}

}  // namespace

TEST(Srs047, ValidAlarmInsideRangeAccepted) {
  IQK_VERIFIES("SRS-047");
  EXPECT_TRUE(check(aam("0,2")).empty());
}

TEST(Srs047, NoAlarmIsNotChecked) {
  IQK_VERIFIES("SRS-047");
  EXPECT_FALSE(has(check(aam("")), "PLN-10"));
}

TEST(Srs047, AlarmExactlyAtMeasurableLimitsAccepted) {
  IQK_VERIFIES("SRS-047");
  EXPECT_TRUE(check(aam("-1.5,3.5")).empty());
}

TEST(Srs047, AlarmLowBelowMeasurableRejected) {
  IQK_VERIFIES("SRS-047");
  EXPECT_TRUE(has(check(aam("-1.51,2")), "PLN-10"));
  EXPECT_TRUE(has(check(aam("-100,2")), "PLN-10"));
}

TEST(Srs047, AlarmHighAboveMeasurableRejected) {
  IQK_VERIFIES("SRS-047");
  EXPECT_TRUE(has(check(aam("0,3.51")), "PLN-10"));
  EXPECT_TRUE(has(check(aam("0,100")), "PLN-10"));
}

TEST(Srs047, AlarmBothBeyondSameSideRejected) {
  IQK_VERIFIES("SRS-047");
  EXPECT_TRUE(has(check(aam("4,5")), "PLN-10"));
  EXPECT_TRUE(has(check(aam("-5,-4")), "PLN-10"));
}

TEST(Srs047, AlarmMustBeOrdered) {
  IQK_VERIFIES("SRS-047");
  EXPECT_TRUE(has(check(aam("2,0")), "PLN-10"));
  EXPECT_TRUE(has(check(aam("1,1")), "PLN-10"));
  EXPECT_TRUE(has(check(aam("-1.5,-1.5")), "PLN-10"));
  EXPECT_FALSE(has(check(aam("1,1.001")), "PLN-10"));
}

TEST(Srs047, MeasurableRangeFollowsOffset) {
  IQK_VERIFIES("SRS-047");
  // offset 10, scale 2.5, 1 V -> 7.5 .. 12.5
  EXPECT_TRUE(check(aam("7.5,12.5", "2.5", "10")).empty());
  EXPECT_TRUE(has(check(aam("7.4,12", "2.5", "10")), "PLN-10"));
  EXPECT_TRUE(has(check(aam("8,12.6", "2.5", "10")), "PLN-10"));
  EXPECT_TRUE(has(check(aam("-1,1", "2.5", "10")), "PLN-10"));  // would be fine with offset 0
  // offset -10 -> -12.5 .. -7.5
  EXPECT_TRUE(check(aam("-12.5,-7.5", "2.5", "-10")).empty());
  EXPECT_TRUE(has(check(aam("-12.6,-8", "2.5", "-10")), "PLN-10"));
  EXPECT_TRUE(has(check(aam("-12,-7.4", "2.5", "-10")), "PLN-10"));
}

TEST(Srs047, MeasurableRangeFollowsScale) {
  IQK_VERIFIES("SRS-047");
  // scale 5, offset 0, 1 V -> -5 .. 5
  EXPECT_TRUE(check(aam("-5,5", "5", "0")).empty());
  EXPECT_TRUE(has(check(aam("-5.1,5", "5", "0")), "PLN-10"));
  EXPECT_TRUE(has(check(aam("-5,5.1", "5", "0")), "PLN-10"));
  // scale 1 -> -1 .. 1: the same alarm is outside
  EXPECT_TRUE(has(check(aam("-5,5", "1", "0")), "PLN-10"));
  EXPECT_FALSE(has(check(aam("-1,1", "1", "0")), "PLN-10"));
}

TEST(Srs047, MeasurableRangeFollowsRange) {
  IQK_VERIFIES("SRS-047");
  // scale 1, offset 0: +-0.1 V, +-1 V, +-5 V, +-10 V
  EXPECT_TRUE(check(aam("-0.1,0.1", "1", "0", "100")).empty());
  EXPECT_TRUE(has(check(aam("-0.2,0.1", "1", "0", "100")), "PLN-10"));
  EXPECT_TRUE(has(check(aam("-0.1,0.2", "1", "0", "100")), "PLN-10"));
  EXPECT_TRUE(check(aam("-5,5", "1", "0", "5000")).empty());
  EXPECT_TRUE(has(check(aam("-5,5.5", "1", "0", "5000")), "PLN-10"));
  EXPECT_TRUE(check(aam("-10,10", "1", "0", "10000")).empty());
  EXPECT_TRUE(has(check(aam("-10.5,10", "1", "0", "10000")), "PLN-10"));
  EXPECT_TRUE(has(check(aam("-1,1.5", "1", "0", "1000")), "PLN-10"));
}

TEST(Srs047, NegativeScaleMeasurableRangeIsStillOrdered) {
  IQK_VERIFIES("SRS-047");
  // scale -2.5, offset 1, 1 V -> -1.5 .. 3.5
  EXPECT_TRUE(check(aam("-1.5,3.5", "-2.5", "1")).empty());
  EXPECT_TRUE(check(aam("0,2", "-2.5", "1")).empty());
  EXPECT_TRUE(has(check(aam("-1.6,2", "-2.5", "1")), "PLN-10"));
  EXPECT_TRUE(has(check(aam("0,3.6", "-2.5", "1")), "PLN-10"));
  EXPECT_TRUE(has(check(aam("2,0", "-2.5", "1")), "PLN-10"));
}

TEST(Srs047, ZeroScaleRejected) {
  IQK_VERIFIES("SRS-047");
  EXPECT_TRUE(has(check(aam("", "0", "0")), "PLN-13"));
  EXPECT_TRUE(has(check(aam("", "0.0", "1")), "PLN-13"));
  EXPECT_TRUE(has(check(aam("0,1", "0", "0")), "PLN-13"));
  EXPECT_FALSE(has(check(aam("", "0.001", "0")), "PLN-13"));
  EXPECT_FALSE(has(check(aam("", "-0.001", "0")), "PLN-13"));
}

TEST(Srs047, DefaultScaleIsNonZero) {
  IQK_VERIFIES("SRS-047");
  EXPECT_TRUE(check("channel ch1 aam:1:0 rate=100 range_mv=1000 alarm=-1,1").empty());
  EXPECT_TRUE(has(check("channel ch1 aam:1:0 rate=100 range_mv=1000 alarm=-1,1.01"), "PLN-10"));
}

TEST(Srs047, ImuGyroAlarmLimits) {
  IQK_VERIFIES("SRS-047");
  EXPECT_TRUE(check("channel g imu:gyro_x rate=100 alarm=-8.727,8.727").empty());
  EXPECT_TRUE(check("channel g imu:gyro_z rate=100 alarm=-1,1").empty());
  EXPECT_TRUE(has(check("channel g imu:gyro_y rate=100 alarm=-8.728,1"), "PLN-10"));
  EXPECT_TRUE(has(check("channel g imu:gyro_z rate=100 alarm=-1,8.728"), "PLN-10"));
  EXPECT_TRUE(has(check("channel g imu:gyro_x rate=100 alarm=1,-1"), "PLN-10"));
  EXPECT_TRUE(has(check("channel g imu:gyro_x rate=100 alarm=1,1"), "PLN-10"));
}

TEST(Srs047, ImuAccelAlarmLimits) {
  IQK_VERIFIES("SRS-047");
  EXPECT_TRUE(check("channel a imu:accel_x rate=100 alarm=-156.9,156.9").empty());
  EXPECT_TRUE(has(check("channel a imu:accel_y rate=100 alarm=-157,10"), "PLN-10"));
  EXPECT_TRUE(has(check("channel a imu:accel_z rate=100 alarm=-10,157"), "PLN-10"));
  EXPECT_TRUE(has(check("channel a imu:accel_z rate=100 alarm=10,-10"), "PLN-10"));
}

TEST(Srs047, ImuTempAlarmLimits) {
  IQK_VERIFIES("SRS-047");
  EXPECT_TRUE(check("channel t imu:temp rate=50 alarm=-40,85").empty());
  EXPECT_TRUE(check("channel t imu:temp rate=50 alarm=-39.9,84.9").empty());
  EXPECT_TRUE(has(check("channel t imu:temp rate=50 alarm=-40.1,50"), "PLN-10"));
  EXPECT_TRUE(has(check("channel t imu:temp rate=50 alarm=0,85.1"), "PLN-10"));
  EXPECT_TRUE(has(check("channel t imu:temp rate=50 alarm=90,100"), "PLN-10"));
}

TEST(Srs047, EachChannelCheckedIndependently) {
  IQK_VERIFIES("SRS-047");
  std::vector<Finding> f;
  const std::string text = "iqk-plan 1\ncampaign c1\n" + aam("0,2") + "\n" +
                           "channel ch2 aam:1:1 rate=100 range_mv=1000 scale=2.5 offset=1 alarm=0,9\n";
  auto p = iqk::plan::parse(text, f);
  ASSERT_TRUE(p.has_value());
  EXPECT_FALSE(iqk::plan::validate(*p, f));
  ASSERT_EQ(f.size(), 1U);
  EXPECT_EQ(f[0].rule, "PLN-10");
  EXPECT_EQ(f[0].line, 4);
}
