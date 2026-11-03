// Serial framing for IMU-SER and GNSS-SER: A5 5A | id u8 | len u8 | payload[len] | crc16 (big-endian on wire,
// CCITT-FALSE over id, len and payload). The deframer accepts bytes in any chunking, resynchronises after
// corruption without losing a following good frame, and never allocates.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace iqk {

struct Frame {
  std::uint8_t id{};
  std::uint8_t len{};
  std::array<std::uint8_t, 255> payload{};
  [[nodiscard]] std::span<const std::uint8_t> data() const noexcept { return {payload.data(), len}; }
};

struct DeframerStats {
  std::uint64_t frames{};
  std::uint64_t crc_errors{};
  std::uint64_t dropped_bytes{};  // bytes discarded while searching for a sync word
};

inline constexpr std::uint8_t kSync0 = 0xA5U;
inline constexpr std::uint8_t kSync1 = 0x5AU;
inline constexpr std::size_t kFrameOverhead = 6;  // sync 2, id 1, len 1, crc 2

// Writes one frame into out; returns its size, or 0 when out is too small.
[[nodiscard]] std::size_t encode_frame(std::uint8_t id, std::span<const std::uint8_t> payload,
                                       std::span<std::uint8_t> out) noexcept;

class Deframer {
 public:
  // max_len: the longest payload the attached device may send; longer lengths are treated as corruption.
  explicit Deframer(std::uint8_t max_len) noexcept : max_len_{max_len} {}

  // Feeds bytes; calls on_frame(const Frame&) for each valid frame, in order.
  template <class F>
  void feed(std::span<const std::uint8_t> bytes, F&& on_frame) {
    for (const std::uint8_t b : bytes) {
      push(b);
      while (try_extract()) {
        on_frame(frame_);
      }
    }
  }

  [[nodiscard]] const DeframerStats& stats() const noexcept { return stats_; }

 private:
  static constexpr std::size_t kBuf = 2 * (255 + kFrameOverhead);
  void push(std::uint8_t b) noexcept;
  bool try_extract() noexcept;
  void drop(std::size_t n) noexcept;

  std::uint8_t max_len_;
  std::array<std::uint8_t, kBuf> buf_{};
  std::size_t n_{0};
  Frame frame_{};
  DeframerStats stats_{};
};

}  // namespace iqk
