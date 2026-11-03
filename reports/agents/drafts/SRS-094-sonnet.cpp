#include "support.hpp"

#include <array>
#include <map>

#include "iqk/config.hpp"
#include "iqk/update.hpp"

namespace {
using namespace iqk::update;

constexpr std::uint32_t kHw = 0x0F710001;
constexpr int kMax = static_cast<int>(iqk::cfg::update::max_trial_boots);

class MemStore final : public Store {
 public:
  bool write_slot(std::uint8_t s, std::span<const std::uint8_t> b) override {
    slots[s].assign(b.begin(), b.end());
    return true;
  }
  std::vector<std::uint8_t> read_slot(std::uint8_t s) const override {
    auto it = slots.find(s);
    return it == slots.end() ? std::vector<std::uint8_t>{} : it->second;
  }
  bool write_state(const State& s) override {
    st = s;
    return true;
  }
  State read_state() const override { return st; }
  std::map<std::uint8_t, std::vector<std::uint8_t>> slots;
  State st;
};

struct Fixture {
  std::array<std::uint8_t, 32> priv{1, 2, 3, 4, 5, 6, 7, 8, 9};
  MemStore store;
  PublicKey pub = public_key_of(priv);
  Agent agent{store, pub, kHw};

  std::vector<std::uint8_t> pkg(std::uint32_t version) {
    std::vector<std::uint8_t> img(64, static_cast<std::uint8_t>(version));
    Header h{};
    h.hw_id = kHw;
    h.version = version;
    h.security_version = 0;
    h.image_size = static_cast<std::uint32_t>(img.size());
    h.sha256 = sha256(img);
    return build(h, img, priv);
  }
  // Slot 0 holds a confirmed image; a new image is installed as pending in slot 1.
  Fixture() {
    store.slots[0] = pkg(1);
    store.st.active = 0;
  }
  void install_pending(std::uint32_t v = 2) { ASSERT_EQ(agent.install(pkg(v)), Verdict::ok); }
};
}  // namespace

TEST(TrialBoot, ConfigValueIsThree) {
  IQK_VERIFIES("SRS-094");
  EXPECT_EQ(kMax, 3);
}

TEST(TrialBoot, PendingImageGetsExactlyMaxTrialBootsThenRollsBack) {
  IQK_VERIFIES("SRS-094");
  Fixture f;
  f.install_pending();
  for (int i = 1; i <= kMax; ++i) {
    auto sel = f.agent.select();
    EXPECT_EQ(sel.kind, Agent::Boot::trial) << "boot " << i;
    EXPECT_EQ(sel.slot, 1) << "boot " << i;
    EXPECT_EQ(f.agent.state().trial_boots, i);
    EXPECT_EQ(f.agent.state().pending, std::optional<std::uint8_t>{1});
  }
  auto sel = f.agent.select();
  EXPECT_EQ(sel.kind, Agent::Boot::rolled_back);
  EXPECT_EQ(sel.slot, 0);
  auto st = f.agent.state();
  EXPECT_FALSE(st.pending.has_value());
  EXPECT_EQ(st.active, 0);
  EXPECT_EQ(st.trial_boots, 0);
}

TEST(TrialBoot, RollbackIsSticky) {
  IQK_VERIFIES("SRS-094");
  Fixture f;
  f.install_pending();
  for (int i = 0; i < kMax + 1; ++i) (void)f.agent.select();
  for (int i = 0; i < 3; ++i) {
    auto sel = f.agent.select();
    EXPECT_EQ(sel.kind, Agent::Boot::active);
    EXPECT_EQ(sel.slot, 0);
  }
  EXPECT_FALSE(f.agent.confirm());
  EXPECT_EQ(f.agent.state().active, 0);
}

TEST(TrialBoot, ConfirmOnLastTrialBootKeepsImage) {
  IQK_VERIFIES("SRS-094");
  Fixture f;
  f.install_pending();
  for (int i = 0; i < kMax; ++i) EXPECT_EQ(f.agent.select().kind, Agent::Boot::trial);
  EXPECT_TRUE(f.agent.confirm());
  auto st = f.agent.state();
  EXPECT_EQ(st.active, 1);
  EXPECT_FALSE(st.pending.has_value());
  for (int i = 0; i < 3; ++i) {
    auto sel = f.agent.select();
    EXPECT_EQ(sel.kind, Agent::Boot::active);
    EXPECT_EQ(sel.slot, 1);
  }
}

TEST(TrialBoot, ConfirmAfterFirstTrialBootKeepsImage) {
  IQK_VERIFIES("SRS-094");
  Fixture f;
  f.install_pending();
  EXPECT_EQ(f.agent.select().kind, Agent::Boot::trial);
  EXPECT_TRUE(f.agent.confirm());
  auto sel = f.agent.select();
  EXPECT_EQ(sel.kind, Agent::Boot::active);
  EXPECT_EQ(sel.slot, 1);
}

TEST(TrialBoot, NewInstallRestartsTrialCount) {
  IQK_VERIFIES("SRS-094");
  Fixture f;
  f.install_pending(2);
  for (int i = 0; i < kMax - 1; ++i) EXPECT_EQ(f.agent.select().kind, Agent::Boot::trial);
  f.install_pending(3);
  EXPECT_EQ(f.agent.state().trial_boots, 0);
  for (int i = 0; i < kMax; ++i) EXPECT_EQ(f.agent.select().kind, Agent::Boot::trial) << i;
  EXPECT_EQ(f.agent.select().kind, Agent::Boot::rolled_back);
}

TEST(TrialBoot, NoPendingImageMeansNoTrialBoot) {
  IQK_VERIFIES("SRS-094");
  Fixture f;
  auto sel = f.agent.select();
  EXPECT_EQ(sel.kind, Agent::Boot::active);
  EXPECT_EQ(sel.slot, 0);
  EXPECT_EQ(f.agent.state().trial_boots, 0);
  EXPECT_FALSE(f.agent.confirm());
}
