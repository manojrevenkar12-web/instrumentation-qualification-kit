// CRCs used on the wire. Table-driven, computed at compile time.
#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace iqk::crc {

namespace detail {
constexpr std::array<std::uint16_t, 256> make16() {
  std::array<std::uint16_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint16_t c = static_cast<std::uint16_t>(i << 8U);
    for (int k = 0; k < 8; ++k) {
      const std::uint32_t shifted = static_cast<std::uint32_t>(c) << 1U;
      c = static_cast<std::uint16_t>((c & 0x8000U) != 0U ? (shifted ^ 0x1021U) : shifted);
    }
    t[i] = c;
  }
  return t;
}
constexpr std::array<std::uint32_t, 256> make32() {
  std::array<std::uint32_t, 256> t{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t c = i;
    for (int k = 0; k < 8; ++k) {
      c = (c & 1U) != 0U ? (c >> 1U) ^ 0xEDB88320U : (c >> 1U);
    }
    t[i] = c;
  }
  return t;
}
inline constexpr auto kT16 = make16();
inline constexpr auto kT32 = make32();
}  // namespace detail

// CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final xor. check("123456789") = 0x29B1.
[[nodiscard]] constexpr std::uint16_t crc16(std::span<const std::uint8_t> d, std::uint16_t c = 0xFFFFU) noexcept {
  for (const std::uint8_t b : d) {
    c = static_cast<std::uint16_t>((c << 8U) ^ detail::kT16[((c >> 8U) ^ b) & 0xFFU]);
  }
  return c;
}

// CRC-32/ISO-HDLC (IEEE 802.3). check("123456789") = 0xCBF43926. Pass the previous value to continue.
[[nodiscard]] constexpr std::uint32_t crc32(std::span<const std::uint8_t> d, std::uint32_t prev = 0U) noexcept {
  std::uint32_t c = ~prev;
  for (const std::uint8_t b : d) {
    c = (c >> 8U) ^ detail::kT32[(c ^ b) & 0xFFU];
  }
  return ~c;
}

}  // namespace iqk::crc
