#include "iqk/drivers.hpp"

#include <algorithm>

namespace iqk {

std::size_t ImuDriver::command(std::uint8_t cmd, std::span<std::uint8_t> out) noexcept {
  icd::imu::ImuCommand c{};
  c.cmd = cmd;
  std::array<std::uint8_t, icd::imu::ImuCommand::kSize> payload{};
  if (icd::imu::encode(c, payload) == 0U) {
    return 0;
  }
  return encode_frame(static_cast<std::uint8_t>(icd::imu::ImuCommand::kId), payload, out);
}

std::int64_t ImuDriver::acquisition_time(std::uint32_t t_us, std::int64_t rx_local) noexcept {
  if (!have_t_) {
    have_t_ = true;
    dev_ns_ = static_cast<std::int64_t>(t_us) * 1000;
  } else {
    dev_ns_ += static_cast<std::int64_t>(static_cast<std::uint32_t>(t_us - last_tus_)) * 1000;  // wraps at 2^32 us
  }
  last_tus_ = t_us;
  const std::int64_t off = rx_local - dev_ns_;
  offsets_[off_pos_] = off;
  off_pos_ = (off_pos_ + 1) % offsets_.size();
  n_off_ = std::min(n_off_ + 1, offsets_.size());
  min_offset_ = *std::min_element(offsets_.begin(), offsets_.begin() + static_cast<std::ptrdiff_t>(n_off_));
  return dev_ns_ + min_offset_;
}

void AamDriver::supervise(std::int64_t now, FaultLog& log) noexcept {
  for (int i = 0; i < kMaxNodes; ++i) {
    Node& n = nodes_[static_cast<std::size_t>(i)];
    if (n.last_hb == INT64_MIN || n.lost_logged) {
      continue;
    }
    if (now - n.last_hb > cfg::aam::heartbeat_timeout_ms * 1'000'000) {
      n.lost_logged = true;
      log.log(Fault::aam_lost, now, i + 1);
    }
  }
}

bool AamDriver::alive(int node, std::int64_t now) const noexcept {
  if (node < 1 || node > kMaxNodes) {
    return false;
  }
  const Node& n = nodes_[static_cast<std::size_t>(node - 1)];
  return n.last_hb != INT64_MIN && now - n.last_hb <= cfg::aam::heartbeat_timeout_ms * 1'000'000;
}

}  // namespace iqk
