// Device drivers. Each is a deterministic state machine: bytes or frames in, with the local time they arrived,
// readings and faults out. No threads, no clocks, no allocation.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "iqk/can.hpp"
#include "iqk/config.hpp"
#include "iqk/faultlog.hpp"
#include "iqk/framing.hpp"
#include "iqk/icd/aam.hpp"
#include "iqk/icd/imu.hpp"

namespace iqk {

struct ImuReading {
  std::int64_t local_ns{};  // acquisition time on the unit's local clock, not arrival time
  icd::imu::ImuSample s{};
};

class ImuDriver {
 public:
  ImuDriver() noexcept : def_{static_cast<std::uint8_t>(icd::imu::ImuSample::kSize)} {}

  // Raw bytes from the serial port. on_reading(const ImuReading&) is called per valid, in-range sample.
  template <class F>
  void on_bytes(std::span<const std::uint8_t> bytes, std::int64_t rx_local, FaultLog& log, F&& on_reading) {
    const auto crc_before = def_.stats().crc_errors;
    def_.feed(bytes, [&](const Frame& f) { on_frame(f, rx_local, log, on_reading); });
    if (def_.stats().crc_errors != crc_before) {
      log.log(Fault::imu_crc, rx_local, static_cast<std::int64_t>(def_.stats().crc_errors));
    }
  }

  // Builds a command frame (IMU-SER imu_command) into out; returns its size.
  [[nodiscard]] static std::size_t command(std::uint8_t cmd, std::span<std::uint8_t> out) noexcept;

  [[nodiscard]] bool alive(std::int64_t now) const noexcept {
    return last_rx_ != INT64_MIN && now - last_rx_ < 50'000'000;  // 20 sample periods
  }
  [[nodiscard]] std::uint64_t samples() const noexcept { return samples_; }
  [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }
  [[nodiscard]] std::uint64_t range_errors() const noexcept { return range_errors_; }
  [[nodiscard]] const DeframerStats& link() const noexcept { return def_.stats(); }
  [[nodiscard]] std::int64_t latency_estimate_ns() const noexcept { return min_offset_; }
  [[nodiscard]] bool bit_passed() const noexcept { return bit_result_ == 0; }

 private:
  template <class F>
  void on_frame(const Frame& f, std::int64_t rx_local, FaultLog& log, F& on_reading) {
    if (f.id == icd::imu::ImuBitResult::kId) {
      icd::imu::ImuBitResult r{};
      if (icd::imu::decode(f.data(), r)) {
        bit_result_ = r.result;
      }
      return;
    }
    if (f.id != icd::imu::ImuSample::kId) {
      return;
    }
    ImuReading out{};
    if (!icd::imu::decode(f.data(), out.s)) {
      return;
    }
    last_rx_ = rx_local;
    if (!icd::imu::in_range(out.s)) {
      ++range_errors_;
      log.log(Fault::imu_range, rx_local, out.s.seq);
      return;
    }
    if (have_seq_) {
      const auto step = static_cast<std::uint16_t>(out.s.seq - last_seq_);
      if (step != 1U) {
        const std::uint64_t missing = step == 0U ? 0U : step - 1U;
        gaps_ += missing;
        if (missing > static_cast<std::uint64_t>(cfg::imu::max_gap_samples) || step == 0U) {
          log.log(Fault::imu_gap, rx_local, static_cast<std::int64_t>(missing));
        }
      }
    }
    have_seq_ = true;
    last_seq_ = out.s.seq;
    out.local_ns = acquisition_time(out.s.t_us, rx_local);
    ++samples_;
    on_reading(static_cast<const ImuReading&>(out));
  }

  // Device time is mapped to local time with a running minimum of (arrival - device time): a sample cannot
  // arrive before it was taken, so the minimum removes transport and scheduling jitter.
  std::int64_t acquisition_time(std::uint32_t t_us, std::int64_t rx_local) noexcept;

  Deframer def_;
  std::int64_t last_rx_{INT64_MIN};
  std::uint64_t samples_{0};
  std::uint64_t gaps_{0};
  std::uint64_t range_errors_{0};
  std::uint16_t last_seq_{0};
  bool have_seq_{false};
  std::uint8_t bit_result_{0xFF};
  // device time unwrapping and latency filter
  std::uint32_t last_tus_{0};
  std::int64_t dev_ns_{0};
  bool have_t_{false};
  std::array<std::int64_t, static_cast<std::size_t>(cfg::imu::latency_window)> offsets_{};
  std::size_t n_off_{0};
  std::size_t off_pos_{0};
  std::int64_t min_offset_{0};
};

struct AamReading {
  int node{};
  int group{};
  std::int64_t local_ns{};
  std::array<std::int16_t, 3> raw{};
};

class AamDriver {
 public:
  static constexpr int kMaxNodes = 32;

  template <class F>
  void on_frame(const CanFrame& f, std::int64_t rx_local, FaultLog& log, F&& on_reading) {
    const std::uint32_t base = f.id & 0x780U;
    const int node = static_cast<int>(f.id & 0x7FU);
    if (node < 1 || node > kMaxNodes) {
      return;
    }
    Node& n = nodes_[static_cast<std::size_t>(node - 1)];
    if (base == icd::aam::AamHeartbeat::kId) {
      icd::aam::AamHeartbeat hb{};
      if (!icd::aam::decode(f.payload(), hb) || !icd::aam::in_range(hb)) {
        ++errors_;
        return;
      }
      n.last_hb = rx_local;
      n.state = hb.state;
      if (hb.errors != 0U && hb.errors != n.errors) {
        log.log(Fault::aam_fault, rx_local, (static_cast<std::int64_t>(node) << 16) | hb.errors);
      }
      n.errors = hb.errors;
      n.lost_logged = false;
      return;
    }
    if (base == icd::aam::AamData::kId) {
      icd::aam::AamData d{};
      if (!icd::aam::decode(f.payload(), d) || !icd::aam::in_range(d)) {
        ++errors_;
        return;
      }
      auto& g = n.groups[d.mux];
      if (g.seen && static_cast<std::uint8_t>(d.counter - g.counter) != 1U) {
        ++gaps_;
        log.log(Fault::aam_gap, rx_local, (static_cast<std::int64_t>(node) << 8) | d.mux);
      }
      g.seen = true;
      g.counter = d.counter;
      on_reading(AamReading{node, d.mux, rx_local, {d.ch_a, d.ch_b, d.ch_c}});
    }
  }

  // Starts supervising a module the plan uses: from now on it must send heartbeats, even if it never has.
  void expect(int node, std::int64_t now) noexcept {
    if (node >= 1 && node <= kMaxNodes) {
      Node& n = nodes_[static_cast<std::size_t>(node - 1)];
      if (n.last_hb == INT64_MIN) {
        n.last_hb = now;
      }
    }
  }

  // Heartbeat supervision; logs each loss once.
  void supervise(std::int64_t now, FaultLog& log) noexcept;
  [[nodiscard]] bool alive(int node, std::int64_t now) const noexcept;
  [[nodiscard]] std::uint8_t state(int node) const noexcept { return nodes_[static_cast<std::size_t>(node - 1)].state; }
  [[nodiscard]] std::uint64_t errors() const noexcept { return errors_; }
  [[nodiscard]] std::uint64_t gaps() const noexcept { return gaps_; }

 private:
  struct Group {
    bool seen{false};
    std::uint8_t counter{0};
  };
  struct Node {
    std::int64_t last_hb{INT64_MIN};
    std::uint8_t state{0};
    std::uint16_t errors{0};
    bool lost_logged{false};
    std::array<Group, 4> groups{};
  };
  std::array<Node, kMaxNodes> nodes_{};
  std::uint64_t errors_{0};
  std::uint64_t gaps_{0};
};

}  // namespace iqk
