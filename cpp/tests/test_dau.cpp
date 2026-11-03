#include <gtest/gtest.h>

#include <map>

#include "fake_module.hpp"
#include "iqk/crc.hpp"
#include "iqk/dau.hpp"
#include "support.hpp"

namespace tlm = iqk::icd::tlm;
using iqk::Dau;
using iqk::Port;
using iqk_test::kMs;
using iqk_test::kS;

namespace {

const char* kPlan = R"(iqk-plan 1
campaign UNIT
channel strain   aam:1:0 rate=1000 range_mv=10000 scale=250 unit=ue alarm=-1000,1000
channel spare    aam:1:1 rate=1000 range_mv=10000
channel gyro_z   imu:gyro_z rate=400
channel accel_z  imu:accel_z rate=200
)";

// A unit with one acquisition module and an IMU, driven in 1 ms steps.
struct Rig {
  Dau dau{0x0A0B0C0D0E0F1011ULL};
  iqk_test::FakeModule mod;
  std::vector<std::vector<std::uint8_t>> tlm;
  std::vector<std::vector<std::uint8_t>> diag;
  std::int64_t now{kS};
  std::uint16_t imu_seq{0};
  bool hold_imu{false};                        // the serial driver holds IMU frames back
  std::vector<std::vector<std::uint8_t>> held;

  iqk::Tx tx() {
    return [this](Port p, std::span<const std::uint8_t> b) {
      if (p == Port::tlm) {
        tlm.emplace_back(b.begin(), b.end());
      } else if (p == Port::diag) {
        diag.emplace_back(b.begin(), b.end());
      } else if (p == Port::can) {
        iqk::CanFrame f{};
        if (iqk::decode_can_udp(b, f)) {
          if (const auto a = mod.on_frame(f)) {
            std::array<std::uint8_t, iqk::kCanUdpSize> u{};
            (void)iqk::encode_can_udp(*a, u);
            pending.push_back({u.begin(), u.end()});
          }
        }
      }
    };
  }
  std::vector<std::vector<std::uint8_t>> pending;

  void can(const iqk::CanFrame& f) {
    std::array<std::uint8_t, iqk::kCanUdpSize> u{};
    (void)iqk::encode_can_udp(f, u);
    dau.on_input(Port::can, u, now, tx());
  }
  void step(bool data = true, std::int16_t raw = 0) {
    for (auto& p : pending) {
      dau.on_input(Port::can, p, now, tx());
    }
    pending.clear();
    if (now % kS == 0) {
      can(iqk_test::heartbeat(1, mod.state));
    }
    if (data && mod.state == 2) {
      can(iqk_test::data_frame(1, 0, static_cast<std::uint8_t>(now / kMs), raw, 0, 0));
    }
    if ((now / 500'000) % 5 == 0 || true) {
      // 400 Hz IMU: one sample every 2.5 ms
    }
    if (now % 2'500'000 < kMs) {
      const auto f = iqk_test::imu_frame(iqk_test::imu_sample(imu_seq, static_cast<std::uint32_t>(now / 1000)));
      ++imu_seq;
      if (hold_imu) {
        held.push_back(f);
      } else {
        dau.on_input(Port::imu, f, now + 200'000, tx());
      }
    }
    dau.tick(now, tx());
    now += kMs;
  }
  void run_ms(int ms, bool data = true, std::int16_t raw = 0) {
    for (int i = 0; i < ms; ++i) {
      step(data, raw);
    }
  }
};

std::vector<tlm::TlmPacket> decode(const std::vector<std::vector<std::uint8_t>>& raw, int* crc_bad = nullptr) {
  std::vector<tlm::TlmPacket> out;
  for (const auto& b : raw) {
    const std::span<const std::uint8_t> s{b};
    const auto body = s.first(s.size() - 4);
    const std::uint32_t trailer = static_cast<std::uint32_t>(iqk::wire::get(s, s.size() - 4, 4, iqk::wire::Order::little));
    if (iqk::crc::crc32(body) != trailer) {
      if (crc_bad != nullptr) {
        ++*crc_bad;
      }
      continue;
    }
    tlm::TlmPacket p{};
    if (tlm::decode(body, p)) {
      out.push_back(p);
    }
  }
  return out;
}
}  // namespace

TEST(Unit, NoTelemetryUntilThePlanIsArmed) {
  IQK_VERIFIES("SRS-054");
  Rig r;
  r.run_ms(500);
  EXPECT_TRUE(r.tlm.empty());  // no plan
  std::vector<iqk::plan::Finding> f;
  ASSERT_TRUE(r.dau.load_plan(kPlan, r.now, f));
  r.step();
  EXPECT_FALSE(r.dau.armed());
  r.run_ms(400);
  ASSERT_TRUE(r.dau.armed());
  r.run_ms(200);
  const auto pk = decode(r.tlm);
  ASSERT_FALSE(pk.empty());
  std::vector<iqk::plan::Finding> g;
  const auto parsed = iqk::plan::parse(kPlan, g);
  for (const auto& p : pk) {
    EXPECT_EQ(p.plan_crc, iqk::plan::crc(*parsed));
  }
}

TEST(Unit, AnInvalidPlanIsRefusedAndTheOldOneStays) {
  IQK_VERIFIES("SRS-054");
  Rig r;
  std::vector<iqk::plan::Finding> f;
  ASSERT_TRUE(r.dau.load_plan(kPlan, r.now, f));
  r.run_ms(500);
  ASSERT_TRUE(r.dau.armed());
  const auto crc = r.dau.plan_crc();
  EXPECT_FALSE(r.dau.load_plan("iqk-plan 1\ncampaign bad\nchannel x aam:1:0 rate=333 range_mv=100\n", r.now, f));
  EXPECT_TRUE(r.dau.armed());
  EXPECT_EQ(r.dau.plan_crc(), crc);
  EXPECT_EQ(r.dau.faults().count(iqk::Fault::plan_rejected), 1U);
}

TEST(Unit, AFailedLoadNeverArms) {
  IQK_VERIFIES("SRS-053", "SRS-054");
  Rig r;
  r.mod.wrong_crc = true;
  std::vector<iqk::plan::Finding> f;
  ASSERT_TRUE(r.dau.load_plan(kPlan, r.now, f));
  r.run_ms(1000);
  EXPECT_FALSE(r.dau.armed());
  EXPECT_EQ(r.dau.plan_crc(), 0U);
  EXPECT_TRUE(r.tlm.empty());
  EXPECT_EQ(r.dau.faults().count(iqk::Fault::plan_load_failed), 1U);
}

TEST(Unit, PacketsCarryACrcAndConsecutiveSequenceNumbers) {
  IQK_VERIFIES("SRS-070");
  Rig r;
  std::vector<iqk::plan::Finding> f;
  ASSERT_TRUE(r.dau.load_plan(kPlan, r.now, f));
  r.run_ms(1500);
  int bad = 0;
  const auto pk = decode(r.tlm, &bad);
  EXPECT_EQ(bad, 0);
  ASSERT_GT(pk.size(), 50U);
  for (std::size_t i = 1; i < pk.size(); ++i) {
    EXPECT_EQ(pk[i].seq, pk[i - 1].seq + 1);
    EXPECT_EQ(pk[i].magic, 0x4B514954U);
  }
  auto corrupted = r.tlm.back();
  corrupted[10] ^= 1;
  int bad2 = 0;
  EXPECT_TRUE(decode({corrupted}, &bad2).empty());
  EXPECT_EQ(bad2, 1);
}

TEST(Unit, SamplesAreInTimeOrderWithOffsetsFromThePacketTime) {
  IQK_VERIFIES("SRS-071");
  Rig r;
  std::vector<iqk::plan::Finding> f;
  ASSERT_TRUE(r.dau.load_plan(kPlan, r.now, f));
  r.run_ms(1500);
  const auto pk = decode(r.tlm);
  ASSERT_FALSE(pk.empty());
  std::size_t n = 0;
  for (const auto& p : pk) {
    for (std::size_t i = 1; i < p.samples_count; ++i) {
      EXPECT_GE(p.samples[i].dt_us, p.samples[i - 1].dt_us);
    }
    if (p.samples_count > 0) {
      EXPECT_EQ(p.samples[0].dt_us, 0);
    }
    n += p.samples_count;
  }
  EXPECT_EQ(n, r.dau.samples_sent());
}

TEST(Unit, LateArrivalsAreSortedIntoPlace) {
  IQK_VERIFIES("SRS-071");
  Rig r;
  std::vector<iqk::plan::Finding> f;
  ASSERT_TRUE(r.dau.load_plan(kPlan, r.now, f));
  r.run_ms(600);
  // The IMU keeps sampling, but its frames are held up for 8 ms while module frames keep arriving; then the
  // held frames arrive in one burst. Their samples were taken before the module samples stamped meanwhile.
  r.hold_imu = true;
  r.run_ms(8);
  r.hold_imu = false;
  ASSERT_GE(r.held.size(), 3U);
  r.tlm.clear();
  for (const auto& fr : r.held) {
    r.dau.on_input(Port::imu, fr, r.now, r.tx());
  }
  r.run_ms(40);
  bool checked = false;
  for (const auto& p : decode(r.tlm)) {
    for (std::size_t i = 1; i < p.samples_count; ++i) {
      EXPECT_GE(p.samples[i].dt_us, p.samples[i - 1].dt_us) << "packet " << p.seq;
      checked = true;
    }
  }
  EXPECT_TRUE(checked);
}

TEST(Unit, ImuChannelsAreDecimatedToTheirPlannedRate) {
  IQK_VERIFIES("SRS-073");
  Rig r;
  std::vector<iqk::plan::Finding> f;
  ASSERT_TRUE(r.dau.load_plan(kPlan, r.now, f));
  r.run_ms(500);
  r.tlm.clear();
  r.run_ms(2000);
  std::map<int, int> per_channel;
  for (const auto& p : decode(r.tlm)) {
    for (std::size_t i = 0; i < p.samples_count; ++i) {
      ++per_channel[p.samples[i].channel];
    }
  }
  EXPECT_NEAR(per_channel[2], 800, 8);  // gyro_z at 400 Hz for 2 s
  EXPECT_NEAR(per_channel[3], 400, 4);  // accel_z at 200 Hz
  EXPECT_NEAR(per_channel[0], 2000, 20);
}

TEST(Unit, ModuleCountsBecomeEngineeringValuesWithOverrange) {
  IQK_VERIFIES("SRS-032");
  Rig r;
  std::vector<iqk::plan::Finding> f;
  ASSERT_TRUE(r.dau.load_plan(kPlan, r.now, f));
  r.run_ms(500);
  auto value_of = [&](std::int16_t raw) {
    r.tlm.clear();
    r.run_ms(30, true, raw);
    tlm::TlmPacketSamples last{};
    for (const auto& p : decode(r.tlm)) {
      for (std::size_t i = 0; i < p.samples_count; ++i) {
        if (p.samples[i].channel == 0) {
          last = p.samples[i];  // the newest: earlier ones may predate the change
        }
      }
    }
    return last;
  };
  // 16384 counts of +/-10 V at 250 ue/V: 16384/32767 * 10 V * 250 = 1250.04 ue
  const auto a = value_of(16384);
  EXPECT_NEAR(a.value * 1e-4, 16384.0 / 32767.0 * 10.0 * 250.0, 1e-3);
  EXPECT_EQ(a.flags & 2, 0);
  EXPECT_NE(value_of(32767).flags & 2, 0);   // full scale: over-range
  EXPECT_NE(value_of(-32767).flags & 2, 0);
  EXPECT_EQ(value_of(32766).flags & 2, 0);
}

TEST(Unit, AlarmFlagsFollowThePlanLimits) {
  IQK_VERIFIES("SRS-072");
  Rig r;
  std::vector<iqk::plan::Finding> f;
  ASSERT_TRUE(r.dau.load_plan(kPlan, r.now, f));
  r.run_ms(500);
  auto flags_of = [&](std::int16_t raw) {
    r.tlm.clear();
    r.run_ms(30, true, raw);
    std::uint8_t last = 0xFF;
    for (const auto& p : decode(r.tlm)) {
      for (std::size_t i = 0; i < p.samples_count; ++i) {
        if (p.samples[i].channel == 0) {
          last = p.samples[i].flags;
        }
      }
    }
    return last;
  };
  // 1000 ue = 4 V = 13107 counts
  EXPECT_EQ(flags_of(13000) & 12, 0);
  EXPECT_EQ(flags_of(14000) & 12, 8);    // above the high limit
  EXPECT_EQ(flags_of(-14000) & 12, 4);   // below the low limit
  EXPECT_EQ(flags_of(-13000) & 12, 0);
}

TEST(Unit, DiagnosticsReportTheUnitsState) {
  IQK_VERIFIES("SRS-080");
  Rig r;
  std::vector<iqk::plan::Finding> f;
  ASSERT_TRUE(r.dau.load_plan(kPlan, r.now, f));
  r.run_ms(1500);
  const std::array<std::uint8_t, 1> req{1};
  r.dau.on_input(Port::diag, req, r.now, r.tx());
  ASSERT_EQ(r.diag.size(), 1U);
  ASSERT_EQ(r.diag[0][0], tlm::DiagStatus::kId);
  tlm::DiagStatus s{};
  ASSERT_TRUE(tlm::decode(std::span{r.diag[0]}.subspan(1), s));
  EXPECT_EQ(s.sw_version, (1U << 16) | (4U << 8));
  EXPECT_EQ(s.plan_crc, r.dau.plan_crc());
  EXPECT_EQ(s.uptime_s, 1U);
  EXPECT_NE(s.bit & 16U, 0U);  // plan armed
  EXPECT_NE(s.bit & 2U, 0U);   // module alive
  EXPECT_EQ(s.bit & 4U, 0U);   // no GNSS
  const std::array<std::uint8_t, 1> faults{2};
  r.dau.on_input(Port::diag, faults, r.now, r.tx());
  ASSERT_EQ(r.diag.size(), 2U);
  EXPECT_EQ(r.diag[1][0], tlm::DiagFaults::kId);
}

TEST(Unit, MalformedDiagnosticRequestsAreLoggedNotAnswered) {
  IQK_VERIFIES("SRS-082");
  Rig r;
  const std::array<std::uint8_t, 1> bad{3};
  const std::array<std::uint8_t, 2> long_req{1, 1};
  r.dau.on_input(Port::diag, bad, r.now, r.tx());
  r.dau.on_input(Port::diag, long_req, r.now, r.tx());
  EXPECT_TRUE(r.diag.empty());
  EXPECT_EQ(r.dau.faults().count(iqk::Fault::diag_bad_request), 2U);
}

TEST(Unit, TheImuIsSelfTestedAndStartedAndRestartedWhenItGoesQuiet) {
  IQK_VERIFIES("SRS-023");
  Dau dau{1};
  std::vector<std::vector<std::uint8_t>> imu_out;
  const iqk::Tx tx = [&](Port p, std::span<const std::uint8_t> b) {
    if (p == Port::imu) {
      imu_out.emplace_back(b.begin(), b.end());
    }
  };
  dau.tick(kS, tx);
  ASSERT_EQ(imu_out.size(), 2U);
  EXPECT_EQ(imu_out[0][4], 3);  // self-test first
  EXPECT_EQ(imu_out[1][4], 1);  // then start
  dau.tick(kS + 500 * kMs, tx);
  EXPECT_EQ(imu_out.size(), 2U);  // not repeated within a second
  dau.tick(kS + 1000 * kMs, tx);
  EXPECT_EQ(imu_out.size(), 4U);
  EXPECT_EQ(dau.faults().count(iqk::Fault::imu_lost), 1U);
}
