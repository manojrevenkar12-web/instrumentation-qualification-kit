// Test support: requirement tags and builders for device traffic.
#pragma once

#include <gtest/gtest.h>

#include <initializer_list>
#include <span>
#include <string>
#include <vector>

#include "iqk/can.hpp"
#include "iqk/framing.hpp"
#include "iqk/icd/aam.hpp"
#include "iqk/icd/imu.hpp"

// Declares the requirements a test verifies. Recorded in the JUnit report as the property "verifies"; the
// qualification pipeline accepts the claim only if the test fails on that requirement's mutants.
#define IQK_VERIFIES(...) ::iqk_test::verifies({__VA_ARGS__})

namespace iqk_test {

inline void verifies(std::initializer_list<const char*> reqs) {
  std::string s;
  for (const char* r : reqs) {
    s += (s.empty() ? "" : ",") + std::string{r};
  }
  ::testing::Test::RecordProperty("verifies", s);
}

inline std::vector<std::uint8_t> imu_frame(const iqk::icd::imu::ImuSample& s) {
  std::array<std::uint8_t, iqk::icd::imu::ImuSample::kSize> p{};
  (void)iqk::icd::imu::encode(s, p);
  std::vector<std::uint8_t> out(p.size() + iqk::kFrameOverhead);
  out.resize(iqk::encode_frame(static_cast<std::uint8_t>(iqk::icd::imu::ImuSample::kId), p, out));
  return out;
}

inline iqk::icd::imu::ImuSample imu_sample(std::uint16_t seq, std::uint32_t t_us) {
  iqk::icd::imu::ImuSample s{};
  s.seq = seq;
  s.t_us = t_us;
  s.gyro_z = 250000;      // 0.25 rad/s
  s.accel_z = -981000;    // -9.81 m/s2
  s.temp = 3150;          // 31.5 degC
  s.status = 0x07;
  return s;
}

inline iqk::CanFrame can(std::uint32_t id, std::span<const std::uint8_t> payload) {
  iqk::CanFrame f{};
  f.id = id;
  f.dlc = static_cast<std::uint8_t>(payload.size());
  for (std::size_t i = 0; i < payload.size(); ++i) {
    f.data[i] = payload[i];
  }
  return f;
}

inline iqk::CanFrame heartbeat(int node, std::uint8_t state, std::uint16_t errors = 0) {
  iqk::icd::aam::AamHeartbeat h{};
  h.state = state;
  h.errors = errors;
  std::array<std::uint8_t, iqk::icd::aam::AamHeartbeat::kSize> p{};
  (void)iqk::icd::aam::encode(h, p);
  return can(iqk::icd::aam::AamHeartbeat::can_id(static_cast<std::uint8_t>(node)), p);
}

inline iqk::CanFrame data_frame(int node, std::uint8_t mux, std::uint8_t counter, std::int16_t a, std::int16_t b,
                                std::int16_t c) {
  iqk::icd::aam::AamData d{};
  d.mux = mux;
  d.counter = counter;
  d.ch_a = a;
  d.ch_b = b;
  d.ch_c = c;
  std::array<std::uint8_t, iqk::icd::aam::AamData::kSize> p{};
  (void)iqk::icd::aam::encode(d, p);
  return can(iqk::icd::aam::AamData::can_id(static_cast<std::uint8_t>(node)), p);
}

inline constexpr std::int64_t kMs = 1'000'000;
inline constexpr std::int64_t kS = 1'000'000'000;

}  // namespace iqk_test
