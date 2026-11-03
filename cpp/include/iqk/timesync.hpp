// Time synchronisation: a disciplined software clock fed by PTP (IEEE 1588 two-step, end-to-end) and by a GNSS
// timing receiver (PPS edge plus the time message naming it). Deterministic: every input carries the local
// monotonic time at which it happened, and nothing reads a clock.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "iqk/icd/gnss.hpp"
#include "iqk/icd/ptp.hpp"

namespace iqk::timesync {

enum class State : std::uint8_t { free_running = 0, locking = 1, locked = 2, holdover = 3 };
enum class Source : std::uint8_t { none = 0, gnss = 1, ptp = 2 };

inline constexpr std::int64_t kNsPerS = 1'000'000'000;
inline constexpr std::int64_t kGpsEpochUnixS = 315'964'800;  // 1980-01-06T00:00:00Z
inline constexpr std::int64_t kStepThresholdNs = 1'000'000;   // larger offsets are stepped, not slewed

// utc(local) = base_utc + (local - base_local) * (1 + rate). Rate is a fraction (1e-9 = 1 ppb).
class Clock {
 public:
  [[nodiscard]] std::int64_t utc(std::int64_t local) const noexcept;
  void set(std::int64_t local, std::int64_t utc) noexcept;
  void set_rate(std::int64_t local, double rate) noexcept;  // re-anchors at local, keeps continuity
  [[nodiscard]] double rate() const noexcept { return rate_; }
  [[nodiscard]] bool valid() const noexcept { return valid_; }

 private:
  std::int64_t base_local_{0};
  std::int64_t base_utc_{0};
  double rate_{0.0};
  bool valid_{false};
};

struct Event {
  enum class Kind : std::uint8_t { stepped, locked, lost_lock, holdover, free_running, switched_source,
                                   ptp_outlier, gnss_rejected };
  Kind kind{};
  std::int64_t local_ns{};
  std::int64_t value{};  // offset for stepped/lost_lock, new source for switched_source, reason code otherwise
};

class TimeSync {
 public:
  TimeSync() noexcept;

  // GNSS: the PPS edge, then the message that names its second.
  void on_pps(std::int64_t local_ns) noexcept;
  void on_gnss_time(const icd::gnss::TimePulse& m, std::int64_t local_ns) noexcept;

  // PTP, after the exchange: t1 master sends Sync, t2 we receive it, t3 we send Delay_Req, t4 master receives
  // it. t2 and t3 are local monotonic times; t1 and t4 master (UTC) times.
  void on_ptp_exchange(std::int64_t t1, std::int64_t t2, std::int64_t t3, std::int64_t t4) noexcept;

  // Advances timeouts (holdover, free-running). Call at least every 100 ms.
  void tick(std::int64_t local_ns) noexcept;

  [[nodiscard]] std::int64_t utc_ns(std::int64_t local_ns) const noexcept { return clock_.utc(local_ns); }
  [[nodiscard]] bool has_time() const noexcept { return clock_.valid(); }
  [[nodiscard]] State state() const noexcept { return state_; }
  [[nodiscard]] Source source() const noexcept { return active_; }
  [[nodiscard]] std::int64_t last_offset_ns() const noexcept { return last_offset_; }
  [[nodiscard]] std::uint8_t quality() const noexcept;  // DAU-UDP tlm_packet.time_quality
  [[nodiscard]] double rate() const noexcept { return clock_.rate(); }
  [[nodiscard]] bool source_healthy(Source s, std::int64_t local_ns) const noexcept { return healthy(s, local_ns); }

  // Events since the last call (bounded; the oldest are dropped first).
  template <class F>
  void drain_events(F&& f) {
    for (std::size_t i = 0; i < n_events_; ++i) {
      f(events_[i]);
    }
    n_events_ = 0;
  }

 private:
  struct SourceHealth {
    std::int64_t last_good_ns{INT64_MIN};
    std::int64_t good_since_ns{INT64_MIN};
  };
  void measurement(Source s, std::int64_t local_ns, std::int64_t master_ns) noexcept;
  void select(std::int64_t local_ns) noexcept;
  [[nodiscard]] bool healthy(Source s, std::int64_t local_ns) const noexcept;
  [[nodiscard]] int priority(Source s) const noexcept;
  void emit(Event::Kind k, std::int64_t local_ns, std::int64_t v) noexcept;

  Clock clock_{};
  State state_{State::free_running};
  Source active_{Source::none};
  std::array<SourceHealth, 3> health_{};
  std::int64_t last_meas_local_{INT64_MIN};
  std::int64_t last_any_good_{INT64_MIN};
  std::int64_t holdover_since_{INT64_MIN};
  double drift_{0.0};
  int in_threshold_{0};
  std::int64_t last_offset_{0};

  std::optional<std::int64_t> pps_edge_;
  std::array<std::int64_t, 16> delays_{};
  std::size_t n_delays_{0};
  std::size_t delay_pos_{0};

  std::array<Event, 32> events_{};
  std::size_t n_events_{0};
};

// The slave side of the PTP message exchange: matches Sync with Follow_Up and Delay_Req with Delay_Resp by
// sequence id, and hands complete exchanges to TimeSync. Bytes in, bytes out; no sockets.
class PtpSlave {
 public:
  PtpSlave(std::uint64_t clock_identity, std::uint8_t domain) noexcept : id_{clock_identity}, domain_{domain} {}

  // A received datagram. Returns the size of a Delay_Req to send (written into out), or 0.
  std::size_t on_datagram(std::span<const std::uint8_t> d, std::int64_t rx_local, TimeSync& ts,
                          std::span<std::uint8_t> out) noexcept;
  // When the Delay_Req built by on_datagram was actually sent.
  void on_sent(std::int64_t tx_local) noexcept { t3_ = tx_local; }

  [[nodiscard]] std::uint64_t rejected() const noexcept { return rejected_; }

 private:
  std::uint64_t id_;
  std::uint8_t domain_;
  std::uint16_t sync_seq_{0};
  bool have_sync_{false};
  std::int64_t t2_{0};
  std::int64_t t1_{0};
  bool have_t1_{false};
  std::uint16_t req_seq_{0};
  bool req_pending_{false};
  std::int64_t t3_{INT64_MIN};
  std::uint64_t rejected_{0};
};

// UTC nanoseconds of the start of GPS week/tow, given GPS-UTC leap seconds.
[[nodiscard]] constexpr std::int64_t gps_to_utc_ns(std::uint16_t week, std::uint32_t tow_ms, std::int8_t leap_s) noexcept {
  return (kGpsEpochUnixS + static_cast<std::int64_t>(week) * 604'800 - leap_s) * kNsPerS +
         static_cast<std::int64_t>(tow_ms) * 1'000'000;
}

}  // namespace iqk::timesync
