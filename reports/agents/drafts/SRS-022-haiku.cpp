#include "support.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "iqk/drivers.hpp"
#include "iqk/faultlog.hpp"

// SRS-022: the 32-bit microsecond device time is unwrapped (ImuDriver::acquisition_time), so the local time
// stamps of consecutive samples stay continuous across the 2^32 us wrap.
//
// Stimulus: IMU-SER sample frames carrying only the low 32 bits of the device time, with a constant transport
// latency. The acquisition time on the local clock is then the unwrapped device time plus that latency, so the
// expected stamps are exact and any wrap error shows up as a jump of about 2^32 us.

namespace {

constexpr std::int64_t kUsNs = 1'000;
constexpr std::int64_t kLatencyNs = 150'000;  // arrival = acquisition + latency
constexpr std::int64_t kPeriodNs = 2'500'000;  // 400 Hz

struct Stream {
  std::vector<std::uint32_t> t_us;  // what the device sends: low 32 bits only
  std::vector<std::int64_t> rx_ns;  // arrival time on the local clock
  std::vector<std::int64_t> acq_ns;  // expected acquisition time on the local clock
};

// unwrapped_us: device time without the 2^32 wrap, and starting below 2^32. jitter_ns: extra arrival delay per
// sample (non-negative; the first sample has none, so the latency minimum is kLatencyNs).
Stream make_stream(const std::vector<std::uint64_t>& unwrapped_us, const std::vector<std::int64_t>& jitter_ns = {}) {
  Stream s;
  for (std::size_t i = 0; i < unwrapped_us.size(); ++i) {
    const auto a = static_cast<std::int64_t>(unwrapped_us[i]);
    const std::int64_t jit = jitter_ns.empty() ? 0 : jitter_ns[i];
    s.t_us.push_back(static_cast<std::uint32_t>(unwrapped_us[i]));
    s.acq_ns.push_back(a * kUsNs + kLatencyNs);
    s.rx_ns.push_back(a * kUsNs + kLatencyNs + jit);
  }
  return s;
}

// Feeds one IMU frame per sample to a fresh driver and returns the local stamp of each delivered reading.
std::vector<std::int64_t> run(const Stream& s) {
  iqk::ImuDriver drv;
  iqk::FaultLog log;
  std::vector<std::int64_t> out;
  for (std::size_t i = 0; i < s.t_us.size(); ++i) {
    const auto frame = iqk_test::imu_frame(iqk_test::imu_sample(static_cast<std::uint16_t>(i), s.t_us[i]));
    drv.on_bytes(frame, s.rx_ns[i], log, [&](const iqk::ImuReading& r) { out.push_back(r.local_ns); });
  }
  return out;
}

// Number of times the 32-bit device time went backwards, i.e. wrapped.
std::size_t wraps(const std::vector<std::uint32_t>& t) {
  std::size_t n = 0;
  for (std::size_t i = 1; i < t.size(); ++i) {
    if (t[i] < t[i - 1]) {
      ++n;
    }
  }
  return n;
}

// Checks the spacing of consecutive stamps: got[i + 1] - got[i] == deltas[i].
void expect_deltas(const std::vector<std::int64_t>& got, const std::vector<std::int64_t>& deltas) {
  ASSERT_EQ(got.size(), deltas.size() + 1);
  for (std::size_t i = 0; i < deltas.size(); ++i) {
    EXPECT_EQ(got[i + 1] - got[i], deltas[i]) << "between samples " << i << " and " << i + 1;
  }
}

}  // namespace

TEST(DeviceTimeWrap, LocalTimeStaysContinuousAcrossWrap) {
  IQK_VERIFIES("SRS-022");
  // Starts 4096 us below the wrap at 2^32 and runs at 400 Hz across it.
  std::vector<std::uint64_t> a;
  for (std::size_t k = 0; k < 10; ++k) {
    a.push_back(0xFFFFF000ULL + 2500ULL * k);
  }
  const Stream s = make_stream(a);
  ASSERT_GE(wraps(s.t_us), std::size_t{1});
  const auto got = run(s);
  ASSERT_EQ(got.size(), s.t_us.size());
  EXPECT_EQ(got, s.acq_ns);
  expect_deltas(got, std::vector<std::int64_t>(a.size() - 1, kPeriodNs));
}

TEST(DeviceTimeWrap, AdjacentSamplesAcrossWrapAreOneMicrosecondApart) {
  IQK_VERIFIES("SRS-022");
  // The last sample before the wrap, the all-ones value, the zero after it, and the one after that.
  const Stream s = make_stream({0xFFFFFFFEULL, 0xFFFFFFFFULL, 0x100000000ULL, 0x100000001ULL});
  ASSERT_EQ(s.t_us[1], 0xFFFFFFFFU);
  ASSERT_EQ(s.t_us[2], 0U);
  ASSERT_EQ(s.t_us[3], 1U);
  const auto got = run(s);
  ASSERT_EQ(got.size(), s.t_us.size());
  EXPECT_EQ(got, s.acq_ns);
  expect_deltas(got, {kUsNs, kUsNs, kUsNs});
}

TEST(DeviceTimeWrap, EveryWrapIsCountedNotOnlyTheFirst) {
  IQK_VERIFIES("SRS-022");
  // Steps of 3000 s (more than half the 2^32 us range), so the stream wraps about every second sample and goes
  // around more than four times in total.
  constexpr std::uint64_t kStepUs = 3'000'000'000ULL;
  std::vector<std::uint64_t> a{1000ULL};
  for (std::size_t k = 0; k < 6; ++k) {
    a.push_back(a.back() + kStepUs);
  }
  const Stream s = make_stream(a);
  ASSERT_GE(wraps(s.t_us), std::size_t{3});
  const auto got = run(s);
  ASSERT_EQ(got.size(), s.t_us.size());
  EXPECT_EQ(got, s.acq_ns);
  expect_deltas(got, std::vector<std::int64_t>(a.size() - 1, static_cast<std::int64_t>(kStepUs) * kUsNs));
}

TEST(DeviceTimeWrap, StepsAroundHalfRangeAreForward) {
  IQK_VERIFIES("SRS-022");
  // Forward steps just below and just above 2^31 us. A wrap is taken from unsigned arithmetic, so a step above
  // 2^31 us must not be read as a step backwards.
  std::vector<std::uint64_t> a{0x10000000ULL};
  std::vector<std::int64_t> deltas;
  for (std::size_t k = 0; k < 6; ++k) {
    const std::uint64_t step = (k % 2 == 0) ? 0x7FFFFFFFULL : 0x80000001ULL;
    a.push_back(a.back() + step);
    deltas.push_back(static_cast<std::int64_t>(step) * kUsNs);
  }
  const Stream s = make_stream(a);
  ASSERT_GE(wraps(s.t_us), std::size_t{2});
  const auto got = run(s);
  ASSERT_EQ(got.size(), s.t_us.size());
  EXPECT_EQ(got, s.acq_ns);
  expect_deltas(got, deltas);
}

TEST(DeviceTimeWrap, ArrivalJitterDoesNotBreakContinuityAcrossWrap) {
  IQK_VERIFIES("SRS-022");
  // Arrival jitter of 0..49 us, never negative. The latency minimum is still the first sample's, so the wrap must
  // not shift the stamps.
  std::vector<std::uint64_t> a;
  std::vector<std::int64_t> jitter;
  for (std::size_t k = 0; k < 10; ++k) {
    a.push_back(0xFFFFF000ULL + 2500ULL * k);
    jitter.push_back(static_cast<std::int64_t>((k * 7919U) % 50U) * kUsNs);
  }
  const Stream s = make_stream(a, jitter);
  ASSERT_GE(wraps(s.t_us), std::size_t{1});
  const auto got = run(s);
  ASSERT_EQ(got.size(), s.t_us.size());
  EXPECT_EQ(got, s.acq_ns);
  expect_deltas(got, std::vector<std::int64_t>(a.size() - 1, kPeriodNs));
}
