// Classic CAN frames and their UDP encapsulation (13 bytes: id u32 LE, dlc u8, data[8]), used by the emulated
// bus. On a target with SocketCAN the same frames go through iqk::SocketCanPort instead (apps/daud.cpp).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "iqk/wire.hpp"

namespace iqk {

struct CanFrame {
  std::uint32_t id{};
  std::uint8_t dlc{};
  std::array<std::uint8_t, 8> data{};
  [[nodiscard]] std::span<const std::uint8_t> payload() const noexcept { return {data.data(), dlc}; }
};

inline constexpr std::size_t kCanUdpSize = 13;

[[nodiscard]] inline std::size_t encode_can_udp(const CanFrame& f, std::span<std::uint8_t> out) noexcept {
  if (out.size() < kCanUdpSize || f.dlc > 8U) {
    return 0;
  }
  wire::put(out, 0, f.id, 4, wire::Order::little);
  out[4] = f.dlc;
  for (std::size_t i = 0; i < 8; ++i) {
    out[5 + i] = i < f.dlc ? f.data[i] : 0U;
  }
  return kCanUdpSize;
}

[[nodiscard]] inline bool decode_can_udp(std::span<const std::uint8_t> in, CanFrame& f) noexcept {
  if (in.size() != kCanUdpSize) {
    return false;
  }
  f.id = static_cast<std::uint32_t>(wire::get(in, 0, 4, wire::Order::little));
  f.dlc = in[4];
  if (f.dlc > 8U || f.id > 0x7FFU) {  // classic 11-bit identifiers only
    return false;
  }
  for (std::size_t i = 0; i < 8; ++i) {
    f.data[i] = in[5 + i];
  }
  return true;
}

}  // namespace iqk
