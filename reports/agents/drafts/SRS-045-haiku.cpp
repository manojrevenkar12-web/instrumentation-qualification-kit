#include "support.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "iqk/dau.hpp"
#include "iqk/loader.hpp"
#include "iqk/plan.hpp"

// SRS-045: inputs carried in the same data frame must be planned at the same rate (PLN-08).
// An AAM data frame carries three inputs of a module: inputs 0..2 share one frame, 3..5 the next, and so on
// (group = input / 3, per module). Frames of different modules are different CAN identifiers.

namespace {

using iqk::plan::Finding;

struct Checked {
  bool parsed{false};
  bool valid{false};
  std::vector<Finding> findings;
};

// Parses and validates a plan made of the given channel lines, after the header (which is two lines long, so
// the first channel line is line 3).
Checked check(const std::string& channels) {
  const std::string text = "iqk-plan 1\ncampaign srs045\n" + channels;
  Checked c;
  std::vector<Finding> f;
  const auto p = iqk::plan::parse(text, f);
  c.parsed = p.has_value();
  if (p) {
    c.valid = iqk::plan::validate(*p, f);
  }
  c.findings = f;
  return c;
}

std::string channel(const std::string& name, int node, int input, const std::string& rate) {
  return "channel " + name + " aam:" + std::to_string(node) + ":" + std::to_string(input) + " rate=" + rate +
         " range_mv=1000\n";
}

std::vector<int> lines_for(const std::vector<Finding>& findings, std::string_view rule) {
  std::vector<int> out;
  for (const auto& f : findings) {
    if (f.rule == rule) {
      out.push_back(f.line);
    }
  }
  return out;
}

}  // namespace

TEST(SRS045SharedRate, InputsOfOneFrameAtDifferentRatesAreRejected) {
  IQK_VERIFIES("SRS-045");
  const auto c = check(channel("a", 1, 0, "100") + channel("b", 1, 1, "50"));
  ASSERT_TRUE(c.parsed);
  EXPECT_FALSE(c.valid);
  // Reported on the second input, the one that disagrees with the frame.
  EXPECT_EQ(lines_for(c.findings, "PLN-08"), std::vector<int>{4});
}

TEST(SRS045SharedRate, InputsOfOneFrameAtTheSameRateAreAccepted) {
  IQK_VERIFIES("SRS-045");
  const auto c = check(channel("a", 1, 0, "100") + channel("b", 1, 1, "100") + channel("c", 1, 2, "100"));
  ASSERT_TRUE(c.parsed);
  EXPECT_TRUE(c.valid);
  EXPECT_TRUE(c.findings.empty());
}

TEST(SRS045SharedRate, EveryInputOfTheFrameIsCompared) {
  IQK_VERIFIES("SRS-045");
  // Two agree, the third does not: the frame still mixes rates.
  const auto c = check(channel("a", 1, 0, "100") + channel("b", 1, 1, "100") + channel("c", 1, 2, "50"));
  ASSERT_TRUE(c.parsed);
  EXPECT_FALSE(c.valid);
  EXPECT_EQ(lines_for(c.findings, "PLN-08"), std::vector<int>{5});
}

TEST(SRS045SharedRate, PartlyUsedFrameMustStillShareARate) {
  IQK_VERIFIES("SRS-045");
  // Only inputs 3 and 5 of module 1 are planned; they are both in frame 1 and so must share a rate.
  const auto c = check(channel("a", 1, 3, "100") + channel("b", 1, 5, "50"));
  ASSERT_TRUE(c.parsed);
  EXPECT_FALSE(c.valid);
  EXPECT_EQ(lines_for(c.findings, "PLN-08"), std::vector<int>{4});
}

TEST(SRS045SharedRate, LastFrameOfAModuleIsChecked) {
  IQK_VERIFIES("SRS-045");
  // Frame 3 is inputs 9..11; input 11 is its last member and must be compared with the rest.
  const auto c = check(channel("a", 1, 9, "100") + channel("b", 1, 11, "50"));
  ASSERT_TRUE(c.parsed);
  EXPECT_FALSE(c.valid);
  EXPECT_EQ(lines_for(c.findings, "PLN-08"), std::vector<int>{4});
}

TEST(SRS045SharedRate, InputsAcrossAFrameBoundaryMayDifferInRate) {
  IQK_VERIFIES("SRS-045");
  // Each pair straddles a frame boundary (2|3, 5|6, 8|9): different frames, so different rates are allowed.
  const std::pair<int, int> pairs[] = {{2, 3}, {5, 6}, {8, 9}};
  for (const auto& [lo, hi] : pairs) {
    SCOPED_TRACE("inputs " + std::to_string(lo) + " and " + std::to_string(hi));
    const auto c = check(channel("a", 1, lo, "100") + channel("b", 1, hi, "50"));
    ASSERT_TRUE(c.parsed);
    EXPECT_TRUE(c.valid);
    EXPECT_TRUE(lines_for(c.findings, "PLN-08").empty());
  }
}

TEST(SRS045SharedRate, SameInputNumberOnDifferentModulesIsNotSameFrame) {
  IQK_VERIFIES("SRS-045");
  // Input 0 of module 1 and input 0 of module 2 are in different frames (different CAN identifiers).
  const auto c = check(channel("a", 1, 0, "100") + channel("b", 2, 0, "50"));
  ASSERT_TRUE(c.parsed);
  EXPECT_TRUE(c.valid);
  EXPECT_TRUE(lines_for(c.findings, "PLN-08").empty());
}

TEST(SRS045SharedRate, RatesEqualAsNumbersAreTheSameRate) {
  IQK_VERIFIES("SRS-045");
  // Written differently, but the same rate: the frame is consistent.
  const auto c = check(channel("a", 1, 0, "100") + channel("b", 1, 1, "100.0"));
  ASSERT_TRUE(c.parsed);
  EXPECT_TRUE(c.valid);
  EXPECT_TRUE(c.findings.empty());
}

TEST(SRS045SharedRate, DauDoesNotArmAPlanWhoseFrameMixesRates) {
  IQK_VERIFIES("SRS-045");
  iqk::Dau dau{0x1234};
  std::vector<Finding> findings;
  const std::string text = "iqk-plan 1\ncampaign srs045\n" + channel("a", 1, 0, "100") + channel("b", 1, 1, "50");
  EXPECT_FALSE(dau.load_plan(text, 0, findings));
  EXPECT_EQ(lines_for(findings, "PLN-08"), std::vector<int>{4});
  EXPECT_FALSE(dau.armed());
  EXPECT_EQ(dau.plan_crc(), 0U);
  EXPECT_EQ(dau.loader().status(), iqk::PlanLoader::Status::idle);
  EXPECT_EQ(dau.loader().total(), 0U);
}

TEST(SRS045SharedRate, DauLoadsAPlanWhoseFramesAreConsistent) {
  IQK_VERIFIES("SRS-045");
  iqk::Dau dau{0x1234};
  std::vector<Finding> findings;
  const std::string text = "iqk-plan 1\ncampaign srs045\n" + channel("a", 1, 0, "100") +
                           channel("b", 1, 1, "100") + channel("c", 1, 3, "50");
  EXPECT_TRUE(dau.load_plan(text, 0, findings));
  EXPECT_TRUE(findings.empty());
  EXPECT_GT(dau.loader().total(), 0U);
}
