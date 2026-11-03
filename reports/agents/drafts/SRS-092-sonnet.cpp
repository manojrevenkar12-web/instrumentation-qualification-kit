#include <cstdlib>

#include "iqk/config.hpp"
#include "iqk/update.hpp"
#include "support.hpp"

namespace {

using namespace iqk::update;

constexpr std::uint32_t kHw = static_cast<std::uint32_t>(iqk::cfg::unit::hw_id);

struct MemStore final : Store {
  std::array<std::vector<std::uint8_t>, 2> slots;
  State st;
  int state_writes{0};
  bool write_slot(std::uint8_t s, std::span<const std::uint8_t> b) override {
    slots[s].assign(b.begin(), b.end());
    return true;
  }
  std::vector<std::uint8_t> read_slot(std::uint8_t s) const override { return slots[s]; }
  bool write_state(const State& s) override {
    st = s;
    ++state_writes;
    return true;
  }
  State read_state() const override { return st; }
};

struct Rig {
  std::array<std::uint8_t, 32> key{};
  MemStore store;
  Rig() {
    for (std::size_t i = 0; i < key.size(); ++i) key[i] = static_cast<std::uint8_t>(i + 1);
    store.slots[0] = pkg(1, 1);  // confirmed factory image in slot A
  }
  std::vector<std::uint8_t> pkg(std::uint32_t sec, std::uint32_t ver = 1) const {
    Header h{};
    h.hw_id = kHw;
    h.version = ver;
    h.security_version = sec;
    const std::array<std::uint8_t, 16> img{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, static_cast<std::uint8_t>(sec)};
    return build(h, img, key);
  }
  Agent agent() { return Agent{store, public_key_of(key), kHw}; }
  // Installs and confirms a package of the given security version.
  void raise_floor(Agent& a, std::uint32_t sec) {
    ASSERT_EQ(a.install(pkg(sec)), Verdict::ok);
    ASSERT_EQ(a.select().kind, Agent::Boot::trial);
    ASSERT_TRUE(a.confirm());
    ASSERT_EQ(a.state().security_floor, sec);
  }
};

}  // namespace

TEST(AntiRollback, VerifyRefusesBelowFloorAcceptsAtAndAbove) {
  IQK_VERIFIES("SRS-092");
  Rig r;
  EXPECT_EQ(verify(r.pkg(4), public_key_of(r.key), kHw, 5), Verdict::rollback_refused);
  EXPECT_EQ(verify(r.pkg(0), public_key_of(r.key), kHw, 1), Verdict::rollback_refused);
  EXPECT_EQ(verify(r.pkg(5), public_key_of(r.key), kHw, 5), Verdict::ok);
  EXPECT_EQ(verify(r.pkg(6), public_key_of(r.key), kHw, 5), Verdict::ok);
  EXPECT_EQ(verify(r.pkg(0), public_key_of(r.key), kHw, 0), Verdict::ok);
}

TEST(AntiRollback, InstallBelowFloorRefusedAndNothingChanges) {
  IQK_VERIFIES("SRS-092");
  Rig r;
  auto a = r.agent();
  r.raise_floor(a, 5);
  const State before = r.store.st;
  const auto s0 = r.store.slots[0];
  const auto s1 = r.store.slots[1];
  const int writes = r.store.state_writes;
  EXPECT_EQ(a.install(r.pkg(4)), Verdict::rollback_refused);
  EXPECT_EQ(r.store.slots[0], s0);
  EXPECT_EQ(r.store.slots[1], s1);
  EXPECT_EQ(r.store.state_writes, writes);
  EXPECT_EQ(a.state().security_floor, before.security_floor);
  EXPECT_EQ(a.state().active, before.active);
  EXPECT_FALSE(a.state().pending.has_value());
}

TEST(AntiRollback, InstallAtAndAboveFloorAccepted) {
  IQK_VERIFIES("SRS-092");
  Rig r;
  auto a = r.agent();
  r.raise_floor(a, 5);
  EXPECT_EQ(a.install(r.pkg(5)), Verdict::ok);
  EXPECT_TRUE(a.state().pending.has_value());
  Rig r2;
  auto a2 = r2.agent();
  r2.raise_floor(a2, 5);
  EXPECT_EQ(a2.install(r2.pkg(6)), Verdict::ok);
}

TEST(AntiRollback, FloorUsesSecurityVersionNotVersion) {
  IQK_VERIFIES("SRS-092");
  Rig r;
  auto a = r.agent();
  r.raise_floor(a, 5);
  EXPECT_EQ(a.install(r.pkg(4, 99)), Verdict::rollback_refused);
  EXPECT_EQ(a.install(r.pkg(5, 0)), Verdict::ok);
}

TEST(AntiRollback, FloorStartsAtZeroAndInstallDoesNotRaiseIt) {
  IQK_VERIFIES("SRS-092");
  Rig r;
  auto a = r.agent();
  ASSERT_EQ(a.install(r.pkg(7)), Verdict::ok);
  EXPECT_EQ(a.state().security_floor, 0U);
  // Trial boots do not raise it either.
  EXPECT_EQ(a.select().kind, Agent::Boot::trial);
  EXPECT_EQ(a.state().security_floor, 0U);
  // So a lower package is still acceptable until the image is confirmed.
  EXPECT_EQ(a.install(r.pkg(2)), Verdict::ok);
  EXPECT_EQ(a.state().security_floor, 0U);
}

TEST(AntiRollback, ConfirmRaisesFloorToConfirmedImage) {
  IQK_VERIFIES("SRS-092");
  Rig r;
  auto a = r.agent();
  ASSERT_EQ(a.install(r.pkg(7)), Verdict::ok);
  ASSERT_EQ(a.select().kind, Agent::Boot::trial);
  ASSERT_TRUE(a.confirm());
  EXPECT_EQ(a.state().security_floor, 7U);
  EXPECT_EQ(a.install(r.pkg(6)), Verdict::rollback_refused);
  EXPECT_EQ(a.install(r.pkg(7)), Verdict::ok);
}

TEST(AntiRollback, FloorNeverDecreasesWhenConfirmingEqualImage) {
  IQK_VERIFIES("SRS-092");
  Rig r;
  auto a = r.agent();
  r.raise_floor(a, 9);
  ASSERT_EQ(a.install(r.pkg(9, 2)), Verdict::ok);
  ASSERT_EQ(a.select().kind, Agent::Boot::trial);
  ASSERT_TRUE(a.confirm());
  EXPECT_EQ(a.state().security_floor, 9U);
  EXPECT_EQ(a.install(r.pkg(8)), Verdict::rollback_refused);
}

TEST(AntiRollback, FloorDoesNotRiseWhenTrialRolledBack) {
  IQK_VERIFIES("SRS-092");
  Rig r;
  auto a = r.agent();
  r.raise_floor(a, 3);
  ASSERT_EQ(a.install(r.pkg(8)), Verdict::ok);
  for (unsigned i = 0; i < iqk::cfg::update::max_trial_boots; ++i) {
    ASSERT_EQ(a.select().kind, Agent::Boot::trial);
  }
  EXPECT_EQ(a.select().kind, Agent::Boot::rolled_back);
  EXPECT_EQ(a.state().security_floor, 3U);
  EXPECT_FALSE(a.confirm());
  EXPECT_EQ(a.state().security_floor, 3U);
  EXPECT_EQ(a.install(r.pkg(3)), Verdict::ok);
}

TEST(AntiRollback, ConfirmWithNothingPendingLeavesFloor) {
  IQK_VERIFIES("SRS-092");
  Rig r;
  auto a = r.agent();
  r.raise_floor(a, 4);
  EXPECT_FALSE(a.confirm());
  EXPECT_EQ(a.state().security_floor, 4U);
}

TEST(AntiRollback, BootRefusesPendingImageBelowFloor) {
  IQK_VERIFIES("SRS-092");
  Rig r;
  auto a = r.agent();
  ASSERT_EQ(a.install(r.pkg(2)), Verdict::ok);
  r.store.st.security_floor = 3;  // floor rises before the trial boot
  const auto sel = a.select();
  EXPECT_NE(sel.kind, Agent::Boot::trial);
  EXPECT_EQ(sel.kind, Agent::Boot::halted);  // active image (sec 1) is below the floor too
  EXPECT_EQ(sel.why, Verdict::rollback_refused);
  EXPECT_FALSE(a.state().pending.has_value());
}

TEST(AntiRollback, BootRollsBackPendingBelowFloorToActiveAtFloor) {
  IQK_VERIFIES("SRS-092");
  Rig r;
  auto a = r.agent();
  r.raise_floor(a, 5);  // active = slot 1, sec 5
  ASSERT_EQ(a.install(r.pkg(5, 2)), Verdict::ok);
  r.store.slots[*r.store.st.pending] = r.pkg(4);  // pending image replaced by one below the floor
  const auto sel = a.select();
  EXPECT_EQ(sel.kind, Agent::Boot::rolled_back);
  EXPECT_EQ(sel.slot, 1);
  EXPECT_EQ(sel.why, Verdict::rollback_refused);
}

TEST(AntiRollback, ConfirmRefusedForImageBelowFloor) {
  IQK_VERIFIES("SRS-092");
  Rig r;
  auto a = r.agent();
  ASSERT_EQ(a.install(r.pkg(2)), Verdict::ok);
  r.store.st.security_floor = 6;
  EXPECT_FALSE(a.confirm());
  EXPECT_EQ(a.state().security_floor, 6U);
  EXPECT_EQ(a.state().active, 0);
}

TEST(AntiRollback, BootHaltsWhenActiveImageBelowFloorAndNoOtherValid) {
  IQK_VERIFIES("SRS-092");
  Rig r;
  auto a = r.agent();
  r.store.st.security_floor = 2;  // slot A holds sec 1
  const auto sel = a.select();
  EXPECT_EQ(sel.kind, Agent::Boot::halted);
  EXPECT_EQ(sel.why, Verdict::rollback_refused);
}

TEST(AntiRollback, BootAtFloorStillBoots) {
  IQK_VERIFIES("SRS-092");
  Rig r;
  auto a = r.agent();
  r.store.st.security_floor = 1;
  EXPECT_EQ(a.select().kind, Agent::Boot::active);
}

TEST(AntiRollback, FloorPersistsInFileStore) {
  IQK_VERIFIES("SRS-092");
  char tmpl[] = "/tmp/iqk_ar_XXXXXX";
  ASSERT_NE(mkdtemp(tmpl), nullptr);
  Rig r;
  {
    FileStore fs{tmpl};
    ASSERT_TRUE(fs.write_slot(0, r.pkg(1)));
    Agent a{fs, public_key_of(r.key), kHw};
    ASSERT_EQ(a.install(r.pkg(6)), Verdict::ok);
    ASSERT_EQ(a.select().kind, Agent::Boot::trial);
    ASSERT_TRUE(a.confirm());
  }
  FileStore fs2{tmpl};
  Agent a2{fs2, public_key_of(r.key), kHw};
  EXPECT_EQ(a2.state().security_floor, 6U);
  EXPECT_EQ(a2.install(r.pkg(5)), Verdict::rollback_refused);
  EXPECT_EQ(a2.install(r.pkg(6)), Verdict::ok);
}
