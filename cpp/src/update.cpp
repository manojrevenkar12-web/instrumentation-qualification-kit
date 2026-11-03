#include "iqk/update.hpp"

#include <fcntl.h>
#include <openssl/evp.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <memory>

#include "iqk/config.hpp"
#include "iqk/crc.hpp"
#include "iqk/wire.hpp"

namespace iqk::update {

namespace {

constexpr std::array<std::uint8_t, 4> kMagic{'I', 'Q', 'K', 'U'};
constexpr std::uint16_t kFormat = 1;
constexpr auto kMaxImage = static_cast<std::uint32_t>(cfg::update::max_image_bytes);

struct PkeyFree {
  void operator()(EVP_PKEY* p) const noexcept { EVP_PKEY_free(p); }
};
struct MdFree {
  void operator()(EVP_MD_CTX* p) const noexcept { EVP_MD_CTX_free(p); }
};

bool ed25519_verify(const PublicKey& pk, std::span<const std::uint8_t> msg, std::span<const std::uint8_t> sig) {
  std::unique_ptr<EVP_PKEY, PkeyFree> key{EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pk.data(), pk.size())};
  std::unique_ptr<EVP_MD_CTX, MdFree> ctx{EVP_MD_CTX_new()};
  if (!key || !ctx || EVP_DigestVerifyInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1) {
    return false;
  }
  return EVP_DigestVerify(ctx.get(), sig.data(), sig.size(), msg.data(), msg.size()) == 1;
}

void put_header(const Header& h, std::span<std::uint8_t> out) {
  std::copy(kMagic.begin(), kMagic.end(), out.begin());
  wire::put(out, 4, kFormat, 2, wire::Order::little);
  wire::put(out, 6, h.hw_id, 4, wire::Order::little);
  wire::put(out, 10, h.version, 4, wire::Order::little);
  wire::put(out, 14, h.security_version, 4, wire::Order::little);
  wire::put(out, 18, h.image_size, 4, wire::Order::little);
  std::copy(h.sha256.begin(), h.sha256.end(), out.begin() + 22);
}

std::string slot_path(const std::string& dir, std::uint8_t slot) {
  return dir + (slot == 0 ? "/slot_a.pkg" : "/slot_b.pkg");
}

bool atomic_write(const std::string& path, std::span<const std::uint8_t> bytes) {
  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) {
    return false;
  }
  std::size_t off = 0;
  while (off < bytes.size()) {
    const ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
    if (n <= 0) {
      ::close(fd);
      return false;
    }
    off += static_cast<std::size_t>(n);
  }
  const bool synced = ::fsync(fd) == 0;
  ::close(fd);
  return synced && std::rename(tmp.c_str(), path.c_str()) == 0;
}

std::vector<std::uint8_t> read_all(const std::string& path) {
  std::vector<std::uint8_t> out;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    return out;
  }
  std::array<std::uint8_t, 65536> buf{};
  std::size_t n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
    out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n));
  }
  std::fclose(f);
  return out;
}

}  // namespace

const char* to_string(Verdict v) noexcept {
  switch (v) {
    case Verdict::ok: return "ok";
    case Verdict::truncated: return "truncated";
    case Verdict::bad_magic: return "bad_magic";
    case Verdict::bad_format: return "bad_format";
    case Verdict::wrong_hardware: return "wrong_hardware";
    case Verdict::too_large: return "too_large";
    case Verdict::bad_signature: return "bad_signature";
    case Verdict::bad_hash: return "bad_hash";
    case Verdict::rollback_refused: return "rollback_refused";
    case Verdict::write_failed: return "write_failed";
    case Verdict::readback_failed: return "readback_failed";
  }
  return "unknown";
}

std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> d) {
  std::array<std::uint8_t, 32> out{};
  unsigned int n = 0;
  EVP_Digest(d.data(), d.size(), out.data(), &n, EVP_sha256(), nullptr);
  return out;
}

Verdict verify(std::span<const std::uint8_t> pkg, const PublicKey& anchor, std::uint32_t hw_id,
               std::uint32_t security_floor, Header* out) {
  if (pkg.size() < kPrefixSize) {
    return Verdict::truncated;
  }
  if (!std::equal(kMagic.begin(), kMagic.end(), pkg.begin())) {
    return Verdict::bad_magic;
  }
  if (wire::get(pkg, 4, 2, wire::Order::little) != kFormat) {
    return Verdict::bad_format;
  }
  Header h{};
  h.hw_id = static_cast<std::uint32_t>(wire::get(pkg, 6, 4, wire::Order::little));
  h.version = static_cast<std::uint32_t>(wire::get(pkg, 10, 4, wire::Order::little));
  h.security_version = static_cast<std::uint32_t>(wire::get(pkg, 14, 4, wire::Order::little));
  h.image_size = static_cast<std::uint32_t>(wire::get(pkg, 18, 4, wire::Order::little));
  std::copy(pkg.begin() + 22, pkg.begin() + 54, h.sha256.begin());
  // The signature covers the header, so nothing below is trusted until it verifies.
  if (!ed25519_verify(anchor, pkg.first(kHeaderSize), pkg.subspan(kHeaderSize, kSignatureSize))) {
    return Verdict::bad_signature;
  }
  if (h.hw_id != hw_id) {
    return Verdict::wrong_hardware;
  }
  if (h.image_size > kMaxImage) {
    return Verdict::too_large;
  }
  if (pkg.size() != kPrefixSize + h.image_size) {
    return Verdict::truncated;
  }
  if (sha256(pkg.subspan(kPrefixSize)) != h.sha256) {
    return Verdict::bad_hash;
  }
  if (h.security_version < security_floor) {
    return Verdict::rollback_refused;
  }
  if (out != nullptr) {
    *out = h;
  }
  return Verdict::ok;
}

std::vector<std::uint8_t> build(const Header& in, std::span<const std::uint8_t> image,
                                std::span<const std::uint8_t, 32> private_key) {
  Header h = in;
  h.image_size = static_cast<std::uint32_t>(image.size());
  h.sha256 = sha256(image);
  std::vector<std::uint8_t> pkg(kPrefixSize + image.size());
  put_header(h, pkg);
  std::unique_ptr<EVP_PKEY, PkeyFree> key{
      EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, private_key.data(), private_key.size())};
  std::unique_ptr<EVP_MD_CTX, MdFree> ctx{EVP_MD_CTX_new()};
  std::size_t sig_len = kSignatureSize;
  if (!key || !ctx || EVP_DigestSignInit(ctx.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
      EVP_DigestSign(ctx.get(), pkg.data() + kHeaderSize, &sig_len, pkg.data(), kHeaderSize) != 1) {
    return {};
  }
  std::copy(image.begin(), image.end(), pkg.begin() + static_cast<std::ptrdiff_t>(kPrefixSize));
  return pkg;
}

PublicKey public_key_of(std::span<const std::uint8_t, 32> private_key) {
  PublicKey pk{};
  std::unique_ptr<EVP_PKEY, PkeyFree> key{
      EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, private_key.data(), private_key.size())};
  std::size_t n = pk.size();
  if (key) {
    EVP_PKEY_get_raw_public_key(key.get(), pk.data(), &n);
  }
  return pk;
}

bool FileStore::write_slot(std::uint8_t slot, std::span<const std::uint8_t> bytes) {
  return atomic_write(slot_path(dir_, slot), bytes);
}

std::vector<std::uint8_t> FileStore::read_slot(std::uint8_t slot) const { return read_all(slot_path(dir_, slot)); }

bool FileStore::write_state(const State& s) {
  std::array<std::uint8_t, 12> b{};
  b[0] = s.active;
  b[1] = s.pending.has_value() ? static_cast<std::uint8_t>(0x10U | *s.pending) : 0U;
  b[2] = s.trial_boots;
  wire::put(b, 4, s.security_floor, 4, wire::Order::little);
  wire::put(b, 8, crc::crc32(std::span{b}.first(8)), 4, wire::Order::little);
  return atomic_write(dir_ + "/state.bin", b);
}

State FileStore::read_state() const {
  const auto b = read_all(dir_ + "/state.bin");
  State s{};
  if (b.size() != 12 || wire::get(b, 8, 4, wire::Order::little) != crc::crc32(std::span{b}.first(8))) {
    return s;  // no state or a corrupt one: boot slot A, nothing pending, floor 0 (images still must verify)
  }
  s.active = b[0] & 1U;
  if ((b[1] & 0x10U) != 0U) {
    s.pending = static_cast<std::uint8_t>(b[1] & 1U);
  }
  s.trial_boots = b[2];
  s.security_floor = static_cast<std::uint32_t>(wire::get(b, 4, 4, wire::Order::little));
  return s;
}

Verdict Agent::install(std::span<const std::uint8_t> pkg) {
  State s = store_.read_state();
  const Verdict v = verify(pkg, anchor_, hw_id_, s.security_floor);
  if (v != Verdict::ok) {
    return v;
  }
  const std::uint8_t target = static_cast<std::uint8_t>(1U - s.active);
  if (!store_.write_slot(target, pkg)) {
    return Verdict::write_failed;
  }
  const auto back = store_.read_slot(target);
  if (back.size() != pkg.size() || !std::equal(back.begin(), back.end(), pkg.begin())) {
    return Verdict::readback_failed;
  }
  s.pending = target;
  s.trial_boots = 0;
  return store_.write_state(s) ? Verdict::ok : Verdict::write_failed;
}

Agent::Selection Agent::select() {
  State s = store_.read_state();
  if (s.pending) {
    const std::uint8_t p = *s.pending;
    const auto img = store_.read_slot(p);
    const Verdict v = verify(img, anchor_, hw_id_, s.security_floor);
    if (v == Verdict::ok && s.trial_boots < cfg::update::max_trial_boots) {
      ++s.trial_boots;
      store_.write_state(s);
      return {Boot::trial, p, Verdict::ok};
    }
    // Trial boots used up, or the pending image no longer verifies: roll back to the confirmed slot.
    s.pending.reset();
    s.trial_boots = 0;
    store_.write_state(s);
    const auto act = store_.read_slot(s.active);
    const Verdict va = verify(act, anchor_, hw_id_, s.security_floor);
    return va == Verdict::ok ? Selection{Boot::rolled_back, s.active, v} : Selection{Boot::halted, s.active, va};
  }
  const auto act = store_.read_slot(s.active);
  const Verdict va = verify(act, anchor_, hw_id_, s.security_floor);
  if (va == Verdict::ok) {
    return {Boot::active, s.active, Verdict::ok};
  }
  // The confirmed image is damaged: the other slot may still hold a verified image.
  const auto other = static_cast<std::uint8_t>(1U - s.active);
  if (verify(store_.read_slot(other), anchor_, hw_id_, s.security_floor) == Verdict::ok) {
    s.active = other;
    store_.write_state(s);
    return {Boot::rolled_back, other, va};
  }
  return {Boot::halted, s.active, va};
}

bool Agent::confirm() {
  State s = store_.read_state();
  if (!s.pending) {
    return false;
  }
  Header h{};
  if (verify(store_.read_slot(*s.pending), anchor_, hw_id_, s.security_floor, &h) != Verdict::ok) {
    return false;
  }
  s.active = *s.pending;
  s.pending.reset();
  s.trial_boots = 0;
  s.security_floor = std::max(s.security_floor, h.security_version);
  return store_.write_state(s);
}

}  // namespace iqk::update
