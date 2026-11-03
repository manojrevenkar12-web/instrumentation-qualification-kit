// The measurement plan: what the planning software asks the unit to acquire. The unit is the last line of
// defence between a plan and the hardware, so every plan is parsed strictly and validated against the
// interface documents and the unit's configuration before a single register is written.
//
// Plans are parsed and validated at configuration time, so this module may allocate; the acquisition path
// (framing, drivers, telemetry) does not.
//
// Format (text, one statement per line, '#' starts a comment):
//   iqk-plan 1
//   campaign <id>
//   channel <name> aam:<node>:<input> rate=<hz> range_mv=<mv> [filter=<f>] [scale=<k>] [offset=<c>]
//                                      [unit=<u>] [alarm=<lo>,<hi>]
//   channel <name> imu:<field> rate=<hz> [alarm=<lo>,<hi>]
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace iqk::plan {

enum class Kind : std::uint8_t { aam, imu };

struct Channel {
  std::string name;
  Kind kind{Kind::aam};
  int node{0};       // aam
  int input{0};      // aam
  std::string field; // imu: gyro_x .. temp
  double rate_hz{0};
  int range_mv{0};   // aam
  std::string filter{"none"};
  double scale{1.0};   // engineering units per volt (aam)
  double offset{0.0};
  std::string unit{"V"};
  std::optional<std::pair<double, double>> alarm;
  int line{0};
};

struct Plan {
  std::string campaign;
  std::vector<Channel> channels;
};

struct Finding {
  std::string rule;  // PLN-xx
  int line{0};       // 0 when not tied to a line
  std::string detail;
};

struct RegWrite {
  int node{};
  std::uint16_t index{};
  std::uint8_t sub{};
  std::uint32_t value{};
};

// Strict parse: any syntax problem is a finding and the plan is not usable.
[[nodiscard]] std::optional<Plan> parse(std::string_view text, std::vector<Finding>& findings);

// All semantic rules; returns true when there are no findings.
[[nodiscard]] bool validate(const Plan& p, std::vector<Finding>& findings);

// Worst-case CAN bus load of the plan (0..1), and the telemetry bit rate it needs.
[[nodiscard]] double can_bus_load(const Plan& p);
[[nodiscard]] double telemetry_bps(const Plan& p);

// Canonical text (fixed formatting and order) and its CRC-32, carried in every telemetry packet.
[[nodiscard]] std::string canonical(const Plan& p);
[[nodiscard]] std::uint32_t crc(const Plan& p);

// The register writes that configure every acquisition module the plan uses (all inputs of a used module
// are written, so nothing from a previous plan survives), and the configuration CRC each module must report.
[[nodiscard]] std::vector<RegWrite> registers(const Plan& p);
[[nodiscard]] std::uint32_t expected_module_crc(const Plan& p, int node);

// Worst-case bits of a classic CAN frame with an 11-bit identifier and n data bytes, bit stuffing included.
[[nodiscard]] constexpr int can_frame_bits(int n) noexcept { return 8 * n + 47 + (34 + 8 * n - 1) / 4; }

}  // namespace iqk::plan
