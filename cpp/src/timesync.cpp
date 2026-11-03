#include "iqk/timesync.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "iqk/config.hpp"

namespace iqk::timesync {

namespace cfg = iqk::cfg::timing;

std::int64_t Clock::utc(std::int64_t local) const noexcept {
  const std::int64_t d = local - base_local_;
  return base_utc_ + d + static_cast<std::int64_t>(std::llround(static_cast<double>(d) * rate_));
}

void Clock::set(std::int64_t local, std::int64_t utc) noexcept {
  base_local_ = local;
  base_utc_ = utc;
  valid_ = true;
}

void Clock::set_rate(std::int64_t local, double rate) noexcept {
  base_utc_ = utc(local);
  base_local_ = local;
  rate_ = rate;
}

TimeSync::TimeSync() noexcept = default;

int TimeSync::priority(Source s) const noexcept {
  int i = 0;
  for (const auto name : cfg::sources) {
    if ((name == "ptp" && s == Source::ptp) || (name == "gnss" && s == Source::gnss)) {
      return i;
    }
    ++i;
  }
  return 100;
}

bool TimeSync::healthy(Source s, std::int64_t local_ns) const noexcept {
  const auto& h = health_[static_cast<std::size_t>(s)];
  return h.last_good_ns != INT64_MIN && local_ns - h.last_good_ns <= cfg::holdover_start_ms * 1'000'000;
}

void TimeSync::emit(Event::Kind k, std::int64_t local_ns, std::int64_t v) noexcept {
  if (n_events_ == events_.size()) {
    std::rotate(events_.begin(), events_.begin() + 1, events_.end());
    --n_events_;
  }
  events_[n_events_++] = Event{k, local_ns, v};
}

void TimeSync::select(std::int64_t local_ns) noexcept {
  Source best = Source::none;
  for (const Source s : {Source::ptp, Source::gnss}) {
    if (!healthy(s, local_ns)) {
      continue;
    }
    if (best == Source::none || priority(s) < priority(best)) {
      best = s;
    }
  }
  if (best == active_) {
    return;
  }
  const bool active_ok = active_ != Source::none && healthy(active_, local_ns);
  if (active_ok && best != Source::none) {
    // A better source must prove itself for switch_hold_ms before we leave a working one.
    const auto& h = health_[static_cast<std::size_t>(best)];
    if (local_ns - h.good_since_ns < cfg::switch_hold_ms * 1'000'000) {
      return;
    }
  }
  if (best == Source::none) {
    return;  // keep the last source as the reference; tick() moves the state to holdover
  }
  active_ = best;
  in_threshold_ = 0;
  if (state_ == State::locked) {
    state_ = State::locking;
  }
  emit(Event::Kind::switched_source, local_ns, static_cast<std::int64_t>(best));
}

void TimeSync::measurement(Source s, std::int64_t local_ns, std::int64_t master_ns) noexcept {
  auto& h = health_[static_cast<std::size_t>(s)];
  if (h.last_good_ns == INT64_MIN || local_ns - h.last_good_ns > cfg::holdover_start_ms * 1'000'000) {
    h.good_since_ns = local_ns;
  }
  h.last_good_ns = local_ns;
  select(local_ns);
  if (s != active_) {
    return;
  }
  last_any_good_ = local_ns;
  holdover_since_ = INT64_MIN;
  if (!clock_.valid()) {
    clock_.set(local_ns, master_ns);
    state_ = State::locking;
    in_threshold_ = 0;
    last_meas_local_ = local_ns;
    emit(Event::Kind::stepped, local_ns, 0);
    return;
  }
  const std::int64_t offset = master_ns - clock_.utc(local_ns);
  last_offset_ = offset;
  if (state_ == State::holdover || state_ == State::free_running) {
    state_ = State::locking;
    in_threshold_ = 0;
  }
  if (std::llabs(offset) > kStepThresholdNs) {
    clock_.set(local_ns, master_ns);
    clock_.set_rate(local_ns, drift_);
    if (state_ == State::locked) {
      emit(Event::Kind::lost_lock, local_ns, offset);
    }
    state_ = State::locking;
    in_threshold_ = 0;
    last_meas_local_ = local_ns;
    emit(Event::Kind::stepped, local_ns, offset);
    return;
  }
  const std::int64_t dt = (last_meas_local_ == INT64_MIN || local_ns <= last_meas_local_)
                              ? kNsPerS
                              : std::max<std::int64_t>(local_ns - last_meas_local_, 10'000'000);
  const bool ptp = s == Source::ptp;
  const double kp = ptp ? cfg::ptp_kp : cfg::gnss_kp;
  const double ki = ptp ? cfg::ptp_ki : cfg::gnss_ki;
  const std::int64_t lock_threshold = ptp ? cfg::ptp_lock_threshold_ns : cfg::gnss_lock_threshold_ns;
  const std::int64_t unlock_threshold = ptp ? cfg::ptp_unlock_threshold_ns : cfg::gnss_unlock_threshold_ns;
  const double limit = static_cast<double>(cfg::max_freq_adj_ppb) * 1e-9;
  drift_ = std::clamp(drift_ + ki * static_cast<double>(offset) / static_cast<double>(dt), -limit, limit);
  const double rate = std::clamp(drift_ + kp * static_cast<double>(offset) / static_cast<double>(dt), -limit, limit);
  clock_.set_rate(local_ns, rate);
  last_meas_local_ = local_ns;

  const std::int64_t a = std::llabs(offset);
  if (a < lock_threshold) {
    ++in_threshold_;
  } else {
    in_threshold_ = 0;
  }
  if (state_ == State::locked && a > unlock_threshold) {
    state_ = State::locking;
    emit(Event::Kind::lost_lock, local_ns, offset);
  }
  if (state_ != State::locked && in_threshold_ >= cfg::lock_count) {
    state_ = State::locked;
    emit(Event::Kind::locked, local_ns, offset);
  }
}

void TimeSync::tick(std::int64_t local_ns) noexcept {
  select(local_ns);
  const bool live = active_ != Source::none && healthy(active_, local_ns);
  if (!live && (state_ == State::locked || state_ == State::locking) && clock_.valid()) {
    state_ = State::holdover;
    holdover_since_ = local_ns;
    in_threshold_ = 0;
    emit(Event::Kind::holdover, local_ns, 0);
  }
  if (state_ == State::holdover && local_ns - holdover_since_ > cfg::holdover_max_ms * 1'000'000) {
    state_ = State::free_running;
    emit(Event::Kind::free_running, local_ns, 0);
  }
}

std::uint8_t TimeSync::quality() const noexcept {
  switch (state_) {
    case State::locked:
      return active_ == Source::ptp ? 3U : 2U;
    case State::holdover:
      return 1U;
    default:
      return 0U;
  }
}

void TimeSync::on_pps(std::int64_t local_ns) noexcept { pps_edge_ = local_ns; }

void TimeSync::on_gnss_time(const icd::gnss::TimePulse& m, std::int64_t local_ns) noexcept {
  std::int64_t reason = 0;
  if (!icd::gnss::in_range(m)) {
    reason = 1;
  } else if (!m.time_valid() || !m.leap_valid() || m.fix < 2U) {
    reason = 2;
  } else if (m.acc_ns > static_cast<std::uint32_t>(cfg::gnss_max_acc_ns)) {
    reason = 3;
  } else if (!pps_edge_.has_value()) {
    reason = 4;
  } else if (local_ns < *pps_edge_ || local_ns - *pps_edge_ > cfg::gnss_max_msg_delay_ms * 1'000'000) {
    reason = 5;  // the message does not belong to the last edge
  }
  if (reason != 0 || !pps_edge_.has_value()) {
    emit(Event::Kind::gnss_rejected, local_ns, reason);
    return;
  }
  const std::int64_t edge = pps_edge_.value();
  pps_edge_.reset();  // each edge is used once
  measurement(Source::gnss, edge, gps_to_utc_ns(m.week, m.tow_ms, m.leap_s));
}

void TimeSync::on_ptp_exchange(std::int64_t t1, std::int64_t t2, std::int64_t t3, std::int64_t t4) noexcept {
  const std::int64_t delay = ((t2 - t1) + (t4 - t3)) / 2;
  std::int64_t min_delay = INT64_MAX;
  for (std::size_t i = 0; i < n_delays_; ++i) {
    min_delay = std::min(min_delay, delays_[i]);
  }
  const std::size_t window = std::min<std::size_t>(static_cast<std::size_t>(cfg::ptp_min_window), delays_.size());
  delays_[delay_pos_] = delay;
  delay_pos_ = (delay_pos_ + 1) % window;
  n_delays_ = std::min(n_delays_ + 1, window);
  const std::int64_t floor = std::max<std::int64_t>(min_delay == INT64_MAX ? 0 : min_delay, 1000);
  if (delay < 0 || (min_delay != INT64_MAX && delay > cfg::ptp_outlier_factor * floor)) {
    emit(Event::Kind::ptp_outlier, t2, delay);
    return;
  }
  measurement(Source::ptp, t2, t1 + delay);
}

namespace {
constexpr std::uint8_t kSync = 0x0;
constexpr std::uint8_t kDelayReq = 0x1;
constexpr std::uint8_t kFollowUp = 0x8;
constexpr std::uint8_t kDelayResp = 0x9;
constexpr std::int64_t ts(std::uint64_t s, std::uint32_t ns) { return static_cast<std::int64_t>(s) * kNsPerS + ns; }
}  // namespace

std::size_t PtpSlave::on_datagram(std::span<const std::uint8_t> d, std::int64_t rx_local, TimeSync& ts_,
                                  std::span<std::uint8_t> out) noexcept {
  if (d.size() < 34 || (d[1] & 0x0FU) != 2U || d[4] != domain_) {
    ++rejected_;
    return 0;
  }
  const std::uint8_t type = d[0] & 0x0FU;
  if (type == kSync) {
    icd::ptp::PtpSync m{};
    if (!icd::ptp::decode(d, m) || !icd::ptp::in_range(m)) {
      ++rejected_;
      return 0;
    }
    sync_seq_ = m.sequence_id;
    have_sync_ = true;
    have_t1_ = false;
    t2_ = rx_local;
    return 0;
  }
  if (type == kFollowUp) {
    icd::ptp::PtpFollowUp m{};
    if (!icd::ptp::decode(d, m) || !icd::ptp::in_range(m) || !have_sync_ || m.sequence_id != sync_seq_) {
      ++rejected_;
      return 0;
    }
    t1_ = ts(m.origin_s, m.origin_ns);
    have_t1_ = true;
    have_sync_ = false;
    icd::ptp::PtpDelayReq r{};
    r.type_sdo = kDelayReq;
    r.version = 2;
    r.length = static_cast<std::uint16_t>(icd::ptp::PtpDelayReq::kSize);
    r.domain = domain_;
    r.clock_identity = id_;
    r.port_number = 1;
    r.sequence_id = ++req_seq_;
    r.control = 1;
    r.log_interval = 0x7F;
    req_pending_ = true;
    t3_ = INT64_MIN;
    return icd::ptp::encode(r, out);
  }
  if (type == kDelayResp) {
    icd::ptp::PtpDelayResp m{};
    if (!icd::ptp::decode(d, m) || !icd::ptp::in_range(m) || !req_pending_ || m.sequence_id != req_seq_ ||
        m.req_clock_identity != id_ || t3_ == INT64_MIN || !have_t1_) {
      ++rejected_;
      return 0;
    }
    req_pending_ = false;
    ts_.on_ptp_exchange(t1_, t2_, t3_, ts(m.receive_s, m.receive_ns));
    return 0;
  }
  ++rejected_;
  return 0;
}

}  // namespace iqk::timesync
