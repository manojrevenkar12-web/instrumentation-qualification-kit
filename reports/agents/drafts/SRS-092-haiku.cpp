// SRS-092 Anti-rollback: a package below the security floor is refused; the floor rises only when a new image
// is confirmed.
#include <gtest/gtest.h>
#include <stdlib.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "iqk/update.hpp"
#include "support.hpp"

namespace {

namespace upd = iqk::update;
using Boot = upd::Agent::Boot;
using Verdict = upd::Verdict;

constexpr std::uint32_t kHw = 0x0F710001U;  // config/unit.yaml: unit.hw_id

// The signing seed used by the tests (a raw Ed25519 seed); the unit trusts its public key.
std::array<std::uint8_t, 32> seed() {
  std::array<std::uint8_t, 32> s{};
  s.fill(0x42);
  return s;
}

upd::PublicKey anchor() { return upd::public_key_of(seed()); }

// A signed package for this hardware. The image content follows image_fill, so distinct packages differ.
std::vector<std::uint8_t> make_pkg(std::uint32_t security_version, std::uint32_t version = 1,
                                   std::uint8_t image_fill = 0x5A) {
  upd::Header h{};
  h.hw_id = kHw;
  h.version = version;
  h.security_version = security_version;
  const std::vector<std::uint8_t> image(64, image_fill);
  return upd::build(h, image, seed());
}

// In-memory persistence: slots and state are kept as the Store contract requires.
class MemStore final : public upd::Store {
 public:
  bool write_slot(std::uint8_t slot, std::span<const std::uint8_t> bytes) override {
    slots_[slot].assign(bytes.begin(), bytes.end());
    return true;
  }
  [[nodiscard]] std::vector<std::uint8_t> read_slot(std::uint8_t slot) const override { return slots_[slot]; }
  bool write_state(const upd::State& s) override {
    state_ = s;
    return true;
  }
  [[nodiscard]] upd::State read_state() const override { return state_; }

  std::array<std::vector<std::uint8_t>, 2> slots_{};
  upd::State state_{};
};

// Installs a package, trial-boots it and confirms it, so the unit runs it and its version becomes the floor.
void confirm_image(upd::Agent& a, const std::vector<std::uint8_t>& pkg) {
  ASSERT_EQ(a.install(pkg), Verdict::ok);
  ASSERT_EQ(a.select().kind, Boot::trial);
  ASSERT_TRUE(a.confirm());
}

bool same_state(const upd::State& x, const upd::State& y) {
  return x.active == y.active && x.pending == y.pending && x.trial_boots == y.trial_boots &&
         x.security_floor == y.security_floor;
}

// A temporary directory for FileStore, removed when the test ends.
struct TempDir {
  std::string path;
  TempDir() {
    std::string t = (std::filesystem::temp_directory_path() / "iqk_srs092_XXXXXX").string();
    std::vector<char> buf(t.begin(), t.end());
    buf.push_back('\0');
    if (::mkdtemp(buf.data()) != nullptr) {
      path = buf.data();
    }
  }
  ~TempDir() {
    std::error_code ec;
    if (!path.empty()) {
      std::filesystem::remove_all(path, ec);
    }
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

}  // namespace

// The verifier refuses a package whose security version is below the floor, and accepts one at the floor.
TEST(AntiRollback, VerifyRefusesBelowFloorAndAcceptsAtFloor) {
  IQK_VERIFIES("SRS-092");
  const auto pkg_sv4 = make_pkg(4);
  const auto pkg_sv5 = make_pkg(5);
  EXPECT_EQ(upd::verify(pkg_sv4, anchor(), kHw, 5), Verdict::rollback_refused);  // one below the floor
  EXPECT_EQ(upd::verify(pkg_sv5, anchor(), kHw, 5), Verdict::ok);                // exactly the floor
  EXPECT_EQ(upd::verify(make_pkg(6), anchor(), kHw, 5), Verdict::ok);            // above the floor
  EXPECT_EQ(upd::verify(make_pkg(0), anchor(), kHw, 0), Verdict::ok);            // no floor: anything verifies
}

// Install refuses a below-floor package and leaves both slots and the state exactly as they were.
TEST(AntiRollback, InstallRefusesBelowFloorWithoutTouchingStore) {
  IQK_VERIFIES("SRS-092");
  MemStore st;
  upd::Agent a{st, anchor(), kHw};
  confirm_image(a, make_pkg(7));  // floor becomes 7
  ASSERT_EQ(st.state_.security_floor, 7U);
  const auto slots_before = st.slots_;
  const upd::State state_before = st.state_;

  EXPECT_EQ(a.install(make_pkg(6)), Verdict::rollback_refused);
  EXPECT_EQ(st.slots_, slots_before);
  EXPECT_TRUE(same_state(st.state_, state_before));
  EXPECT_FALSE(st.state_.pending.has_value());

  EXPECT_EQ(a.install(make_pkg(7)), Verdict::ok);  // the floor itself is accepted
  EXPECT_TRUE(st.state_.pending.has_value());
}

// Installing does not raise the floor: until a new image is confirmed, an older package is still accepted.
TEST(AntiRollback, InstallDoesNotRaiseFloor) {
  IQK_VERIFIES("SRS-092");
  MemStore st;
  upd::Agent a{st, anchor(), kHw};
  ASSERT_EQ(a.install(make_pkg(7)), Verdict::ok);
  EXPECT_EQ(st.state_.security_floor, 0U);
  ASSERT_EQ(a.select().kind, Boot::trial);
  EXPECT_EQ(st.state_.security_floor, 0U);  // a trial boot does not raise it either

  // Still floor 0, so an older package installs (it replaces the unconfirmed one in the inactive slot).
  EXPECT_EQ(a.install(make_pkg(2, 1, 0x33)), Verdict::ok);
  EXPECT_EQ(st.state_.security_floor, 0U);
}

// Confirming raises the floor to the confirmed image's security version, not to some other value.
TEST(AntiRollback, FloorRisesToConfirmedVersion) {
  IQK_VERIFIES("SRS-092");
  MemStore st;
  upd::Agent a{st, anchor(), kHw};
  confirm_image(a, make_pkg(4, 1, 0x11));
  EXPECT_EQ(st.state_.security_floor, 4U);

  confirm_image(a, make_pkg(4, 2, 0x22));  // same security version: the floor stays where it is
  EXPECT_EQ(st.state_.security_floor, 4U);

  confirm_image(a, make_pkg(9, 3, 0x33));  // higher: the floor rises to exactly 9
  EXPECT_EQ(st.state_.security_floor, 9U);
  EXPECT_EQ(upd::verify(make_pkg(8), anchor(), kHw, st.state_.security_floor), Verdict::rollback_refused);
}

// An unconfirmed image never raises the floor, even when it is booted through all its trial boots and rolled back.
TEST(AntiRollback, TrialBootsDoNotRaiseFloor) {
  IQK_VERIFIES("SRS-092");
  MemStore st;
  upd::Agent a{st, anchor(), kHw};
  confirm_image(a, make_pkg(1, 1, 0x11));  // floor 1, active slot 1
  ASSERT_EQ(a.install(make_pkg(8, 2, 0x22)), Verdict::ok);

  // cfg::update::max_trial_boots == 3 (config/unit.yaml)
  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(a.select().kind, Boot::trial) << "trial boot " << i + 1;
  }
  const auto sel = a.select();
  EXPECT_EQ(sel.kind, Boot::rolled_back);
  EXPECT_EQ(sel.slot, std::uint8_t{1});
  EXPECT_EQ(st.state_.security_floor, 1U);
  EXPECT_FALSE(a.confirm());  // nothing pending to confirm
  EXPECT_EQ(st.state_.security_floor, 1U);
}

// A failed confirmation (here: the pending image no longer verifies) leaves the floor where it was.
TEST(AntiRollback, FailedConfirmDoesNotRaiseFloor) {
  IQK_VERIFIES("SRS-092");
  MemStore st;
  upd::Agent a{st, anchor(), kHw};
  EXPECT_FALSE(a.confirm());  // nothing pending
  EXPECT_EQ(st.state_.security_floor, 0U);

  ASSERT_EQ(a.install(make_pkg(8)), Verdict::ok);
  ASSERT_EQ(a.select().kind, Boot::trial);
  st.slots_[0].back() ^= 0x01U;  // corrupt the pending image (slot 0)
  EXPECT_FALSE(a.confirm());
  EXPECT_EQ(st.state_.security_floor, 0U);
}

// The floor comes from the confirmed image: an older package confirmed after a newer one sets the floor to its own version.
TEST(AntiRollback, FloorComesFromConfirmedImageNotFromInstalledOne) {
  IQK_VERIFIES("SRS-092");
  MemStore st;
  upd::Agent a{st, anchor(), kHw};
  ASSERT_EQ(a.install(make_pkg(7)), Verdict::ok);
  ASSERT_EQ(a.install(make_pkg(2, 1, 0x33)), Verdict::ok);  // replaces the sv7 package before it was confirmed
  ASSERT_EQ(a.select().kind, Boot::trial);
  ASSERT_TRUE(a.confirm());
  EXPECT_EQ(st.state_.security_floor, 2U);
}

// Boot never selects an image below the floor: with the confirmed slot damaged, the older image is not a fallback.
TEST(AntiRollback, BootDoesNotFallBackToImageBelowFloor) {
  IQK_VERIFIES("SRS-092");
  MemStore st;
  upd::Agent a{st, anchor(), kHw};
  confirm_image(a, make_pkg(3, 1, 0x11));  // slot 1
  confirm_image(a, make_pkg(5, 2, 0x22));  // slot 0, floor 5
  ASSERT_EQ(st.state_.security_floor, 5U);
  st.slots_[0].back() ^= 0x01U;  // the confirmed sv5 image is damaged

  const upd::State before = st.state_;
  const auto sel = a.select();
  EXPECT_EQ(sel.kind, Boot::halted);  // slot 1 holds sv3 < floor 5, so it must not boot
  EXPECT_TRUE(same_state(st.state_, before));
}

// Boundary: an older image exactly at the floor is still a valid fallback.
TEST(AntiRollback, BootFallsBackToImageAtFloor) {
  IQK_VERIFIES("SRS-092");
  MemStore st;
  upd::Agent a{st, anchor(), kHw};
  confirm_image(a, make_pkg(3, 1, 0x11));  // slot 1
  confirm_image(a, make_pkg(3, 2, 0x22));  // slot 0, floor 3
  ASSERT_EQ(st.state_.security_floor, 3U);
  st.slots_[0].back() ^= 0x01U;

  const auto sel = a.select();
  EXPECT_EQ(sel.kind, Boot::rolled_back);
  EXPECT_EQ(sel.slot, std::uint8_t{1});
  EXPECT_EQ(st.state_.active, std::uint8_t{1});
}

// A pending image below the floor is never trial-booted. (The floor is set directly in the store here, as a
// newer build would leave it; the unit's own paths only raise it.)
TEST(AntiRollback, PendingImageBelowFloorIsNeverBooted) {
  IQK_VERIFIES("SRS-092");
  MemStore st;
  upd::Agent a{st, anchor(), kHw};
  confirm_image(a, make_pkg(3, 1, 0x11));  // slot 1, active
  ASSERT_EQ(a.install(make_pkg(4, 2, 0x22)), Verdict::ok);  // pending in slot 0
  ASSERT_TRUE(st.state_.pending.has_value());

  st.state_.security_floor = 6;
  const auto sel = a.select();
  EXPECT_NE(sel.kind, Boot::trial);
  EXPECT_EQ(sel.why, Verdict::rollback_refused);
  EXPECT_FALSE(st.state_.pending.has_value());
}

// The floor is persisted and survives a reboot (a new Agent over the same files).
TEST(AntiRollback, FloorSurvivesReopen) {
  IQK_VERIFIES("SRS-092");
  TempDir dir;
  ASSERT_FALSE(dir.path.empty());
  {
    upd::FileStore st{dir.path};
    upd::Agent a{st, anchor(), kHw};
    confirm_image(a, make_pkg(7));
  }
  upd::FileStore st{dir.path};
  EXPECT_EQ(st.read_state().security_floor, 7U);
  upd::Agent a{st, anchor(), kHw};
  EXPECT_EQ(a.install(make_pkg(6)), Verdict::rollback_refused);
  EXPECT_EQ(a.install(make_pkg(7)), Verdict::ok);
}
