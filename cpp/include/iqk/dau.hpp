// The data acquisition unit's core: drivers, time synchronisation, the plan loader, telemetry and diagnostics,
// wired together. Bytes in with the local time they arrived, bytes out through a Tx. Two units fed the same
// inputs at the same times produce the same bytes, which is what makes record and replay exact.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "iqk/drivers.hpp"
#include "iqk/faultlog.hpp"
#include "iqk/icd/tlm.hpp"
#include "iqk/loader.hpp"
#include "iqk/plan.hpp"
#include "iqk/timesync.hpp"

namespace iqk {

enum class Port : std::uint8_t { imu = 0, gnss = 1, pps = 2, can = 3, ptp_event = 4, ptp_general = 5, diag = 6, tlm = 7 };

// Where the core's output goes. The daemon sends it to sockets and serial ports; tests collect it.
using Tx = std::function<void(Port, std::span<const std::uint8_t>)>;

struct Bit {  // built-in test, DAU-UDP diag_status.bit
  bool imu{false};
  bool aam{false};
  bool gnss{false};
  bool ptp{false};
  bool plan{false};
  bool storage{true};
  [[nodiscard]] std::uint16_t bits() const noexcept;
};

class Dau {
 public:
  explicit Dau(std::uint64_t clock_identity, std::uint8_t ptp_domain = 0) noexcept;

  // Parses and validates a plan; when valid, starts loading it into the modules. Telemetry stops until the
  // new plan is armed.
  bool load_plan(std::string_view text, std::int64_t now, std::vector<plan::Finding>& findings);

  void on_input(Port p, std::span<const std::uint8_t> bytes, std::int64_t local_ns, const Tx& tx);
  void tick(std::int64_t local_ns, const Tx& tx);
  void on_ptp_sent(std::int64_t local_ns) noexcept { ptp_.on_sent(local_ns); }
  void set_boot_slot(std::uint8_t slot) noexcept { slot_ = slot; }

  [[nodiscard]] icd::tlm::DiagStatus status(std::int64_t local_ns) const noexcept;
  [[nodiscard]] Bit bit(std::int64_t local_ns) const noexcept;
  [[nodiscard]] const FaultLog& faults() const noexcept { return log_; }
  [[nodiscard]] const timesync::TimeSync& time() const noexcept { return ts_; }
  [[nodiscard]] const PlanLoader& loader() const noexcept { return loader_; }
  [[nodiscard]] bool armed() const noexcept { return armed_; }
  [[nodiscard]] std::uint32_t plan_crc() const noexcept { return armed_ ? plan_crc_ : 0U; }
  [[nodiscard]] std::uint64_t packets_sent() const noexcept { return tlm_seq_; }
  [[nodiscard]] std::uint64_t samples_sent() const noexcept { return samples_sent_; }

 private:
  struct Out {  // one planned channel
    plan::Channel ch;
    int imu_field{-1};
    int decimate{1};
  };
  struct Sample {
    std::int64_t local_ns;
    std::uint16_t channel;
    double value;
    std::uint8_t flags;
  };
  void on_imu(const ImuReading& r);
  void on_aam(const AamReading& r);
  void add_sample(std::uint16_t ch, std::int64_t local_ns, double value, bool overrange);
  void flush(std::int64_t now, const Tx& tx);
  void diag(std::span<const std::uint8_t> req, std::int64_t now, const Tx& tx);
  void time_events();

  ImuDriver imu_;
  AamDriver aam_;
  Deframer gnss_def_;
  timesync::TimeSync ts_;
  timesync::PtpSlave ptp_;
  PlanLoader loader_;
  FaultLog log_;

  std::optional<plan::Plan> plan_;
  std::vector<Out> outs_;
  std::uint32_t plan_crc_{0};
  bool armed_{false};
  bool nmt_sent_{false};
  bool load_failure_logged_{false};
  bool imu_started_{false};
  std::int64_t last_imu_cmd_{INT64_MIN};
  std::int64_t last_gnss_ok_{INT64_MIN};
  std::uint8_t slot_{0};
  std::int64_t boot_ns_{INT64_MIN};

  std::vector<Sample> frame_;   // reserved once; samples of the current output frame
  std::int64_t frame_start_{INT64_MIN};
  std::uint32_t tlm_seq_{0};
  std::uint64_t samples_sent_{0};
  std::array<std::uint8_t, icd::tlm::TlmPacket::kMaxSize + 4> buf_{};
};

}  // namespace iqk
