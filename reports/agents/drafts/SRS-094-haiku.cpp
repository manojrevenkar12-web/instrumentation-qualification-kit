// SRS-094 Trial boot: a pending image gets update.max_trial_boots boots; unless confirmed by the application
// it is rolled back.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

#include "iqk/config.hpp"
#include "iqk/update.hpp"
#include "support.hpp"

namespace {

using iqk::update::Agent;
using iqk::update::Boot;
using iqk::update::PublicKey;
using iqk::update::State;
using iqk::update::Verdict;

constexpr auto kHw = static_cast<std::uint32_t>(iqk::cfg::unit::hw_id);
constexpr std::uint32_t kSecurityVersion = 1;
constexpr auto kMaxTrialBoots = static_cast<int>(iqk::cfg::update::max_trial_boots);

// In-memory persistence: a reboot is modelled by constructing a new Agent over the same store.
class MemStore final : public iqk::update::Store {
 public:
  bool write_slot(std::uint8_t slot, std::span<const std::uint8_t> bytes) override {
    slots_[slot].assign(bytes.begin(), bytes.end());
    return true;
  }
  [[nodiscard]] std::vector<std::uint8_t> read_slot(std::uint8_t slot) const override {
    const auto it = slots_.find(slot);
    return it == slots_.end() ? std::vector<std::uint8_t>{} : it->second;
  }
  bool write_state(const State& s) override {
    state_ = s;
    return true;
  }
  [[nodiscard]] State read_state() const override { return state_; }

 private:
  std::map<std::uint8_t, std::vector<std::uint8_t>> slots_;
  State state_{};
};

std::array<std::uint8_t, 32> signing_seed() {
  std::array<std::uint8_t, 32> k{};
  for (std::size_t i = 0; i < k.size(); ++i) {
    k[i] = static_cast<std::uint8_t>(i * 7U + 3U);
  }
  return k;
}

PublicKey trust_anchor() { return iqk::update::public_key_of(signing_seed()); }

std::vector<std::uint8_t> package(std::uint32_t version, std::uint8_t fill) {
  iqk::update::Header h{};
  h.hw_id = kHw;
  h.version = version;
  h.security_version = kSecurityVersion;
  const std::vector<std::uint8_t> image(256, fill);
  return iqk::update::build(h, image, signing_seed());
}

// Leaves a confirmed image in the active slot (slot 1), with nothing pending.
void install_confirmed_base(MemStore& st) {
  Agent a{st, trust_anchor(), kHw};
  ASSERT_EQ(a.install(package(1, 0xA1)), Verdict::ok);
  ASSERT_TRUE(a.confirm());
  ASSERT_EQ(st.read_state().active, 1);
  ASSERT_FALSE(st.read_state().pending.has_value());
}

// Installs a new image over the confirmed base; it becomes pending in the inactive slot (0).
void install_pending(MemStore& st, std::uint32_t version, std::uint8_t fill) {
  Agent a{st, trust_anchor(), kHw};
  ASSERT_EQ(a.install(package(version, fill)), Verdict::ok);
  ASSERT_TRUE(st.read_state().pending.has_value());
}

}  // namespace

TEST(TrialBoot, PendingImageBootsExactlyMaxTrialBootsThenIsRolledBack) {
  IQK_VERIFIES("SRS-094");
  MemStore st;
  install_confirmed_base(st);
  const std::uint8_t confirmed_slot = st.read_state().active;
  install_pending(st, 2, 0xB2);
  const std::uint8_t pending_slot = *st.read_state().pending;
  ASSERT_NE(pending_slot, confirmed_slot);

  // Each boot is a fresh agent on the persisted store, as after a reboot.
  for (int i = 1; i <= kMaxTrialBoots; ++i) {
    const auto sel = Agent{st, trust_anchor(), kHw}.select();
    EXPECT_EQ(sel.kind, Boot::trial) << "boot " << i << " of " << kMaxTrialBoots;
    EXPECT_EQ(sel.slot, pending_slot) << "boot " << i;
    EXPECT_EQ(static_cast<int>(st.read_state().trial_boots), i) << "boot " << i;
    EXPECT_EQ(st.read_state().pending, std::optional<std::uint8_t>{pending_slot}) << "boot " << i;
  }

  // The boot after the last trial boot is not a trial: the unconfirmed image is rolled back.
  const auto sel = Agent{st, trust_anchor(), kHw}.select();
  EXPECT_EQ(sel.kind, Boot::rolled_back);
  EXPECT_EQ(sel.slot, confirmed_slot);
  EXPECT_FALSE(st.read_state().pending.has_value());
  EXPECT_EQ(st.read_state().active, confirmed_slot);
}

TEST(TrialBoot, RolledBackImageIsNotTrialledAgain) {
  IQK_VERIFIES("SRS-094");
  MemStore st;
  install_confirmed_base(st);
  const std::uint8_t confirmed_slot = st.read_state().active;
  install_pending(st, 2, 0xB2);
  for (int i = 0; i < kMaxTrialBoots; ++i) {
    (void)Agent{st, trust_anchor(), kHw}.select();
  }
  ASSERT_EQ(Agent{st, trust_anchor(), kHw}.select().kind, Boot::rolled_back);

  for (int i = 0; i < 3; ++i) {
    const auto sel = Agent{st, trust_anchor(), kHw}.select();
    EXPECT_EQ(sel.kind, Boot::active) << "boot after rollback " << i;
    EXPECT_EQ(sel.slot, confirmed_slot);
  }
}

TEST(TrialBoot, ConfirmAfterLastAllowedTrialBootKeepsImage) {
  IQK_VERIFIES("SRS-094");
  MemStore st;
  install_confirmed_base(st);
  install_pending(st, 2, 0xB2);
  const std::uint8_t pending_slot = *st.read_state().pending;
  for (int i = 0; i < kMaxTrialBoots; ++i) {
    ASSERT_EQ(Agent{st, trust_anchor(), kHw}.select().kind, Boot::trial);
  }

  // Confirmed on the last trial boot, before the boot that would roll it back.
  ASSERT_TRUE(Agent{st, trust_anchor(), kHw}.confirm());
  EXPECT_EQ(st.read_state().active, pending_slot);
  EXPECT_FALSE(st.read_state().pending.has_value());

  for (int i = 0; i < kMaxTrialBoots + 2; ++i) {
    const auto sel = Agent{st, trust_anchor(), kHw}.select();
    EXPECT_EQ(sel.kind, Boot::active) << "boot " << i;
    EXPECT_EQ(sel.slot, pending_slot) << "boot " << i;
  }
  EXPECT_EQ(st.read_state().active, pending_slot);
}

TEST(TrialBoot, ConfirmedDuringFirstTrialBootIsNotRolledBack) {
  IQK_VERIFIES("SRS-094");
  MemStore st;
  install_confirmed_base(st);
  install_pending(st, 2, 0xB2);
  const std::uint8_t pending_slot = *st.read_state().pending;

  ASSERT_EQ(Agent{st, trust_anchor(), kHw}.select().kind, Boot::trial);
  ASSERT_TRUE(Agent{st, trust_anchor(), kHw}.confirm());

  for (int i = 0; i < kMaxTrialBoots + 2; ++i) {
    const auto sel = Agent{st, trust_anchor(), kHw}.select();
    EXPECT_EQ(sel.kind, Boot::active) << "boot " << i;
    EXPECT_EQ(sel.slot, pending_slot) << "boot " << i;
  }
  EXPECT_EQ(st.read_state().trial_boots, 0);
}

TEST(TrialBoot, ConfirmAfterRollbackIsRefusedAndKeepsConfirmedImage) {
  IQK_VERIFIES("SRS-094");
  MemStore st;
  install_confirmed_base(st);
  const std::uint8_t confirmed_slot = st.read_state().active;
  install_pending(st, 2, 0xB2);
  for (int i = 0; i < kMaxTrialBoots; ++i) {
    ASSERT_EQ(Agent{st, trust_anchor(), kHw}.select().kind, Boot::trial);
  }
  ASSERT_EQ(Agent{st, trust_anchor(), kHw}.select().kind, Boot::rolled_back);

  EXPECT_FALSE(Agent{st, trust_anchor(), kHw}.confirm());
  EXPECT_EQ(st.read_state().active, confirmed_slot);
  EXPECT_FALSE(st.read_state().pending.has_value());
  EXPECT_EQ(Agent{st, trust_anchor(), kHw}.select().slot, confirmed_slot);
}

TEST(TrialBoot, EachPendingImageGetsAFullTrialBudget) {
  IQK_VERIFIES("SRS-094");
  MemStore st;
  install_confirmed_base(st);
  install_pending(st, 2, 0xB2);
  // Use part of the first image's budget, then install a replacement pending image.
  ASSERT_EQ(Agent{st, trust_anchor(), kHw}.select().kind, Boot::trial);
  ASSERT_EQ(Agent{st, trust_anchor(), kHw}.select().kind, Boot::trial);
  install_pending(st, 3, 0xC3);
  const std::uint8_t pending_slot = *st.read_state().pending;
  EXPECT_EQ(st.read_state().trial_boots, 0);

  for (int i = 1; i <= kMaxTrialBoots; ++i) {
    const auto sel = Agent{st, trust_anchor(), kHw}.select();
    EXPECT_EQ(sel.kind, Boot::trial) << "boot " << i;
    EXPECT_EQ(sel.slot, pending_slot) << "boot " << i;
  }
  EXPECT_EQ(Agent{st, trust_anchor(), kHw}.select().kind, Boot::rolled_back);
}

TEST(TrialBoot, ConfirmWithNothingPendingIsRefused) {
  IQK_VERIFIES("SRS-094");
  MemStore st;
  install_confirmed_base(st);
  const State before = st.read_state();

  EXPECT_FALSE(Agent{st, trust_anchor(), kHw}.confirm());
  const State after = st.read_state();
  EXPECT_EQ(after.active, before.active);
  EXPECT_FALSE(after.pending.has_value());
}
