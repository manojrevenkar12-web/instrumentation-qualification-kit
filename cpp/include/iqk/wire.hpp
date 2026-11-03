// Byte-order helpers for wire formats. Explicit, allocation-free and free of undefined behaviour:
// signed values go through their unsigned counterpart, which C++20 defines as two's complement.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace iqk::wire {

enum class Order : std::uint8_t { little, big };

inline constexpr void put(std::span<std::uint8_t> out, std::size_t off, std::uint64_t v, std::size_t n,
                          Order o) noexcept {
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t shift = 8U * (o == Order::little ? i : (n - 1U - i));
    out[off + i] = static_cast<std::uint8_t>((v >> shift) & 0xFFU);
  }
}

[[nodiscard]] inline constexpr std::uint64_t get(std::span<const std::uint8_t> in, std::size_t off, std::size_t n,
                                                 Order o) noexcept {
  std::uint64_t v = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t shift = 8U * (o == Order::little ? i : (n - 1U - i));
    v |= static_cast<std::uint64_t>(in[off + i]) << shift;
  }
  return v;
}

template <class T>
[[nodiscard]] inline constexpr std::uint64_t to_raw(T v) noexcept {
  using U = std::make_unsigned_t<T>;
  return static_cast<std::uint64_t>(static_cast<U>(v));
}

template <class T>
[[nodiscard]] inline constexpr T from_raw(std::uint64_t v) noexcept {
  using U = std::make_unsigned_t<T>;
  return static_cast<T>(static_cast<U>(v));
}

}  // namespace iqk::wire
