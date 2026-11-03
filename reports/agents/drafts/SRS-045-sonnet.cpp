#include "support.hpp"

#include <algorithm>
#include <string>
#include <vector>

#include "iqk/plan.hpp"

namespace {

using iqk::plan::Finding;

struct Ch {
  int node;
  int input;
  const char* rate;
};

std::vector<Finding> check(const std::vector<Ch>& chs) {
  std::string text = "iqk-plan 1\ncampaign c1\n";
  int n = 0;
  for (const auto& c : chs) {
    text += "channel ch" + std::to_string(n++) + " aam:" + std::to_string(c.node) + ":" + std::to_string(c.input) +
            " rate=" + c.rate + " range_mv=1000\n";
  }
  std::vector<Finding> parse_f;
  auto p = iqk::plan::parse(text, parse_f);
  EXPECT_TRUE(p.has_value());
  EXPECT_TRUE(parse_f.empty());
  std::vector<Finding> f;
  if (p) {
    (void)iqk::plan::validate(*p, f);
  }
  return f;
}

int count(const std::vector<Finding>& f, const std::string& rule) {
  return static_cast<int>(std::count_if(f.begin(), f.end(), [&](const Finding& x) { return x.rule == rule; }));
}

}  // namespace

TEST(Srs045, SameRateInOneFramePasses) {
  IQK_VERIFIES("SRS-045");
  auto f = check({{1, 0, "100"}, {1, 1, "100"}, {1, 2, "100"}});
  EXPECT_EQ(count(f, "PLN-08"), 0);
  EXPECT_TRUE(f.empty());
}

TEST(Srs045, DifferentRateInOneFrameRejected) {
  IQK_VERIFIES("SRS-045");
  auto f = check({{1, 0, "100"}, {1, 1, "50"}});
  EXPECT_EQ(count(f, "PLN-08"), 1);
}

TEST(Srs045, MismatchBetweenFirstAndLastOfFrame) {
  IQK_VERIFIES("SRS-045");
  EXPECT_EQ(count(check({{1, 0, "100"}, {1, 2, "200"}}), "PLN-08"), 1);
  EXPECT_EQ(count(check({{1, 3, "10"}, {1, 5, "1000"}}), "PLN-08"), 1);
  EXPECT_EQ(count(check({{1, 9, "50"}, {1, 11, "20"}}), "PLN-08"), 1);
}

TEST(Srs045, MismatchIndependentOfOrder) {
  IQK_VERIFIES("SRS-045");
  EXPECT_EQ(count(check({{1, 2, "50"}, {1, 0, "100"}}), "PLN-08"), 1);
  EXPECT_EQ(count(check({{1, 1, "100"}, {1, 0, "100"}, {1, 2, "50"}}), "PLN-08"), 1);
}

TEST(Srs045, ThirdInputDiffersFromTwoAgreeing) {
  IQK_VERIFIES("SRS-045");
  auto f = check({{1, 0, "100"}, {1, 1, "100"}, {1, 2, "50"}});
  ASSERT_EQ(count(f, "PLN-08"), 1);
  auto it = std::find_if(f.begin(), f.end(), [](const Finding& x) { return x.rule == "PLN-08"; });
  EXPECT_EQ(it->line, 5);  // the offending channel statement
}

TEST(Srs045, AdjacentInputsInDifferentFramesMayDiffer) {
  IQK_VERIFIES("SRS-045");
  // Inputs 2 and 3 belong to different data frames (groups 0 and 1).
  EXPECT_TRUE(check({{1, 2, "100"}, {1, 3, "50"}}).empty());
  EXPECT_TRUE(check({{1, 5, "100"}, {1, 6, "50"}}).empty());
  EXPECT_TRUE(check({{1, 8, "100"}, {1, 9, "50"}}).empty());
  EXPECT_TRUE(check({{1, 0, "1000"}, {1, 3, "500"}, {1, 6, "100"}, {1, 9, "50"}}).empty());
}

TEST(Srs045, SameInputIndexOnDifferentModulesMayDiffer) {
  IQK_VERIFIES("SRS-045");
  EXPECT_TRUE(check({{1, 0, "100"}, {2, 0, "50"}}).empty());
  EXPECT_TRUE(check({{1, 1, "100"}, {2, 2, "50"}}).empty());
}

TEST(Srs045, EachMismatchedFrameReported) {
  IQK_VERIFIES("SRS-045");
  auto f = check({{1, 0, "100"}, {1, 1, "50"}, {1, 3, "100"}, {1, 4, "50"}});
  EXPECT_EQ(count(f, "PLN-08"), 2);
}

TEST(Srs045, ImuChannelsNotSubjectToFrameRule) {
  IQK_VERIFIES("SRS-045");
  std::vector<Finding> pf, f;
  auto p = iqk::plan::parse(
      "iqk-plan 1\ncampaign c\nchannel a imu:gyro_z rate=400\nchannel b imu:accel_z rate=100\n", pf);
  ASSERT_TRUE(p.has_value());
  (void)iqk::plan::validate(*p, f);
  EXPECT_EQ(count(f, "PLN-08"), 0);
}
