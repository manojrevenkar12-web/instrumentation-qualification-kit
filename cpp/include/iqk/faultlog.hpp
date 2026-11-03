// Fault log: a fixed-capacity ring of timestamped fault records. A burst of the same fault within one second
// is folded into one record with a count, so a babbling device cannot push older faults out.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "iqk/config.hpp"

namespace iqk {

enum class Fault : std::uint16_t {
  imu_gap = 1,
  imu_crc = 2,
  imu_range = 3,
  imu_lost = 4,
  aam_lost = 10,
  aam_gap = 11,
  aam_fault = 12,
  time_stepped = 20,
  time_lost_lock = 21,
  time_holdover = 22,
  time_free_running = 23,
  time_switched = 24,
  gnss_rejected = 25,
  ptp_outlier = 26,
  plan_rejected = 30,
  plan_load_failed = 31,
  diag_bad_request = 40,
};

struct FaultRecord {
  std::int64_t local_ns{};
  Fault code{};
  std::int64_t detail{};
  std::uint32_t count{};
};

class FaultLog {
 public:
  static constexpr std::size_t kCapacity = static_cast<std::size_t>(cfg::diag::fault_log_capacity);
  static constexpr std::int64_t kFoldNs = 1'000'000'000;

  void log(Fault code, std::int64_t local_ns, std::int64_t detail) noexcept {
    ++total_;
    for (std::size_t k = 0; k < n_; ++k) {
      FaultRecord& r = at(n_ - 1 - k);
      if (local_ns - r.local_ns > kFoldNs) {
        break;
      }
      if (r.code == code) {
        ++r.count;
        r.detail = detail;
        return;
      }
    }
    if (n_ == kCapacity) {
      head_ = (head_ + 1) % kCapacity;
      --n_;
      ++overwritten_;
    }
    ring_[(head_ + n_) % kCapacity] = FaultRecord{local_ns, code, detail, 1};
    ++n_;
  }

  [[nodiscard]] std::size_t size() const noexcept { return n_; }
  [[nodiscard]] const FaultRecord& operator[](std::size_t i) const noexcept { return ring_[(head_ + i) % kCapacity]; }
  [[nodiscard]] std::uint64_t total() const noexcept { return total_; }
  [[nodiscard]] std::uint64_t overwritten() const noexcept { return overwritten_; }
  [[nodiscard]] std::uint32_t count(Fault code) const noexcept {
    std::uint32_t c = 0;
    for (std::size_t i = 0; i < n_; ++i) {
      if ((*this)[i].code == code) {
        c += (*this)[i].count;
      }
    }
    return c;
  }

 private:
  FaultRecord& at(std::size_t i) noexcept { return ring_[(head_ + i) % kCapacity]; }
  std::array<FaultRecord, kCapacity> ring_{};
  std::size_t head_{0};
  std::size_t n_{0};
  std::uint64_t total_{0};
  std::uint64_t overwritten_{0};
};

}  // namespace iqk
