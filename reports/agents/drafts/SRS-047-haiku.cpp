// SRS-047 Alarm limits are measurable: alarm limits are ordered (low < high) and inside what the channel can
// measure given its range, scale and offset; scale is not zero (PLN-10, PLN-13).
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "support.hpp"
#include "iqk/plan.hpp"

namespace {

using iqk::plan::Finding;

struct Verdict {
  bool parsed{false};
  bool valid{false};
  std::vector<Finding> findings;
};

// Parses and validates a plan text, collecting every finding from both steps.
Verdict check(const std::string& text) {
  Verdict v;
  std::vector<Finding> f;
  const auto p = iqk::plan::parse(text, f);
  v.parsed = p.has_value();
  if (p) {
    v.valid = iqk::plan::validate(*p, f);
  }
  v.findings = f;
  return v;
}

bool has_rule(const std::vector<Finding>& f, std::string_view rule, int line) {
  for (const auto& x : f) {
    if (x.rule == rule && x.line == line) {
      return true;
    }
  }
  return false;
}

// A one-channel AAM plan on input aam:1:0 at 100 Hz, which is valid apart from the channel attributes given.
std::string aam_plan(const std::string& attrs) {
  return "iqk-plan 1\ncampaign srs047\nchannel ch1 aam:1:0 rate=100 " + attrs + "\n";
}

// A one-channel IMU plan at 400 Hz, which is valid apart from the alarm given.
std::string imu_plan(const std::string& field, const std::string& alarm) {
  return "iqk-plan 1\ncampaign srs047\nchannel ch1 imu:" + field + " rate=400 alarm=" + alarm + "\n";
}

// Exact decimal text for a double, so a boundary nudge of 1e-6 survives the round trip.
std::string num(double v) {
  std::ostringstream os;
  os.precision(17);
  os << v;
  return os.str();
}

// The channel attributes for an AAM input: range code, scale, offset and alarm limits.
std::string aam_attrs(int range_mv, double scale, double offset, const std::string& alarm) {
  return "range_mv=" + std::to_string(range_mv) + " scale=" + num(scale) + " offset=" + num(offset) +
         " alarm=" + alarm;
}

}  // namespace

// Ordering: the low limit must be strictly below the high limit. Equal limits are not ordered.
TEST(Srs047, AlarmLimitsMustBeOrdered) {
  IQK_VERIFIES("SRS-047");

  const auto reversed = check(aam_plan(aam_attrs(1000, 1.0, 0.0, "0.5,-0.5")));
  ASSERT_TRUE(reversed.parsed) << "the plan must parse so that validation is what rejects it";
  EXPECT_FALSE(reversed.valid);
  EXPECT_TRUE(has_rule(reversed.findings, "PLN-10", 3));

  const auto equal = check(aam_plan(aam_attrs(1000, 1.0, 0.0, "0.25,0.25")));
  ASSERT_TRUE(equal.parsed);
  EXPECT_FALSE(equal.valid);
  EXPECT_TRUE(has_rule(equal.findings, "PLN-10", 3));

  const auto ordered = check(aam_plan(aam_attrs(1000, 1.0, 0.0, "-0.5,0.5")));
  ASSERT_TRUE(ordered.parsed);
  EXPECT_TRUE(ordered.valid);
  EXPECT_TRUE(ordered.findings.empty());
}

// Measurable range for an AAM input at range +/-1 V, scale 1, offset 0 is [-1, 1]: both ends are inside.
TEST(Srs047, AamAlarmInsideRangeIncludingEndpoints) {
  IQK_VERIFIES("SRS-047");

  const auto at_ends = check(aam_plan(aam_attrs(1000, 1.0, 0.0, "-1,1")));
  ASSERT_TRUE(at_ends.parsed);
  EXPECT_TRUE(at_ends.valid);
  EXPECT_TRUE(at_ends.findings.empty());

  const auto above = check(aam_plan(aam_attrs(1000, 1.0, 0.0, "0,1.000001")));
  ASSERT_TRUE(above.parsed);
  EXPECT_FALSE(above.valid);
  EXPECT_TRUE(has_rule(above.findings, "PLN-10", 3));

  const auto below = check(aam_plan(aam_attrs(1000, 1.0, 0.0, "-1.000001,0")));
  ASSERT_TRUE(below.parsed);
  EXPECT_FALSE(below.valid);
  EXPECT_TRUE(has_rule(below.findings, "PLN-10", 3));
}

// The measurable range follows the range code: +/-100 mV is [-0.1, 0.1] and +/-10 V is [-10, 10].
TEST(Srs047, AamMeasurableRangeFollowsRangeCode) {
  IQK_VERIFIES("SRS-047");

  struct Case {
    int range_mv;
    double edge;
  };
  for (const Case c : {Case{100, 0.1}, Case{1000, 1.0}, Case{5000, 5.0}, Case{10000, 10.0}}) {
    const auto inside = check(aam_plan(aam_attrs(c.range_mv, 1.0, 0.0, num(-c.edge) + "," + num(c.edge))));
    ASSERT_TRUE(inside.parsed) << "range " << c.range_mv << " mV";
    EXPECT_TRUE(inside.valid) << "range " << c.range_mv << " mV";
    EXPECT_TRUE(inside.findings.empty()) << "range " << c.range_mv << " mV";

    const auto outside = check(aam_plan(aam_attrs(c.range_mv, 1.0, 0.0, "0," + num(c.edge + 1e-6))));
    ASSERT_TRUE(outside.parsed) << "range " << c.range_mv << " mV";
    EXPECT_FALSE(outside.valid) << "range " << c.range_mv << " mV";
    EXPECT_TRUE(has_rule(outside.findings, "PLN-10", 3)) << "range " << c.range_mv << " mV";
  }
}

// Scale and offset move the measurable range: scale 2 and offset 0.5 on +/-1 V gives [-1.5, 2.5].
TEST(Srs047, AamMeasurableRangeFollowsScaleAndOffset) {
  IQK_VERIFIES("SRS-047");

  const auto at_ends = check(aam_plan(aam_attrs(1000, 2.0, 0.5, "-1.5,2.5")));
  ASSERT_TRUE(at_ends.parsed);
  EXPECT_TRUE(at_ends.valid);
  EXPECT_TRUE(at_ends.findings.empty());

  const auto low_out = check(aam_plan(aam_attrs(1000, 2.0, 0.5, "-1.500001,2.5")));
  ASSERT_TRUE(low_out.parsed);
  EXPECT_FALSE(low_out.valid);
  EXPECT_TRUE(has_rule(low_out.findings, "PLN-10", 3));

  const auto high_out = check(aam_plan(aam_attrs(1000, 2.0, 0.5, "-1.5,2.500001")));
  ASSERT_TRUE(high_out.parsed);
  EXPECT_FALSE(high_out.valid);
  EXPECT_TRUE(has_rule(high_out.findings, "PLN-10", 3));

  // The offset alone must shift the range: [-1, 1] shifted by +0.5 is [-0.5, 1.5], so -0.75 is outside.
  const auto shifted_out = check(aam_plan(aam_attrs(1000, 1.0, 0.5, "-0.75,1.5")));
  ASSERT_TRUE(shifted_out.parsed);
  EXPECT_FALSE(shifted_out.valid);
  EXPECT_TRUE(has_rule(shifted_out.findings, "PLN-10", 3));
}

// A negative scale inverts the range but measures the same span: scale -2 on +/-1 V is still [-2, 2] around
// offset 0, so limits at +/-2 are accepted and +/-2.000001 is not.
TEST(Srs047, AamNegativeScaleMeasuresSameSpan) {
  IQK_VERIFIES("SRS-047");

  const auto at_ends = check(aam_plan(aam_attrs(1000, -2.0, 0.0, "-2,2")));
  ASSERT_TRUE(at_ends.parsed);
  EXPECT_TRUE(at_ends.valid);
  EXPECT_TRUE(at_ends.findings.empty());

  const auto outside = check(aam_plan(aam_attrs(1000, -2.0, 0.0, "-2.000001,2")));
  ASSERT_TRUE(outside.parsed);
  EXPECT_FALSE(outside.valid);
  EXPECT_TRUE(has_rule(outside.findings, "PLN-10", 3));
}

// Scale must not be zero (PLN-13). A zero scale collapses the measurable range to the offset alone; a tiny
// non-zero scale is still a valid scale.
TEST(Srs047, ScaleZeroIsRejected) {
  IQK_VERIFIES("SRS-047");

  for (const char* zero : {"0", "0.0", "-0.0"}) {
    const auto v = check(aam_plan(std::string{"range_mv=1000 scale="} + zero + " offset=0 alarm=-0.5,0.5"));
    ASSERT_TRUE(v.parsed) << "scale " << zero;
    EXPECT_FALSE(v.valid) << "scale " << zero;
    EXPECT_TRUE(has_rule(v.findings, "PLN-13", 3)) << "scale " << zero;
  }

  const auto tiny = check(aam_plan(aam_attrs(1000, 1e-9, 0.0, "-5e-10,5e-10")));
  ASSERT_TRUE(tiny.parsed);
  EXPECT_TRUE(tiny.valid);
  EXPECT_FALSE(has_rule(tiny.findings, "PLN-13", 3));
}

// IMU channels: the measurable range is the engineering range of the field in the IMU-SER interface document.
TEST(Srs047, ImuAlarmInsideEngineeringRange) {
  IQK_VERIFIES("SRS-047");

  const auto gyro_edge = check(imu_plan("gyro_x", "-8.727,8.727"));
  ASSERT_TRUE(gyro_edge.parsed);
  EXPECT_TRUE(gyro_edge.valid);
  EXPECT_TRUE(gyro_edge.findings.empty());

  const auto gyro_out = check(imu_plan("gyro_x", "-8.727,8.728"));
  ASSERT_TRUE(gyro_out.parsed);
  EXPECT_FALSE(gyro_out.valid);
  EXPECT_TRUE(has_rule(gyro_out.findings, "PLN-10", 3));

  const auto accel_edge = check(imu_plan("accel_z", "-156.9,156.9"));
  ASSERT_TRUE(accel_edge.parsed);
  EXPECT_TRUE(accel_edge.valid);

  const auto accel_out = check(imu_plan("accel_z", "-156.9,157"));
  ASSERT_TRUE(accel_out.parsed);
  EXPECT_FALSE(accel_out.valid);
  EXPECT_TRUE(has_rule(accel_out.findings, "PLN-10", 3));

  const auto temp_edge = check(imu_plan("temp", "-40,85"));
  ASSERT_TRUE(temp_edge.parsed);
  EXPECT_TRUE(temp_edge.valid);

  const auto temp_out = check(imu_plan("temp", "-40.01,85"));
  ASSERT_TRUE(temp_out.parsed);
  EXPECT_FALSE(temp_out.valid);
  EXPECT_TRUE(has_rule(temp_out.findings, "PLN-10", 3));

  const auto temp_unordered = check(imu_plan("temp", "10,-10"));
  ASSERT_TRUE(temp_unordered.parsed);
  EXPECT_FALSE(temp_unordered.valid);
  EXPECT_TRUE(has_rule(temp_unordered.findings, "PLN-10", 3));
}
