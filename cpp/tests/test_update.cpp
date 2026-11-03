#include <gtest/gtest.h>

#include <filesystem>
#include <optional>

#include "iqk/update.hpp"
#include "support.hpp"

namespace up = iqk::update;
using Boot = up::Agent::Boot;

namespace {

constexpr std::uint32_t kHw = 0x0F710001U;

std::array<std::uint8_t, 32> seed(std::uint8_t k) {
  std::array<std::uint8_t, 32> s{};
  for (std::size_t i = 0; i < s.size(); ++i) {
    s[i] = static_cast<std::uint8_t>(k * 31 + i);
  }
  return s;
}
const auto kKey = seed(1);
const auto kOtherKey = seed(2);
const up::PublicKey kAnchor = up::public_key_of(kKey);

std::vector<std::uint8_t> image(std::uint8_t fill, std::size_t n = 4096) { return std::vector<std::uint8_t>(n, fill); }

std::vector<std::uint8_t> pkg(std::uint32_t version, std::uint32_t secver, std::uint8_t fill = 0x11,
                              const std::array<std::uint8_t, 32>& key = kKey, std::uint32_t hw = kHw) {
  up::Header h{};
  h.hw_id = hw;
  h.version = version;
  h.security_version = secver;
  return up::build(h, image(fill), key);
}

// In-memory store; can be told to "lose power" before the nth write of a kind.
struct MemStore : up::Store {
  std::array<std::vector<std::uint8_t>, 2> slots;
  up::State st{};
  bool has_state{false};
  int fail_slot_writes_after{-1};
  int fail_state_writes_after{-1};
  int slot_writes{0};
  int state_writes{0};
  bool write_slot(std::uint8_t s, std::span<const std::uint8_t> b) override {
    if (fail_slot_writes_after >= 0 && slot_writes++ >= fail_slot_writes_after) {
      return false;  // power lost: the atomic write never happened
    }
    slots[s].assign(b.begin(), b.end());
    return true;
  }
  std::vector<std::uint8_t> read_slot(std::uint8_t s) const override { return slots[s]; }
  bool write_state(const up::State& s) override {
    if (fail_state_writes_after >= 0 && state_writes++ >= fail_state_writes_after) {
      return false;
    }
    st = s;
    has_state = true;
    return true;
  }
  up::State read_state() const override { return has_state ? st : up::State{}; }
};

// A unit that runs version 1.0 (security version 1) from slot A.
MemStore factory() {
  MemStore m;
  m.slots[0] = pkg(0x010000, 1, 0xAA);
  m.st.security_floor = 1;
  m.has_state = true;
  return m;
}
}  // namespace

TEST(Update, OnlyPackagesSignedByTheReleaseKeyAreAccepted) {
  IQK_VERIFIES("SRS-090");
  EXPECT_EQ(up::verify(pkg(2, 1), kAnchor, kHw, 0), up::Verdict::ok);
  EXPECT_EQ(up::verify(pkg(2, 1, 0x11, kOtherKey), kAnchor, kHw, 0), up::Verdict::bad_signature);
  auto tampered_header = pkg(2, 1);
  tampered_header[10] ^= 1;  // version
  EXPECT_EQ(up::verify(tampered_header, kAnchor, kHw, 0), up::Verdict::bad_signature);
  auto tampered_image = pkg(2, 1);
  tampered_image.back() ^= 1;
  EXPECT_EQ(up::verify(tampered_image, kAnchor, kHw, 0), up::Verdict::bad_hash);
  auto truncated = pkg(2, 1);
  truncated.pop_back();
  EXPECT_EQ(up::verify(truncated, kAnchor, kHw, 0), up::Verdict::truncated);
  auto magic = pkg(2, 1);
  magic[0] = 'X';
  EXPECT_EQ(up::verify(magic, kAnchor, kHw, 0), up::Verdict::bad_magic);
}

TEST(Update, ARejectedPackageWritesNothing) {
  IQK_VERIFIES("SRS-090");
  MemStore m = factory();
  up::Agent a{m, kAnchor, kHw};
  EXPECT_EQ(a.install(pkg(2, 1, 0x11, kOtherKey)), up::Verdict::bad_signature);
  EXPECT_TRUE(m.slots[1].empty());
  EXPECT_FALSE(m.st.pending.has_value());
}

TEST(Update, PackagesForOtherHardwareAreRefused) {
  IQK_VERIFIES("SRS-091");
  EXPECT_EQ(up::verify(pkg(2, 1, 0x11, kKey, kHw + 1), kAnchor, kHw, 0), up::Verdict::wrong_hardware);
}

TEST(Update, OlderSecurityVersionsAreRefusedAndTheFloorRisesOnlyOnConfirm) {
  IQK_VERIFIES("SRS-092");
  MemStore m = factory();
  up::Agent a{m, kAnchor, kHw};
  EXPECT_EQ(a.install(pkg(0x000900, 0)), up::Verdict::rollback_refused);
  ASSERT_EQ(a.install(pkg(0x020000, 3)), up::Verdict::ok);
  EXPECT_EQ(m.st.security_floor, 1U);  // not yet: the new image has not proven itself
  ASSERT_EQ(a.select().kind, Boot::trial);
  ASSERT_TRUE(a.confirm());
  EXPECT_EQ(m.st.security_floor, 3U);
  EXPECT_EQ(a.install(pkg(0x010500, 2)), up::Verdict::rollback_refused);
}

TEST(Update, InstallWritesTheInactiveSlotAndVerifiesIt) {
  IQK_VERIFIES("SRS-093");
  MemStore m = factory();
  const auto active_before = m.slots[0];
  up::Agent a{m, kAnchor, kHw};
  ASSERT_EQ(a.install(pkg(0x020000, 1, 0x22)), up::Verdict::ok);
  EXPECT_EQ(m.slots[0], active_before);
  EXPECT_EQ(m.slots[1].back(), 0x22);
  EXPECT_EQ(m.st.pending, std::optional<std::uint8_t>{1});
  EXPECT_EQ(m.st.active, 0);
}

TEST(Update, AnUnconfirmedImageIsRolledBackAfterThreeTrialBoots) {
  IQK_VERIFIES("SRS-094");
  MemStore m = factory();
  up::Agent a{m, kAnchor, kHw};
  ASSERT_EQ(a.install(pkg(0x020000, 1)), up::Verdict::ok);
  for (int i = 0; i < 3; ++i) {
    const auto s = a.select();
    EXPECT_EQ(s.kind, Boot::trial);
    EXPECT_EQ(s.slot, 1);
  }
  const auto s = a.select();
  EXPECT_EQ(s.kind, Boot::rolled_back);
  EXPECT_EQ(s.slot, 0);
  EXPECT_EQ(a.select().kind, Boot::active);
  EXPECT_FALSE(m.st.pending.has_value());
}

TEST(Update, AConfirmedImageBecomesActive) {
  IQK_VERIFIES("SRS-094");
  MemStore m = factory();
  up::Agent a{m, kAnchor, kHw};
  ASSERT_EQ(a.install(pkg(0x020000, 1)), up::Verdict::ok);
  ASSERT_EQ(a.select().kind, Boot::trial);
  ASSERT_TRUE(a.confirm());
  const auto s = a.select();
  EXPECT_EQ(s.kind, Boot::active);
  EXPECT_EQ(s.slot, 1);
}

TEST(Update, BootVerifiesTheImageItIsAboutToRun) {
  IQK_VERIFIES("SRS-095");
  MemStore m = factory();
  m.slots[1] = pkg(0x010000, 1, 0xBB);
  up::Agent a{m, kAnchor, kHw};
  m.slots[0][200] ^= 1;  // the active image is damaged in storage
  const auto s = a.select();
  EXPECT_EQ(s.kind, Boot::rolled_back);
  EXPECT_EQ(s.slot, 1);
  EXPECT_EQ(s.why, up::Verdict::bad_hash);
  m.slots[1][200] ^= 1;  // both damaged: refuse to boot
  EXPECT_EQ(a.select().kind, Boot::halted);
}

TEST(Update, ADamagedPendingImageIsNeverBooted) {
  IQK_VERIFIES("SRS-095");
  MemStore m = factory();
  up::Agent a{m, kAnchor, kHw};
  ASSERT_EQ(a.install(pkg(0x020000, 1)), up::Verdict::ok);
  m.slots[1][300] ^= 1;
  const auto s = a.select();
  EXPECT_EQ(s.kind, Boot::rolled_back);
  EXPECT_EQ(s.slot, 0);
}

TEST(Update, PowerLossAtAnyWriteLeavesABootableVerifiedImage) {
  IQK_VERIFIES("SRS-096");
  for (int slot_fail : {-1, 0}) {
    for (int state_fail : {-1, 0, 1, 2, 3}) {
      MemStore m = factory();
      m.fail_slot_writes_after = slot_fail;
      m.fail_state_writes_after = state_fail;
      up::Agent a{m, kAnchor, kHw};
      (void)a.install(pkg(0x020000, 2));
      (void)a.select();
      (void)a.confirm();
      // Power comes back with whatever made it to storage.
      m.fail_slot_writes_after = -1;
      m.fail_state_writes_after = -1;
      const auto s = a.select();
      ASSERT_NE(s.kind, Boot::halted) << slot_fail << "/" << state_fail;
      EXPECT_EQ(up::verify(m.slots[s.slot], kAnchor, kHw, m.st.security_floor), up::Verdict::ok);
    }
  }
}

TEST(Update, FileStoreRoundTripsAndRejectsACorruptState) {
  IQK_VERIFIES("SRS-096");
  const auto dir = std::filesystem::temp_directory_path() / "iqk-update-test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  up::FileStore fs{dir.string()};
  up::State s{};
  s.active = 1;
  s.pending = 0;
  s.trial_boots = 2;
  s.security_floor = 7;
  ASSERT_TRUE(fs.write_state(s));
  const auto back = fs.read_state();
  EXPECT_EQ(back.active, 1);
  EXPECT_EQ(back.pending, std::optional<std::uint8_t>{0});
  EXPECT_EQ(back.security_floor, 7U);
  // Flip a byte: the CRC fails and the store falls back to defaults (images must still verify to boot).
  {
    std::FILE* f = std::fopen((dir / "state.bin").c_str(), "r+b");
    ASSERT_NE(f, nullptr);
    std::fseek(f, 4, SEEK_SET);
    std::fputc(0x55, f);
    std::fclose(f);
  }
  EXPECT_EQ(fs.read_state().security_floor, 0U);
  const auto p = pkg(0x020000, 1);
  ASSERT_TRUE(fs.write_slot(1, p));
  EXPECT_EQ(fs.read_slot(1), p);
  EXPECT_FALSE(std::filesystem::exists(dir / "slot_b.pkg.tmp"));
  std::filesystem::remove_all(dir);
}
