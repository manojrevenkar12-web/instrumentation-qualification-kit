#include "iqk/dau.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "iqk/config.hpp"
#include "iqk/crc.hpp"
#include "iqk/icd/gnss.hpp"

namespace iqk {

namespace tlm = iqk::icd::tlm;

namespace {
constexpr std::string_view kImuFields[] = {"gyro_x", "gyro_y", "gyro_z", "accel_x", "accel_y", "accel_z", "temp"};
constexpr std::uint8_t kImuStart = 1;
constexpr std::uint8_t kImuSelfTest = 3;
constexpr std::uint8_t kNmtStart = 0x01;
constexpr std::int64_t kMs = 1'000'000;

double imu_value(const icd::imu::ImuSample& s, int field) {
  switch (field) {
    case 0: return s.gyro_x_eng();
    case 1: return s.gyro_y_eng();
    case 2: return s.gyro_z_eng();
    case 3: return s.accel_x_eng();
    case 4: return s.accel_y_eng();
    case 5: return s.accel_z_eng();
    default: return s.temp_eng();
  }
}

Fault fault_for(timesync::Event::Kind k) {
  using K = timesync::Event::Kind;
  switch (k) {
    case K::stepped: return Fault::time_stepped;
    case K::lost_lock: return Fault::time_lost_lock;
    case K::holdover: return Fault::time_holdover;
    case K::free_running: return Fault::time_free_running;
    case K::switched_source: return Fault::time_switched;
    case K::gnss_rejected: return Fault::gnss_rejected;
    case K::ptp_outlier: return Fault::ptp_outlier;
    default: return Fault::time_stepped;
  }
}
}  // namespace

std::uint16_t Bit::bits() const noexcept {
  return static_cast<std::uint16_t>((imu ? 1U : 0U) | (aam ? 2U : 0U) | (gnss ? 4U : 0U) | (ptp ? 8U : 0U) |
                                    (plan ? 16U : 0U) | (storage ? 32U : 0U));
}

Dau::Dau(std::uint64_t clock_identity, std::uint8_t ptp_domain) noexcept
    : gnss_def_{static_cast<std::uint8_t>(icd::gnss::TimePulse::kSize)}, ptp_{clock_identity, ptp_domain} {
  frame_.reserve(static_cast<std::size_t>(cfg::plan::max_channels) * 64U);
}

bool Dau::load_plan(std::string_view text, std::int64_t now, std::vector<plan::Finding>& findings) {
  auto p = plan::parse(text, findings);
  if (!p || !plan::validate(*p, findings)) {
    log_.log(Fault::plan_rejected, now, static_cast<std::int64_t>(findings.size()));
    return false;
  }
  armed_ = false;
  nmt_sent_ = false;
  load_failure_logged_ = false;
  frame_.clear();
  frame_start_ = INT64_MIN;
  outs_.clear();
  for (const auto& c : p->channels) {
    Out o{c, -1, 1};
    if (c.kind == plan::Kind::imu) {
      o.imu_field = static_cast<int>(std::find(std::begin(kImuFields), std::end(kImuFields), c.field) - std::begin(kImuFields));
      o.decimate = static_cast<int>(std::lround(icd::imu::ImuSample::kRateHz / c.rate_hz));
    }
    outs_.push_back(o);
  }
  plan_crc_ = plan::crc(*p);
  loader_.start(*p, now);
  plan_ = std::move(p);
  return true;
}

void Dau::time_events() {
  ts_.drain_events([&](const timesync::Event& e) { log_.log(fault_for(e.kind), e.local_ns, e.value); });
}

void Dau::on_input(Port p, std::span<const std::uint8_t> bytes, std::int64_t local_ns, const Tx& tx) {
  if (boot_ns_ == INT64_MIN) {
    boot_ns_ = local_ns;
  }
  switch (p) {
    case Port::imu:
      imu_.on_bytes(bytes, local_ns, log_, [&](const ImuReading& r) { on_imu(r); });
      return;
    case Port::gnss:
      gnss_def_.feed(bytes, [&](const Frame& f) {
        icd::gnss::TimePulse m{};
        if (f.id != icd::gnss::TimePulse::kId || !icd::gnss::decode(f.data(), m)) {
          return;
        }
        ts_.on_gnss_time(m, local_ns);
        if (ts_.source_healthy(timesync::Source::gnss, local_ns)) {
          last_gnss_ok_ = local_ns;
        }
      });
      time_events();
      return;
    case Port::pps:
      ts_.on_pps(local_ns);
      return;
    case Port::can:
      for (std::size_t off = 0; off + kCanUdpSize <= bytes.size(); off += kCanUdpSize) {
        CanFrame f{};
        if (!decode_can_udp(bytes.subspan(off, kCanUdpSize), f)) {
          continue;
        }
        loader_.on_frame(f, local_ns);
        aam_.on_frame(f, local_ns, log_, [&](const AamReading& r) { on_aam(r); });
      }
      return;
    case Port::ptp_event:
    case Port::ptp_general: {
      std::array<std::uint8_t, 64> out{};
      const std::size_t n = ptp_.on_datagram(bytes, local_ns, ts_, out);
      if (n > 0) {
        tx(Port::ptp_event, std::span<const std::uint8_t>{out.data(), n});
      }
      time_events();
      return;
    }
    case Port::diag:
      diag(bytes, local_ns, tx);
      return;
    case Port::tlm:
      return;
  }
}

void Dau::on_imu(const ImuReading& r) {
  if (!armed_) {
    return;
  }
  for (std::size_t i = 0; i < outs_.size(); ++i) {
    const Out& o = outs_[i];
    if (o.imu_field < 0 || (r.s.seq % o.decimate) != 0) {
      continue;
    }
    add_sample(static_cast<std::uint16_t>(i), r.local_ns, imu_value(r.s, o.imu_field), r.s.saturated());
  }
}

void Dau::on_aam(const AamReading& r) {
  if (!armed_) {
    return;
  }
  for (std::size_t i = 0; i < outs_.size(); ++i) {
    const auto& c = outs_[i].ch;
    if (c.kind != plan::Kind::aam || c.node != r.node || c.input / 3 != r.group) {
      continue;
    }
    const std::int16_t raw = r.raw[static_cast<std::size_t>(c.input % 3)];
    const double volts = static_cast<double>(raw) / 32767.0 * static_cast<double>(c.range_mv) / 1000.0;
    const bool over = std::abs(static_cast<int>(raw)) >= cfg::aam::overrange_raw;
    add_sample(static_cast<std::uint16_t>(i), r.local_ns, c.scale * volts + c.offset, over);
  }
}

void Dau::add_sample(std::uint16_t ch, std::int64_t local_ns, double value, bool overrange) {
  std::uint8_t flags = overrange ? 2U : 0U;
  if (const auto& a = outs_[ch].ch.alarm) {
    if (value < a->first) {
      flags |= 4U;
    }
    if (value > a->second) {
      flags |= 8U;
    }
  }
  if (frame_start_ == INT64_MIN) {
    frame_start_ = local_ns;
  }
  if (frame_.size() < frame_.capacity()) {  // never reallocate on the acquisition path
    frame_.push_back(Sample{local_ns, ch, value, flags});
  }
}

void Dau::flush(std::int64_t now, const Tx& tx) {
  (void)now;
  std::stable_sort(frame_.begin(), frame_.end(), [](const Sample& a, const Sample& b) { return a.local_ns < b.local_ns; });
  std::size_t i = 0;
  while (i < frame_.size()) {
    tlm::TlmPacket pk{};
    pk.magic = 0x4B514954U;
    pk.seq = tlm_seq_++;
    const std::int64_t base = frame_[i].local_ns;
    pk.time_ns = static_cast<std::uint64_t>(ts_.utc_ns(base));
    pk.time_quality = ts_.quality();
    pk.plan_crc = plan_crc_;
    std::size_t n = 0;
    while (i < frame_.size() && n < tlm::TlmPacket::kMaxRecords) {
      const Sample& s = frame_[i];
      auto& r = pk.samples[n];
      r.channel = s.channel;
      r.dt_us = static_cast<std::uint16_t>(std::clamp<std::int64_t>((s.local_ns - base) / 1000, 0, 65535));
      const double scaled = std::round(s.value / tlm::TlmPacketSamples::kValueScale);
      const double lim = static_cast<double>(std::numeric_limits<std::int32_t>::max());
      r.value = static_cast<std::int32_t>(std::clamp(scaled, -lim, lim));
      r.flags = static_cast<std::uint8_t>(s.flags | (std::fabs(scaled) > lim ? 2U : 0U));
      ++n;
      ++i;
    }
    pk.samples_count = static_cast<std::uint8_t>(n);
    const std::size_t len = tlm::encode(pk, buf_);
    const std::uint32_t c = crc::crc32(std::span<const std::uint8_t>{buf_.data(), len});
    wire::put(buf_, len, c, 4, wire::Order::little);
    samples_sent_ += n;
    tx(Port::tlm, std::span<const std::uint8_t>{buf_.data(), len + 4});
  }
  frame_.clear();
  frame_start_ = INT64_MIN;
}

void Dau::tick(std::int64_t local_ns, const Tx& tx) {
  if (boot_ns_ == INT64_MIN) {
    boot_ns_ = local_ns;
  }
  ts_.tick(local_ns);
  time_events();
  aam_.supervise(local_ns, log_);

  // The IMU runs its self-test, then streams; resend until samples arrive (it may have been powered later).
  if (!imu_.alive(local_ns) && (last_imu_cmd_ == INT64_MIN || local_ns - last_imu_cmd_ >= 1000 * kMs)) {
    std::array<std::uint8_t, 16> b{};
    for (const std::uint8_t cmd : {kImuSelfTest, kImuStart}) {
      const std::size_t n = ImuDriver::command(cmd, b);
      tx(Port::imu, std::span<const std::uint8_t>{b.data(), n});
    }
    last_imu_cmd_ = local_ns;
    if (imu_started_) {
      log_.log(Fault::imu_lost, local_ns, 0);
    }
    imu_started_ = true;
  }

  if (loader_.status() == PlanLoader::Status::busy) {
    if (const auto f = loader_.poll(local_ns)) {
      std::array<std::uint8_t, kCanUdpSize> b{};
      (void)encode_can_udp(*f, b);
      tx(Port::can, b);
    }
  }
  if (loader_.status() == PlanLoader::Status::failed && !load_failure_logged_) {
    log_.log(Fault::plan_load_failed, local_ns, static_cast<std::int64_t>(loader_.failure()));
    load_failure_logged_ = true;
  } else if (loader_.status() == PlanLoader::Status::done && !armed_ && plan_) {
    armed_ = true;
    for (const int node : loader_.nodes()) {
      aam_.expect(node, local_ns);  // a module that never sends a heartbeat is lost too
    }
  }
  if (armed_ && !nmt_sent_) {
    for (const int node : loader_.nodes()) {
      icd::aam::AamNmt m{};
      m.cmd = kNmtStart;
      m.node = static_cast<std::uint8_t>(node);
      CanFrame f{};
      f.id = icd::aam::AamNmt::kId;
      f.dlc = static_cast<std::uint8_t>(icd::aam::encode(m, f.data));
      std::array<std::uint8_t, kCanUdpSize> b{};
      (void)encode_can_udp(f, b);
      tx(Port::can, b);
    }
    nmt_sent_ = true;
  }
  if (armed_ && frame_start_ != INT64_MIN && local_ns - frame_start_ >= cfg::plan::frame_period_ms * kMs) {
    flush(local_ns, tx);
  }
}

Bit Dau::bit(std::int64_t local_ns) const noexcept {
  Bit b{};
  b.imu = imu_.alive(local_ns) && imu_.bit_passed();
  b.aam = true;
  for (const int n : loader_.nodes()) {
    b.aam = b.aam && aam_.alive(n, local_ns);
  }
  b.gnss = last_gnss_ok_ != INT64_MIN && local_ns - last_gnss_ok_ <= cfg::timing::holdover_start_ms * kMs;
  b.ptp = ts_.source_healthy(timesync::Source::ptp, local_ns);
  b.plan = armed_;
  return b;
}

tlm::DiagStatus Dau::status(std::int64_t local_ns) const noexcept {
  tlm::DiagStatus s{};
  s.uptime_s = boot_ns_ == INT64_MIN ? 0U : static_cast<std::uint32_t>((local_ns - boot_ns_) / 1'000'000'000);
  s.sw_version = static_cast<std::uint32_t>((cfg::unit::sw_version[0] << 16) | (cfg::unit::sw_version[1] << 8) |
                                            cfg::unit::sw_version[2]);
  s.slot = slot_;
  s.bit = bit(local_ns).bits();
  s.time_state = static_cast<std::uint8_t>(ts_.state());
  s.time_source = static_cast<std::uint8_t>(ts_.source());
  s.offset_ns = static_cast<std::int32_t>(std::clamp<std::int64_t>(ts_.last_offset_ns(), INT32_MIN, INT32_MAX));
  s.imu_crc_errors = static_cast<std::uint32_t>(imu_.link().crc_errors);
  s.imu_gaps = static_cast<std::uint32_t>(imu_.gaps());
  s.can_errors = static_cast<std::uint32_t>(aam_.errors() + aam_.gaps());
  s.plan_crc = plan_crc();
  s.faults_logged = static_cast<std::uint16_t>(std::min<std::uint64_t>(log_.total(), 65535U));
  return s;
}

void Dau::diag(std::span<const std::uint8_t> req, std::int64_t now, const Tx& tx) {
  tlm::DiagRequest r{};
  if (!tlm::decode(req, r) || !tlm::in_range(r)) {
    log_.log(Fault::diag_bad_request, now, static_cast<std::int64_t>(req.size()));
    return;
  }
  std::array<std::uint8_t, tlm::DiagFaults::kMaxSize + 1> out{};
  std::size_t n = 0;
  if (r.what == 1U) {
    out[0] = static_cast<std::uint8_t>(tlm::DiagStatus::kId);
    n = tlm::encode(status(now), std::span{out}.subspan(1));
  } else {
    tlm::DiagFaults f{};
    f.total = static_cast<std::uint32_t>(log_.total());
    const std::size_t k = std::min(log_.size(), tlm::DiagFaults::kMaxRecords);
    for (std::size_t i = 0; i < k; ++i) {
      const FaultRecord& rec = log_[log_.size() - k + i];
      auto& o = f.records[i];
      o.local_ms = static_cast<std::uint32_t>(std::max<std::int64_t>(rec.local_ns - boot_ns_, 0) / kMs);
      o.code = static_cast<std::uint16_t>(rec.code);
      o.detail = static_cast<std::int32_t>(std::clamp<std::int64_t>(rec.detail, INT32_MIN, INT32_MAX));
      o.count = static_cast<std::uint16_t>(std::min<std::uint32_t>(rec.count, 65535U));
    }
    f.records_count = static_cast<std::uint8_t>(k);
    out[0] = static_cast<std::uint8_t>(tlm::DiagFaults::kId);
    n = tlm::encode(f, std::span{out}.subspan(1));
  }
  tx(Port::diag, std::span<const std::uint8_t>{out.data(), n + 1});
}

}  // namespace iqk
