// Loads a validated plan into the acquisition modules over the CAN register protocol: every write is
// acknowledged, the configuration is committed, every register is read back, and each module's configuration
// CRC must equal the one computed from the plan. Only then is the plan armed.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "iqk/can.hpp"
#include "iqk/plan.hpp"

namespace iqk {

class PlanLoader {
 public:
  enum class Status : std::uint8_t { idle, busy, done, failed };
  enum class Failure : std::uint8_t { none, timeout, abort, readback_mismatch, crc_mismatch };

  void start(const plan::Plan& p, std::int64_t now) noexcept;
  // The request to send now (first transmission or a retry), if any.
  [[nodiscard]] std::optional<CanFrame> poll(std::int64_t now) noexcept;
  void on_frame(const CanFrame& f, std::int64_t now) noexcept;

  [[nodiscard]] Status status() const noexcept { return status_; }
  [[nodiscard]] Failure failure() const noexcept { return failure_; }
  [[nodiscard]] int failed_node() const noexcept { return failed_node_; }
  [[nodiscard]] std::uint32_t detail() const noexcept { return detail_; }
  [[nodiscard]] std::size_t progress() const noexcept { return pos_; }
  [[nodiscard]] std::size_t total() const noexcept { return ops_.size(); }
  [[nodiscard]] const std::vector<int>& nodes() const noexcept { return nodes_; }

 private:
  struct Op {
    bool write{};
    plan::RegWrite r{};
    std::uint32_t expect{};
  };
  void fail(Failure f, std::uint32_t detail) noexcept;

  std::vector<Op> ops_;
  std::vector<int> nodes_;
  std::size_t pos_{0};
  int attempts_{0};
  std::int64_t sent_at_{0};
  bool waiting_{false};
  Status status_{Status::idle};
  Failure failure_{Failure::none};
  int failed_node_{0};
  std::uint32_t detail_{0};
};

}  // namespace iqk
