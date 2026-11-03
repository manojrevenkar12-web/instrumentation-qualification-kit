#include "iqk/plan.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <tuple>

#include "iqk/config.hpp"
#include "iqk/crc.hpp"
#include "iqk/icd/aam.hpp"
#include "iqk/icd/imu.hpp"
#include "iqk/icd/tlm.hpp"

namespace iqk::plan {

namespace {

namespace aam = iqk::icd::aam;
namespace pcfg = iqk::cfg::plan;

struct ImuField {
  std::string_view name;
  double lo;
  double hi;
};
// Engineering ranges from IMU-SER imu_sample (icd/imu.yaml).
constexpr ImuField kImuFields[] = {
    {"gyro_x", -8.727, 8.727},  {"gyro_y", -8.727, 8.727},  {"gyro_z", -8.727, 8.727},
    {"accel_x", -156.9, 156.9}, {"accel_y", -156.9, 156.9}, {"accel_z", -156.9, 156.9},
    {"temp", -40.0, 85.0},
};

std::vector<std::string_view> split(std::string_view s) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
      ++i;
    }
    std::size_t j = i;
    while (j < s.size() && s[j] != ' ' && s[j] != '\t') {
      ++j;
    }
    if (j > i) {
      out.push_back(s.substr(i, j - i));
    }
    i = j;
  }
  return out;
}

bool to_double(std::string_view s, double& v) {
  if (s.empty()) {
    return false;
  }
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  return r.ec == std::errc{} && r.ptr == s.data() + s.size() && std::isfinite(v);
}

bool to_int(std::string_view s, int& v) {
  if (s.empty()) {
    return false;
  }
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}

bool valid_name(std::string_view n) {
  if (n.empty() || n.size() > 16) {
    return false;
  }
  return std::all_of(n.begin(), n.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

const ImuField* imu_field(std::string_view n) {
  for (const auto& f : kImuFields) {
    if (f.name == n) {
      return &f;
    }
  }
  return nullptr;
}

int range_code(int mv) {
  for (std::size_t i = 0; i < aam::kRangesMv.size(); ++i) {
    if (aam::kRangesMv[i] == mv) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int filter_code(std::string_view f) {
  for (std::size_t i = 0; i < aam::kFilters.size(); ++i) {
    if (aam::kFilters[i] == f) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

int divisor_for(double rate) {
  for (const auto d : aam::kDivisors) {
    if (std::fabs(static_cast<double>(aam::kBaseRateHz) / static_cast<double>(d) - rate) < 1e-9) {
      return static_cast<int>(d);
    }
  }
  return -1;
}

std::pair<double, double> measurable(const Channel& c) {
  if (c.kind == Kind::imu) {
    const auto* f = imu_field(c.field);
    return f != nullptr ? std::pair{f->lo, f->hi} : std::pair{0.0, 0.0};
  }
  const double v = static_cast<double>(c.range_mv) / 1000.0;
  const double a = c.offset - c.scale * v;
  const double b = c.offset + c.scale * v;
  return {std::min(a, b), std::max(a, b)};
}

void add(std::vector<Finding>& f, const char* rule, int line, std::string detail) {
  f.push_back(Finding{rule, line, std::move(detail)});
}

std::string fmt(double v) {
  char b[32];
  (void)std::snprintf(b, sizeof b, "%.9g", v);
  return b;
}

}  // namespace

std::optional<Plan> parse(std::string_view text, std::vector<Finding>& findings) {
  Plan p;
  int line_no = 0;
  bool header = false;
  bool ok = true;
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const std::size_t nl = text.find('\n', pos);
    std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
    pos = nl == std::string_view::npos ? text.size() + 1 : nl + 1;
    ++line_no;
    if (const auto h = line.find('#'); h != std::string_view::npos) {
      line = line.substr(0, h);
    }
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    const auto t = split(line);
    if (t.empty()) {
      continue;
    }
    if (!header) {
      if (t.size() != 2 || t[0] != "iqk-plan" || t[1] != "1") {
        add(findings, "PLN-01", line_no, "the first statement must be 'iqk-plan 1'");
        return std::nullopt;
      }
      header = true;
      continue;
    }
    if (t[0] == "campaign") {
      if (t.size() != 2 || !p.campaign.empty()) {
        add(findings, "PLN-02", line_no, "exactly one 'campaign <id>' is allowed");
        ok = false;
        continue;
      }
      p.campaign = std::string{t[1]};
      continue;
    }
    if (t[0] != "channel" || t.size() < 3) {
      add(findings, "PLN-02", line_no, "unknown statement '" + std::string{t[0]} + "'");
      ok = false;
      continue;
    }
    Channel c;
    c.line = line_no;
    c.name = std::string{t[1]};
    const std::string_view src = t[2];
    if (src.starts_with("aam:")) {
      c.kind = Kind::aam;
      const auto rest = src.substr(4);
      const auto colon = rest.find(':');
      if (colon == std::string_view::npos || !to_int(rest.substr(0, colon), c.node) ||
          !to_int(rest.substr(colon + 1), c.input)) {
        add(findings, "PLN-02", line_no, "source must be aam:<node>:<input>");
        ok = false;
        continue;
      }
      c.unit = "V";
    } else if (src.starts_with("imu:")) {
      c.kind = Kind::imu;
      c.field = std::string{src.substr(4)};
      c.unit = "";
    } else {
      add(findings, "PLN-02", line_no, "source must be aam:<node>:<input> or imu:<field>");
      ok = false;
      continue;
    }
    std::set<std::string_view> seen;
    bool line_ok = true;
    for (std::size_t i = 3; i < t.size(); ++i) {
      const auto eq = t[i].find('=');
      if (eq == std::string_view::npos) {
        add(findings, "PLN-02", line_no, "expected key=value, got '" + std::string{t[i]} + "'");
        line_ok = false;
        continue;
      }
      const auto k = t[i].substr(0, eq);
      const auto v = t[i].substr(eq + 1);
      if (!seen.insert(k).second) {
        add(findings, "PLN-02", line_no, "duplicate key '" + std::string{k} + "'");
        line_ok = false;
        continue;
      }
      bool good = true;
      if (k == "rate") {
        good = to_double(v, c.rate_hz);
      } else if (k == "range_mv" && c.kind == Kind::aam) {
        good = to_int(v, c.range_mv);
      } else if (k == "filter" && c.kind == Kind::aam) {
        c.filter = std::string{v};
      } else if (k == "scale" && c.kind == Kind::aam) {
        good = to_double(v, c.scale);
      } else if (k == "offset" && c.kind == Kind::aam) {
        good = to_double(v, c.offset);
      } else if (k == "unit" && c.kind == Kind::aam) {
        good = !v.empty() && v.size() <= 8;
        c.unit = std::string{v};
      } else if (k == "alarm") {
        const auto comma = v.find(',');
        double lo = 0;
        double hi = 0;
        good = comma != std::string_view::npos && to_double(v.substr(0, comma), lo) &&
               to_double(v.substr(comma + 1), hi);
        if (good) {
          c.alarm = std::pair{lo, hi};
        }
      } else {
        add(findings, "PLN-02", line_no, "unknown key '" + std::string{k} + "' for this source");
        line_ok = false;
        continue;
      }
      if (!good) {
        add(findings, "PLN-02", line_no, "bad value for '" + std::string{k} + "'");
        line_ok = false;
      }
    }
    if (!seen.contains("rate") || (c.kind == Kind::aam && !seen.contains("range_mv"))) {
      add(findings, "PLN-02", line_no, c.kind == Kind::aam ? "rate and range_mv are required" : "rate is required");
      line_ok = false;
    }
    if (!line_ok) {
      ok = false;
      continue;
    }
    p.channels.push_back(std::move(c));
  }
  if (!header) {
    add(findings, "PLN-01", 0, "empty plan");
    return std::nullopt;
  }
  if (p.campaign.empty()) {
    add(findings, "PLN-02", 0, "missing 'campaign <id>'");
    ok = false;
  }
  return ok ? std::optional<Plan>{std::move(p)} : std::nullopt;
}

double can_bus_load(const Plan& p) {
  // Per module: the heartbeat, plus each group's data frame at the group's rate (the fastest of its inputs).
  std::map<int, std::map<int, double>> groups;
  for (const auto& c : p.channels) {
    if (c.kind == Kind::aam) {
      auto& r = groups[c.node][c.input / 3];
      r = std::max(r, c.rate_hz);
    }
  }
  double bits = 0;
  for (const auto& [node, gs] : groups) {
    bits += aam::AamHeartbeat::kRateHz * can_frame_bits(static_cast<int>(aam::AamHeartbeat::kSize));
    for (const auto& [g, rate] : gs) {
      bits += rate * can_frame_bits(static_cast<int>(aam::AamData::kSize));
    }
  }
  return bits / static_cast<double>(aam::kBitrate);
}

double telemetry_bps(const Plan& p) {
  double samples_per_s = 0;
  for (const auto& c : p.channels) {
    samples_per_s += c.rate_hz;
  }
  namespace tlm = iqk::icd::tlm;
  const double frames_per_s = 1000.0 / static_cast<double>(pcfg::frame_period_ms);
  const double per_frame = samples_per_s / frames_per_s;
  const double packets_per_frame = std::max(1.0, std::ceil(per_frame / static_cast<double>(tlm::TlmPacket::kMaxRecords)));
  constexpr double kUdpIpEth = 8 + 20 + 38;  // UDP, IPv4, Ethernet framing incl. preamble and gap
  const double packet_overhead = static_cast<double>(tlm::TlmPacket::kFixedSize) + 4.0 + kUdpIpEth;
  return 8.0 * (samples_per_s * static_cast<double>(tlm::TlmPacket::kRecordSize) +
                frames_per_s * packets_per_frame * packet_overhead);
}

bool validate(const Plan& p, std::vector<Finding>& f) {
  const std::size_t before = f.size();
  if (p.channels.empty() || p.channels.size() > static_cast<std::size_t>(pcfg::max_channels)) {
    add(f, "PLN-04", 0, "a plan has 1.." + std::to_string(pcfg::max_channels) + " channels, this one has " +
                            std::to_string(p.channels.size()));
  }
  std::set<std::string> names;
  std::set<std::tuple<int, int, std::string>> inputs;
  std::map<std::pair<int, int>, double> group_rate;
  for (const auto& c : p.channels) {
    if (!valid_name(c.name)) {
      add(f, "PLN-03", c.line, "channel names are 1..16 of a-z, 0-9, _: '" + c.name + "'");
    }
    if (!names.insert(c.name).second) {
      add(f, "PLN-03", c.line, "duplicate channel name '" + c.name + "'");
    }
    if (c.kind == Kind::aam) {
      if (c.node < 1 || c.node > 32 || c.input < 0 || c.input >= aam::kChannels) {
        add(f, "PLN-05", c.line, "no such input aam:" + std::to_string(c.node) + ":" + std::to_string(c.input));
        continue;
      }
      if (divisor_for(c.rate_hz) < 0) {
        add(f, "PLN-06", c.line, "the module cannot sample at " + fmt(c.rate_hz) + " Hz (1000 Hz / a divisor)");
      }
      if (range_code(c.range_mv) < 0) {
        add(f, "PLN-07", c.line, "no range of +/-" + std::to_string(c.range_mv) + " mV");
      }
      if (filter_code(c.filter) < 0) {
        add(f, "PLN-07", c.line, "no filter '" + c.filter + "'");
      }
      if (c.scale == 0.0) {
        add(f, "PLN-13", c.line, "scale must not be 0");
      }
      const auto key = std::pair{c.node, c.input / 3};
      if (auto it = group_rate.find(key); it != group_rate.end() && it->second != c.rate_hz) {
        add(f, "PLN-08", c.line, "inputs " + std::to_string(key.second * 3) + ".." + std::to_string(key.second * 3 + 2) +
                                     " of module " + std::to_string(c.node) + " share one data frame and must share a rate");
      } else {
        group_rate[key] = c.rate_hz;
      }
      if (!inputs.insert({c.node, c.input, ""}).second) {
        add(f, "PLN-09", c.line, "input aam:" + std::to_string(c.node) + ":" + std::to_string(c.input) + " is used twice");
      }
    } else {
      if (imu_field(c.field) == nullptr) {
        add(f, "PLN-05", c.line, "no IMU field '" + c.field + "'");
        continue;
      }
      if (std::find(pcfg::allowed_imu_rates_hz.begin(), pcfg::allowed_imu_rates_hz.end(),
                    static_cast<std::int64_t>(c.rate_hz)) == pcfg::allowed_imu_rates_hz.end() ||
          c.rate_hz != std::floor(c.rate_hz)) {
        add(f, "PLN-06", c.line, "IMU channels run at 400, 200, 100 or 50 Hz, not " + fmt(c.rate_hz));
      }
      if (!inputs.insert({0, 0, c.field}).second) {
        add(f, "PLN-09", c.line, "imu:" + c.field + " is used twice");
      }
    }
    if (c.alarm) {
      const auto [lo, hi] = *c.alarm;
      const auto [mlo, mhi] = measurable(c);
      if (!(lo < hi)) {
        add(f, "PLN-10", c.line, "alarm low must be below alarm high");
      } else if (lo < mlo || hi > mhi) {
        add(f, "PLN-10", c.line, "alarm " + fmt(lo) + ".." + fmt(hi) + " is outside what the channel can measure (" +
                                     fmt(mlo) + ".." + fmt(mhi) + ")");
      }
    }
  }
  if (const double load = can_bus_load(p); load > pcfg::max_can_bus_load) {
    add(f, "PLN-11", 0, "worst-case CAN bus load " + fmt(std::round(load * 1000) / 10) + "% exceeds " +
                            fmt(pcfg::max_can_bus_load * 100) + "%");
  }
  if (const double bps = telemetry_bps(p); bps > static_cast<double>(pcfg::telemetry_link_bps)) {
    add(f, "PLN-12", 0, "telemetry needs " + fmt(std::round(bps / 1000)) + " kbit/s, the link has " +
                            fmt(static_cast<double>(pcfg::telemetry_link_bps) / 1000) + " kbit/s");
  }
  return f.size() == before;
}

std::string canonical(const Plan& p) {
  std::string out = "iqk-plan 1\ncampaign " + p.campaign + "\n";
  for (const auto& c : p.channels) {
    out += "channel " + c.name + " ";
    if (c.kind == Kind::aam) {
      out += "aam:" + std::to_string(c.node) + ":" + std::to_string(c.input) + " rate=" + fmt(c.rate_hz) +
             " range_mv=" + std::to_string(c.range_mv) + " filter=" + c.filter + " scale=" + fmt(c.scale) +
             " offset=" + fmt(c.offset) + " unit=" + c.unit;
    } else {
      out += "imu:" + c.field + " rate=" + fmt(c.rate_hz);
    }
    if (c.alarm) {
      out += " alarm=" + fmt(c.alarm->first) + "," + fmt(c.alarm->second);
    }
    out += "\n";
  }
  return out;
}

std::uint32_t crc(const Plan& p) {
  const std::string s = canonical(p);
  return crc::crc32(std::span{reinterpret_cast<const std::uint8_t*>(s.data()), s.size()});
}

namespace {
struct InputCfg {
  std::uint32_t enable{0};
  std::uint32_t range{0};
  std::uint32_t divisor{1};
  std::uint32_t filter{0};
};
std::map<int, std::array<InputCfg, aam::kChannels>> module_configs(const Plan& p) {
  std::map<int, std::array<InputCfg, aam::kChannels>> m;
  for (const auto& c : p.channels) {
    if (c.kind != Kind::aam) {
      continue;
    }
    auto& in = m[c.node][static_cast<std::size_t>(c.input)];
    in.enable = 1;
    in.range = static_cast<std::uint32_t>(std::max(range_code(c.range_mv), 0));
    in.divisor = static_cast<std::uint32_t>(std::max(divisor_for(c.rate_hz), 1));
    in.filter = static_cast<std::uint32_t>(std::max(filter_code(c.filter), 0));
  }
  return m;
}
}  // namespace

std::vector<RegWrite> registers(const Plan& p) {
  std::vector<RegWrite> out;
  for (const auto& [node, ins] : module_configs(p)) {
    for (std::size_t i = 0; i < ins.size(); ++i) {
      const auto idx = static_cast<std::uint16_t>(aam::kRegChEnableIndex + i);
      out.push_back({node, idx, aam::kRegChEnableSub, ins[i].enable});
      out.push_back({node, idx, aam::kRegChRangeCodeSub, ins[i].range});
      out.push_back({node, idx, aam::kRegChDivisorSub, ins[i].divisor});
      out.push_back({node, idx, aam::kRegChFilterSub, ins[i].filter});
    }
  }
  return out;
}

std::uint32_t expected_module_crc(const Plan& p, int node) {
  const auto cfgs = module_configs(p);
  const auto it = cfgs.find(node);
  std::array<std::uint8_t, aam::kChannels * 16> buf{};
  std::size_t off = 0;
  for (std::size_t i = 0; i < static_cast<std::size_t>(aam::kChannels); ++i) {
    const InputCfg c = it == cfgs.end() ? InputCfg{} : it->second[i];
    for (const std::uint32_t v : {c.enable, c.range, c.divisor, c.filter}) {
      for (int b = 0; b < 4; ++b) {
        buf[off++] = static_cast<std::uint8_t>((v >> (8 * b)) & 0xFFU);
      }
    }
  }
  return crc::crc32(buf);
}

}  // namespace iqk::plan
