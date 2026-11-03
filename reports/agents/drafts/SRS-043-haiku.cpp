// SRS-043 Rates are achievable: module rates are the base rate over an allowed divisor; IMU rates are one of
// plan.allowed_imu_rates_hz (PLN-06).
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "fake_module.hpp"
#include "iqk/config.hpp"
#include "iqk/dau.hpp"
#include "iqk/icd/aam.hpp"
#include "iqk/plan.hpp"
#include "support.hpp"

namespace {

namespace aam = iqk::icd::aam;
using iqk::plan::Finding;

constexpr int kNode = 3;
constexpr int kInput = 7;

std::string aam_plan(std::string_view rate) {
  return "iqk-plan 1\ncampaign qual\nchannel t1 aam:" + std::to_string(kNode) + ":" + std::to_string(kInput) +
         " rate=" + std::string{rate} + " range_mv=1000\n";
}

std::string imu_plan(std::string_view rate) {
  return "iqk-plan 1\ncampaign qual\nchannel t1 imu:gyro_x rate=" + std::string{rate} + "\n";
}

struct Verdict {
  bool parsed{false};
  bool valid{false};
  std::vector<Finding> findings;
};

Verdict check(const std::string& text) {
  Verdict v;
  std::vector<Finding> f;
  const auto p = iqk::plan::parse(text, f);
  v.parsed = p.has_value();
  v.valid = p && iqk::plan::validate(*p, f);
  v.findings = std::move(f);
  return v;
}

bool has_rule(const std::vector<Finding>& f, std::string_view rule) {
  for (const auto& x : f) {
    if (x.rule == rule && x.line > 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

// The module base rate and the divisors the module accepts, from icd/aam.yaml.
TEST(ModuleRates, BaseRateAndDivisorsMatchTheInterfaceDocument) {
  IQK_VERIFIES("SRS-043");
  EXPECT_EQ(aam::kBaseRateHz, 1000);
  const std::array<std::int64_t, 7> want{1, 2, 5, 10, 20, 50, 100};
  EXPECT_EQ(aam::kDivisors, want);
}

// Every base-over-divisor rate is accepted: 1000 (divisor 1) down to 10 Hz (divisor 100).
TEST(ModuleRates, EveryBaseOverAllowedDivisorIsAccepted) {
  IQK_VERIFIES("SRS-043");
  for (const std::string rate : {"1000", "500", "200", "100", "50", "20", "10"}) {
    SCOPED_TRACE("rate " + rate);
    const auto v = check(aam_plan(rate));
    ASSERT_TRUE(v.parsed);
    EXPECT_TRUE(v.valid);
    EXPECT_FALSE(has_rule(v.findings, "PLN-06"));
  }
}

// Anything that is not base over an allowed divisor is refused with PLN-06, including near misses on both sides
// of each valid rate (so a loose tolerance or a wrong base or divisor table is caught), and rates beyond the
// ends of the table (above 1000 Hz, below 10 Hz, non-integer divisors).
TEST(ModuleRates, EveryOtherRateIsRefusedWithPln06) {
  IQK_VERIFIES("SRS-043");
  for (const std::string rate : {"0", "-10", "2", "2.5", "5", "7", "9.9", "9", "166", "199", "201", "333", "333.3",
                                 "334", "499", "500.5", "501", "999", "1001", "1010", "2000"}) {
    SCOPED_TRACE("rate " + rate);
    const auto v = check(aam_plan(rate));
    ASSERT_TRUE(v.parsed);
    EXPECT_FALSE(v.valid);
    EXPECT_TRUE(has_rule(v.findings, "PLN-06"));
  }
}

// The unit refuses to arm a plan with an unachievable module rate.
TEST(ModuleRates, UnitDoesNotArmAPlanWithAnUnachievableModuleRate) {
  IQK_VERIFIES("SRS-043");
  for (const std::string rate : {"333.3", "999", "9.9"}) {
    SCOPED_TRACE("rate " + rate);
    iqk::Dau dau{0x1234};
    std::vector<Finding> f;
    const iqk::Tx tx = [](iqk::Port, std::span<const std::uint8_t>) {};
    EXPECT_FALSE(dau.load_plan(aam_plan(rate), 0, f));
    dau.tick(0, tx);
    EXPECT_FALSE(dau.armed());
    EXPECT_TRUE(has_rule(f, "PLN-06"));
  }
}

// The divisor each module is told to apply is 1000 / rate, and the module applies it: the configuration written
// through the register protocol is what the module holds after commit.
TEST(ModuleRates, ModuleAppliesBaseOverRateAsItsDivisor) {
  IQK_VERIFIES("SRS-043");
  const std::array<std::pair<int, std::uint32_t>, 7> cases{{{1000, 1}, {500, 2}, {200, 5}, {100, 10}, {50, 20},
                                                             {20, 50}, {10, 100}}};
  for (const auto& [rate, divisor] : cases) {
    SCOPED_TRACE("rate " + std::to_string(rate));
    const auto v = check(aam_plan(std::to_string(rate)));
    ASSERT_TRUE(v.parsed);
    ASSERT_TRUE(v.valid);

    std::vector<Finding> f;
    const auto p = iqk::plan::parse(aam_plan(std::to_string(rate)), f);
    ASSERT_TRUE(p.has_value());

    iqk_test::FakeModule m;
    m.node = kNode;
    for (const auto& w : iqk::plan::registers(*p)) {
      aam::AamRegRequest req{};
      req.cmd = 0x23;
      req.index = w.index;
      req.sub = w.sub;
      req.value = w.value;
      std::array<std::uint8_t, aam::AamRegRequest::kSize> pl{};
      ASSERT_EQ(aam::encode(req, pl), pl.size());
      const auto resp = m.on_frame(iqk_test::can(aam::AamRegRequest::can_id(kNode), pl));
      ASSERT_TRUE(resp.has_value());
      ASSERT_EQ(resp->data[0], 0x60) << "write to index " << w.index << " sub " << int{w.sub} << " not acknowledged";
    }
    aam::AamRegRequest commit{};
    commit.cmd = 0x23;
    commit.index = aam::kRegCommitIndex;
    commit.sub = aam::kRegCommitSub;
    commit.value = 1;
    std::array<std::uint8_t, aam::AamRegRequest::kSize> cp{};
    ASSERT_EQ(aam::encode(commit, cp), cp.size());
    ASSERT_TRUE(m.on_frame(iqk_test::can(aam::AamRegRequest::can_id(kNode), cp)).has_value());

    EXPECT_EQ(m.applied[kInput].enable, 1U);
    EXPECT_EQ(m.applied[kInput].divisor, divisor);
    EXPECT_EQ(static_cast<std::int64_t>(aam::kBaseRateHz) / static_cast<std::int64_t>(m.applied[kInput].divisor),
              rate);
  }
}

// The IMU rate set is the configured one, and every member of it is accepted.
TEST(ImuRates, ConfiguredAllowedRatesAreTheIcdSet) {
  IQK_VERIFIES("SRS-043");
  const std::array<std::int64_t, 4> want{400, 200, 100, 50};
  EXPECT_EQ(iqk::cfg::plan::allowed_imu_rates_hz, want);
}

TEST(ImuRates, EveryAllowedImuRateIsAccepted) {
  IQK_VERIFIES("SRS-043");
  for (const std::string rate : {"400", "200", "100", "50"}) {
    SCOPED_TRACE("rate " + rate);
    const auto v = check(imu_plan(rate));
    ASSERT_TRUE(v.parsed);
    EXPECT_TRUE(v.valid);
    EXPECT_FALSE(has_rule(v.findings, "PLN-06"));
  }
}

// Rates outside the IMU set are refused with PLN-06. Included: module rates that are valid for an AAM input but
// not for the IMU (1000, 500, 20, 10), near misses either side of each allowed rate, rates that truncate to an
// allowed one (400.5, 50.5), and out-of-range values.
TEST(ImuRates, EveryOtherImuRateIsRefusedWithPln06) {
  IQK_VERIFIES("SRS-043");
  for (const std::string rate : {"0", "-400", "10", "20", "25", "49", "49.5", "50.5", "51", "99", "101", "199",
                                 "201", "399", "400.5", "401", "450", "500", "800", "1000"}) {
    SCOPED_TRACE("rate " + rate);
    const auto v = check(imu_plan(rate));
    ASSERT_TRUE(v.parsed);
    EXPECT_FALSE(v.valid);
    EXPECT_TRUE(has_rule(v.findings, "PLN-06"));
  }
}

// The rate the IMU delivers is the planned one: a 400 Hz stream yields exactly the planned number of samples per
// second, for each allowed rate (decimation of the 400 Hz samples).
TEST(ImuRates, PlannedImuRateIsDeliveredFromThe400HzStream) {
  IQK_VERIFIES("SRS-043");
  for (const int rate : {400, 200, 100, 50}) {
    SCOPED_TRACE("rate " + std::to_string(rate));
    iqk::Dau dau{0x1234};
    std::vector<Finding> f;
    const iqk::Tx tx = [](iqk::Port, std::span<const std::uint8_t>) {};
    ASSERT_TRUE(dau.load_plan(imu_plan(std::to_string(rate)), 0, f));
    dau.tick(0, tx);
    ASSERT_TRUE(dau.armed());

    constexpr std::int64_t kStart = iqk_test::kS;
    constexpr std::int64_t kPeriodNs = 2'500'000;  // 400 Hz
    for (std::uint16_t seq = 0; seq < 400; ++seq) {
      const std::int64_t t = kStart + static_cast<std::int64_t>(seq) * kPeriodNs;
      const auto bytes = iqk_test::imu_frame(iqk_test::imu_sample(seq, static_cast<std::uint32_t>(seq) * 2500U));
      dau.on_input(iqk::Port::imu, bytes, t, tx);
      dau.tick(t, tx);
    }
    dau.tick(kStart + 2 * iqk_test::kS, tx);  // flush the last frame
    EXPECT_EQ(dau.samples_sent(), static_cast<std::uint64_t>(rate));
  }
}
