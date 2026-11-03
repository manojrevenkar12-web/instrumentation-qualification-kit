#include "support.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "iqk/config.hpp"
#include "iqk/plan.hpp"

namespace {

struct Ch {
  int node;
  int input;
  int rate;
};

std::string plan_text(const std::vector<Ch>& chs, const std::string& extra = "") {
  std::string t = "iqk-plan 1\ncampaign c1\n";
  int i = 0;
  for (const auto& c : chs) {
    t += "channel ch" + std::to_string(i++) + " aam:" + std::to_string(c.node) + ":" + std::to_string(c.input) +
         " rate=" + std::to_string(c.rate) + " range_mv=1000\n";
  }
  return t + extra;
}

iqk::plan::Plan parse_ok(const std::string& text) {
  std::vector<iqk::plan::Finding> f;
  auto p = iqk::plan::parse(text, f);
  EXPECT_TRUE(p.has_value());
  return p.value_or(iqk::plan::Plan{});
}

bool has_pln11(const iqk::plan::Plan& p) {
  std::vector<iqk::plan::Finding> f;
  (void)iqk::plan::validate(p, f);
  return std::any_of(f.begin(), f.end(), [](const auto& x) { return x.rule == "PLN-11"; });
}

// Independent model: classic CAN, 11-bit id, worst-case stuffing: 8n+47 bits + floor((34+8n-1)/4) stuff bits.
double model_load(const std::vector<Ch>& chs) {
  std::map<int, std::map<int, int>> g;
  for (const auto& c : chs) {
    auto& r = g[c.node][c.input / 3];
    r = std::max(r, c.rate);
  }
  const int hb = 8 * 4 + 47 + (34 + 8 * 4 - 1) / 4;  // 95
  const int dt = 8 * 8 + 47 + (34 + 8 * 8 - 1) / 4;  // 135
  double bits = 0;
  for (const auto& [n, gs] : g) {
    bits += hb;
    for (const auto& [i, r] : gs) bits += double(r) * dt;
  }
  return bits / 500000.0;
}

}  // namespace

TEST(CanBusLoad, FrameBitsIncludeWorstCaseStuffing) {
  IQK_VERIFIES("SRS-048");
  EXPECT_EQ(iqk::plan::can_frame_bits(8), 135);
  EXPECT_EQ(iqk::plan::can_frame_bits(4), 95);
  EXPECT_EQ(iqk::plan::can_frame_bits(0), 47 + 8);
}

TEST(CanBusLoad, LoadValueSingleGroupWithHeartbeat) {
  IQK_VERIFIES("SRS-048");
  const auto p = parse_ok(plan_text({{1, 0, 1000}}));
  EXPECT_NEAR(iqk::plan::can_bus_load(p), (135.0 * 1000 + 95.0) / 500000.0, 1e-12);
}

TEST(CanBusLoad, GroupSharesOneFrameAtFastestRate) {
  IQK_VERIFIES("SRS-048");
  const std::vector<Ch> chs = {{1, 0, 100}, {1, 1, 1000}, {1, 2, 10}};
  EXPECT_NEAR(iqk::plan::can_bus_load(parse_ok(plan_text(chs))), model_load(chs), 1e-12);
  EXPECT_NEAR(iqk::plan::can_bus_load(parse_ok(plan_text({{1, 0, 500}, {1, 1, 500}, {1, 2, 500}}))),
              iqk::plan::can_bus_load(parse_ok(plan_text({{1, 0, 500}}))), 1e-12);
  // Inputs 2 and 3 are in different groups.
  EXPECT_NEAR(iqk::plan::can_bus_load(parse_ok(plan_text({{1, 2, 500}, {1, 3, 500}}))),
              (2 * 135.0 * 500 + 95.0) / 500000.0, 1e-12);
}

TEST(CanBusLoad, EachModuleAddsHeartbeatAndImuAddsNothing) {
  IQK_VERIFIES("SRS-048");
  const std::vector<Ch> chs = {{1, 0, 10}, {2, 0, 10}, {3, 0, 10}};
  EXPECT_NEAR(iqk::plan::can_bus_load(parse_ok(plan_text(chs))), model_load(chs), 1e-12);
  const auto with_imu = parse_ok(plan_text(chs, "channel g imu:gyro_x rate=400\n"));
  EXPECT_NEAR(iqk::plan::can_bus_load(with_imu), model_load(chs), 1e-12);
}

TEST(CanBusLoad, JustBelowLimitAccepted) {
  IQK_VERIFIES("SRS-048");
  // Group rates 1000+1000+200+20 = 2220 on one module: 0.59959.
  const std::vector<Ch> chs = {{1, 0, 1000}, {1, 3, 1000}, {1, 6, 200}, {1, 9, 20}};
  const auto p = parse_ok(plan_text(chs));
  const double load = iqk::plan::can_bus_load(p);
  EXPECT_NEAR(load, (135.0 * 2220 + 95) / 500000.0, 1e-12);
  EXPECT_LE(load, iqk::cfg::plan::max_can_bus_load);
  EXPECT_FALSE(has_pln11(p));
  std::vector<iqk::plan::Finding> f;
  EXPECT_TRUE(iqk::plan::validate(p, f));
}

TEST(CanBusLoad, JustAboveLimitRejected) {
  IQK_VERIFIES("SRS-048");
  // Group rates 1000+1000+200+50 = 2250 on one module: 0.60759.
  const std::vector<Ch> chs = {{1, 0, 1000}, {1, 3, 1000}, {1, 6, 200}, {1, 9, 50}};
  const auto p = parse_ok(plan_text(chs));
  EXPECT_GT(iqk::plan::can_bus_load(p), iqk::cfg::plan::max_can_bus_load);
  EXPECT_TRUE(has_pln11(p));
  std::vector<iqk::plan::Finding> f;
  EXPECT_FALSE(iqk::plan::validate(p, f));
}

TEST(CanBusLoad, HeartbeatsTipTheBalance) {
  IQK_VERIFIES("SRS-048");
  // Same data rate (2220), two modules: 0.59978, still accepted.
  const std::vector<Ch> ok = {{1, 0, 1000}, {1, 3, 1000}, {1, 6, 200}, {2, 0, 20}};
  EXPECT_FALSE(has_pln11(parse_ok(plan_text(ok))));
  EXPECT_NEAR(iqk::plan::can_bus_load(parse_ok(plan_text(ok))), model_load(ok), 1e-12);
  // One more 10 Hz group (2230) crosses the limit.
  const std::vector<Ch> bad = {{1, 0, 1000}, {1, 3, 1000}, {1, 6, 200}, {2, 0, 20}, {2, 3, 10}};
  EXPECT_TRUE(has_pln11(parse_ok(plan_text(bad))));
}

TEST(CanBusLoad, ClearlyOverloadedAndClearlyLightPlans) {
  IQK_VERIFIES("SRS-048");
  const auto heavy = parse_ok(plan_text({{1, 0, 1000}, {1, 3, 1000}, {1, 6, 1000}}));
  EXPECT_TRUE(has_pln11(heavy));
  const auto light = parse_ok(plan_text({{1, 0, 10}}));
  EXPECT_FALSE(has_pln11(light));
}

TEST(CanBusLoad, ValidationAgreesWithModelAcrossSweep) {
  IQK_VERIFIES("SRS-048");
  const int rates[] = {1000, 500, 200, 100, 50, 20, 10};
  int accepted = 0, rejected = 0;
  for (int a : rates)
    for (int b : rates)
      for (int c : rates)
        for (int d : rates) {
          const std::vector<Ch> chs = {{1, 0, a}, {1, 3, b}, {2, 0, c}, {2, 3, d}, {3, 6, 1000}};
          const auto p = parse_ok(plan_text(chs));
          const double m = model_load(chs);
          ASSERT_NEAR(iqk::plan::can_bus_load(p), m, 1e-12);
          const bool over = m > iqk::cfg::plan::max_can_bus_load;
          ASSERT_EQ(has_pln11(p), over) << a << ' ' << b << ' ' << c << ' ' << d;
          (over ? rejected : accepted)++;
        }
  EXPECT_GT(accepted, 0);
  EXPECT_GT(rejected, 0);
}
