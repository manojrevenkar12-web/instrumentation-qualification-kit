// iqkctl: the engineer's and the CI's tool for the unit.
//   iqkctl validate <plan>                        validate a measurement plan exactly as the unit does
//   iqkctl codec decode <icd> <msg> <hex>         decode a payload to key=value (raw values)
//   iqkctl codec encode <icd> <msg>               encode key=value lines from stdin to hex
//   iqkctl replay <recording>                     re-run a daud recording; prints the telemetry digest
//   iqkctl pkg keygen <seed-out> <pub-out>        new Ed25519 key (hex files)
//   iqkctl pkg build <seed> <hw> <ver> <secver> <image> <out>
//   iqkctl pkg verify <pub> <hw> <floor> <pkg>
//   iqkctl update install|boot|confirm|state <store-dir> <pub> [pkg]
#include <openssl/rand.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "iqk/dau.hpp"
#include "iqk/icd/codec_dispatch.hpp"
#include "iqk/io.hpp"
#include "iqk/plan.hpp"
#include "iqk/update.hpp"

namespace {

std::string slurp(const std::string& path) { return iqk::io::read_file(path); }

std::string hex(std::span<const std::uint8_t> b) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string s;
  for (const auto c : b) {
    s += kDigits[c >> 4U];
    s += kDigits[c & 0xFU];
  }
  return s;
}

bool unhex(std::string_view s, std::vector<std::uint8_t>& out) {
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) {
    s.remove_suffix(1);
  }
  if (s.size() % 2 != 0) {
    return false;
  }
  out.clear();
  for (std::size_t i = 0; i < s.size(); i += 2) {
    unsigned v = 0;
    if (std::sscanf(std::string{s.substr(i, 2)}.c_str(), "%2x", &v) != 1) {
      return false;
    }
    out.push_back(static_cast<std::uint8_t>(v));
  }
  return true;
}

bool key32(const std::string& path, std::array<std::uint8_t, 32>& k) {
  std::vector<std::uint8_t> b;
  if (!unhex(slurp(path), b) || b.size() != 32) {
    return false;
  }
  std::copy(b.begin(), b.end(), k.begin());
  return true;
}

std::string json_escape(const std::string& s) {
  std::string o;
  for (const char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
    }
    o += c;
  }
  return o;
}

int validate(const std::string& path) {
  const std::string text = slurp(path);
  std::vector<iqk::plan::Finding> f;
  const auto p = iqk::plan::parse(text, f);
  bool ok = p.has_value() && iqk::plan::validate(*p, f);
  for (const auto& x : f) {
    std::cout << R"({"rule":")" << x.rule << R"(","line":)" << x.line << R"(,"detail":")" << json_escape(x.detail)
              << "\"}\n";
  }
  if (p) {
    std::printf("{\"ok\":%s,\"channels\":%zu,\"can_bus_load\":%.4f,\"telemetry_bps\":%.0f,\"crc\":\"%08x\"}\n",
                ok ? "true" : "false", p->channels.size(), iqk::plan::can_bus_load(*p), iqk::plan::telemetry_bps(*p),
                iqk::plan::crc(*p));
  } else {
    std::printf("{\"ok\":false}\n");
  }
  return ok ? 0 : 1;
}

int codec(int argc, char** argv) {
  if (argc >= 6 && std::strcmp(argv[2], "decode") == 0) {
    std::vector<std::uint8_t> b;
    std::string out;
    if (!unhex(argv[5], b) || !iqk::icd::codec_decode(argv[3], argv[4], b, out)) {
      std::cout << "reject\n";
      return 1;
    }
    std::cout << out;
    return 0;
  }
  if (argc >= 5 && std::strcmp(argv[2], "encode") == 0) {
    std::map<std::string, std::string> kv;
    std::string line;
    while (std::getline(std::cin, line)) {
      const auto eq = line.find('=');
      if (eq != std::string::npos) {
        kv[line.substr(0, eq)] = line.substr(eq + 1);
      }
    }
    const auto out = iqk::icd::codec_encode(argv[3], argv[4], kv);
    if (out.empty()) {
      std::cout << "reject\n";
      return 1;
    }
    std::cout << hex(out) << "\n";
    return 0;
  }
  return 2;
}

// Recording format (daud --record): records of port u8 | local_ns i64 LE | len u32 LE | bytes.
// Ports: the iqk::Port values for inputs, 0xFF tick, 0xFE PTP sent, 0xFD plan text, 0xFC clock identity.
int replay(const std::string& path) {
  const std::string rec = slurp(path);
  std::optional<iqk::Dau> unit;
  std::size_t off = 0;
  std::uint64_t packets = 0;
  std::vector<std::uint8_t> stream;
  const iqk::Tx tx = [&](iqk::Port p, std::span<const std::uint8_t> b) {
    if (p == iqk::Port::tlm) {
      ++packets;
      stream.insert(stream.end(), b.begin(), b.end());
    }
  };
  auto rd = [&](std::size_t n) -> std::uint64_t {
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < n; ++i) {
      v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(rec[off + i])) << (8U * i);
    }
    off += n;
    return v;
  };
  while (off + 13 <= rec.size()) {
    const auto port = static_cast<std::uint8_t>(rd(1));
    const auto t = static_cast<std::int64_t>(rd(8));
    const auto len = static_cast<std::size_t>(rd(4));
    if (off + len > rec.size()) {
      std::cerr << "truncated recording\n";
      return 1;
    }
    const std::span<const std::uint8_t> b{reinterpret_cast<const std::uint8_t*>(rec.data()) + off, len};
    off += len;
    if (port == 0xFC && len == 8) {  // the unit's PTP clock identity, first record of every recording
      std::uint64_t id = 0;
      for (std::size_t i = 0; i < 8; ++i) {
        id |= static_cast<std::uint64_t>(b[i]) << (8U * i);
      }
      unit.emplace(id);
      continue;
    }
    if (!unit) {
      std::cerr << "recording does not start with the clock identity\n";
      return 1;
    }
    iqk::Dau& dau = *unit;
    if (port == 0xFF) {
      dau.tick(t, tx);
    } else if (port == 0xFE) {
      dau.on_ptp_sent(t);
    } else if (port == 0xFD) {
      std::vector<iqk::plan::Finding> f;
      (void)dau.load_plan(std::string_view{reinterpret_cast<const char*>(b.data()), b.size()}, t, f);
    } else {
      dau.on_input(static_cast<iqk::Port>(port), b, t, tx);
    }
  }
  const auto digest = iqk::update::sha256(stream);
  std::printf("{\"packets\":%llu,\"bytes\":%zu,\"sha256\":\"%s\"}\n", static_cast<unsigned long long>(packets),
              stream.size(), hex(digest).c_str());
  return 0;
}

int pkg(int argc, char** argv) {
  const std::string sub = argc > 2 ? argv[2] : "";
  if (sub == "keygen" && argc == 5) {
    std::array<std::uint8_t, 32> seed{};
    if (RAND_bytes(seed.data(), static_cast<int>(seed.size())) != 1) {
      return 1;
    }
    std::ofstream(argv[3]) << hex(seed) << "\n";
    std::ofstream(argv[4]) << hex(iqk::update::public_key_of(seed)) << "\n";
    return 0;
  }
  if (sub == "build" && argc == 9) {
    std::array<std::uint8_t, 32> seed{};
    if (!key32(argv[3], seed)) {
      std::cerr << "bad key\n";
      return 1;
    }
    iqk::update::Header h{};
    h.hw_id = static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 0));
    h.version = static_cast<std::uint32_t>(std::strtoul(argv[5], nullptr, 0));
    h.security_version = static_cast<std::uint32_t>(std::strtoul(argv[6], nullptr, 0));
    const std::string img = slurp(argv[7]);
    const auto out = iqk::update::build(h, std::span{reinterpret_cast<const std::uint8_t*>(img.data()), img.size()}, seed);
    std::ofstream(argv[8], std::ios::binary).write(reinterpret_cast<const char*>(out.data()),
                                                   static_cast<std::streamsize>(out.size()));
    return out.empty() ? 1 : 0;
  }
  if (sub == "verify" && argc == 7) {
    iqk::update::PublicKey pk{};
    if (!key32(argv[3], pk)) {
      return 1;
    }
    const std::string p = slurp(argv[6]);
    const auto v = iqk::update::verify(std::span{reinterpret_cast<const std::uint8_t*>(p.data()), p.size()}, pk,
                                       static_cast<std::uint32_t>(std::strtoul(argv[4], nullptr, 0)),
                                       static_cast<std::uint32_t>(std::strtoul(argv[5], nullptr, 0)));
    std::cout << iqk::update::to_string(v) << "\n";
    return v == iqk::update::Verdict::ok ? 0 : 1;
  }
  return 2;
}

int update(int argc, char** argv) {
  if (argc < 6) {
    return 2;
  }
  const std::string sub = argv[2];
  iqk::update::FileStore store{argv[3]};
  iqk::update::PublicKey pk{};
  if (!key32(argv[4], pk)) {
    return 1;
  }
  iqk::update::Agent agent{store, pk, static_cast<std::uint32_t>(std::strtoul(argv[5], nullptr, 0))};
  if (sub == "install" && argc == 7) {
    const std::string p = slurp(argv[6]);
    const auto v = agent.install(std::span{reinterpret_cast<const std::uint8_t*>(p.data()), p.size()});
    std::cout << iqk::update::to_string(v) << "\n";
    return v == iqk::update::Verdict::ok ? 0 : 1;
  }
  if (sub == "boot") {
    const auto s = agent.select();
    static constexpr const char* kKinds[] = {"active", "trial", "rolled_back", "halted"};
    std::printf("{\"boot\":\"%s\",\"slot\":%u,\"why\":\"%s\"}\n", kKinds[static_cast<int>(s.kind)], s.slot,
                iqk::update::to_string(s.why));
    return s.kind == iqk::update::Agent::Boot::halted ? 1 : 0;
  }
  if (sub == "confirm") {
    return agent.confirm() ? 0 : 1;
  }
  if (sub == "state") {
    const auto s = agent.state();
    std::printf("{\"active\":%u,\"pending\":%d,\"trial_boots\":%u,\"security_floor\":%u}\n", s.active,
                s.pending ? static_cast<int>(*s.pending) : -1, s.trial_boots, s.security_floor);
    return 0;
  }
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: iqkctl validate|codec|replay|pkg|update ...\n";
    return 2;
  }
  const std::string cmd = argv[1];
  if (cmd == "validate" && argc == 3) {
    return validate(argv[2]);
  }
  if (cmd == "codec") {
    return codec(argc, argv);
  }
  if (cmd == "replay" && argc == 3) {
    return replay(argv[2]);
  }
  if (cmd == "pkg") {
    return pkg(argc, argv);
  }
  if (cmd == "update") {
    return update(argc, argv);
  }
  std::cerr << "unknown command\n";
  return 2;
}
