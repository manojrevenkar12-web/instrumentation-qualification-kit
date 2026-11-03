// Secure software update with two slots (A/B), signed packages, anti-rollback and a trial boot.
//
// Package (all integers little-endian):
//   magic "IQKU" | format u16 = 1 | hw_id u32 | version u32 | security_version u32 | image_size u32 |
//   image_sha256 [32] | signature [64] (Ed25519 over everything before it) | image [image_size]
//
// Install writes only the inactive slot and verifies what it wrote before marking it pending. The boot
// selector verifies the signature and hash of whatever it is about to boot (the software model of a secure
// boot chain), gives a pending image a bounded number of trial boots, and rolls back unless the application
// confirms it healthy. The persisted state is written last and atomically, so losing power at any point
// leaves a bootable, verified image.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace iqk::update {

inline constexpr std::size_t kHeaderSize = 4 + 2 + 4 + 4 + 4 + 4 + 32;  // 54
inline constexpr std::size_t kSignatureSize = 64;
inline constexpr std::size_t kPrefixSize = kHeaderSize + kSignatureSize;   // 118

using PublicKey = std::array<std::uint8_t, 32>;

struct Header {
  std::uint32_t hw_id{};
  std::uint32_t version{};
  std::uint32_t security_version{};
  std::uint32_t image_size{};
  std::array<std::uint8_t, 32> sha256{};
};

enum class Verdict : std::uint8_t {
  ok,
  truncated,
  bad_magic,
  bad_format,
  wrong_hardware,
  too_large,
  bad_signature,
  bad_hash,
  rollback_refused,
  write_failed,
  readback_failed,
};
[[nodiscard]] const char* to_string(Verdict v) noexcept;

// Verifies a package against the trust anchor, the hardware and the anti-rollback floor.
[[nodiscard]] Verdict verify(std::span<const std::uint8_t> pkg, const PublicKey& anchor, std::uint32_t hw_id,
                             std::uint32_t security_floor, Header* out = nullptr);

[[nodiscard]] std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> d);

// Builds and signs a package (host-side tool and tests). private_key is a raw 32-byte Ed25519 seed.
[[nodiscard]] std::vector<std::uint8_t> build(const Header& h, std::span<const std::uint8_t> image,
                                              std::span<const std::uint8_t, 32> private_key);
[[nodiscard]] PublicKey public_key_of(std::span<const std::uint8_t, 32> private_key);

struct State {
  std::uint8_t active{0};                 // slot that last booted and was confirmed
  std::optional<std::uint8_t> pending;    // slot awaiting its trial boots
  std::uint8_t trial_boots{0};
  std::uint32_t security_floor{0};        // monotonic: packages below it never install or boot
};

// Persistence. Writes of a slot or of the state must be atomic for the guarantees above.
class Store {
 public:
  virtual ~Store() = default;
  virtual bool write_slot(std::uint8_t slot, std::span<const std::uint8_t> bytes) = 0;
  [[nodiscard]] virtual std::vector<std::uint8_t> read_slot(std::uint8_t slot) const = 0;
  virtual bool write_state(const State& s) = 0;
  [[nodiscard]] virtual State read_state() const = 0;
};

// A directory: slot_a.pkg, slot_b.pkg, state.bin; each replaced by write-to-temporary, fsync, rename.
class FileStore final : public Store {
 public:
  explicit FileStore(std::string dir) : dir_{std::move(dir)} {}
  bool write_slot(std::uint8_t slot, std::span<const std::uint8_t> bytes) override;
  [[nodiscard]] std::vector<std::uint8_t> read_slot(std::uint8_t slot) const override;
  bool write_state(const State& s) override;
  [[nodiscard]] State read_state() const override;

 private:
  std::string dir_;
};

class Agent {
 public:
  Agent(Store& s, PublicKey anchor, std::uint32_t hw_id) : store_{s}, anchor_{anchor}, hw_id_{hw_id} {}

  // Verifies, writes the inactive slot, reads it back, then marks it pending.
  Verdict install(std::span<const std::uint8_t> pkg);

  enum class Boot : std::uint8_t { active, trial, rolled_back, halted };
  struct Selection {
    Boot kind{Boot::halted};
    std::uint8_t slot{0};
    Verdict why{Verdict::ok};
  };
  // What the boot loader boots now. Counts a trial boot when it boots a pending image.
  Selection select();
  // The application calls this once its built-in test has passed on the trial image.
  bool confirm();

  [[nodiscard]] State state() const { return store_.read_state(); }

 private:
  Store& store_;
  PublicKey anchor_;
  std::uint32_t hw_id_;
};

}  // namespace iqk::update
