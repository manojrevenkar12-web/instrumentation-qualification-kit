// daud: the data acquisition unit daemon. The imperative shell around iqk::Dau: it owns the serial ports and
// sockets, reads the monotonic clock once per event, and hands bytes and times to the deterministic core.
//
//   daud --imu-tty PATH --gnss-tty PATH --pps-port P --can-port P --can-peer HOST:PORT
//        --ptp-event-port P --ptp-general-port P --ptp-master HOST:PORT --tlm-dest HOST:PORT --diag-port P
//        [--plan FILE] [--record FILE] [--clock-id HEX] [--run-ms N] [--status FILE]
//
// Serial devices are opened raw (termios) at the baud rate their ICD names. The PPS line arrives as a UDP
// datagram per edge on emulated benches; on hardware it is a GPIO edge timestamped by the kernel.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "iqk/dau.hpp"
#include "iqk/icd/gnss.hpp"
#include "iqk/icd/imu.hpp"
#include "iqk/io.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

std::int64_t mono_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

speed_t baud(std::int64_t b) {
  switch (b) {
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    default: return B115200;
  }
}

int open_tty(const std::string& path, std::int64_t rate) {
  const int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) {
    std::perror(path.c_str());
    return -1;
  }
  termios t{};
  if (tcgetattr(fd, &t) == 0) {
    cfmakeraw(&t);
    cfsetispeed(&t, baud(rate));
    cfsetospeed(&t, baud(rate));
    t.c_cflag |= (CLOCAL | CREAD);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &t);
  }
  return fd;
}

int udp_bind(int port) {
  const int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<std::uint16_t>(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (fd < 0 || ::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    (void)std::fprintf(stderr, "bind %d: %s\n", port, std::strerror(errno));
    return -1;
  }
  return fd;
}

// A decimal integer from a command-line value; 0 when the value is not one.
long long to_ll(const std::string& s) {
  char* end = nullptr;
  errno = 0;
  const long long v = std::strtoll(s.c_str(), &end, 10);
  return (errno != 0 || end == s.c_str() || *end != '\0') ? 0 : v;
}

sockaddr_in addr(const std::string& host_port) {
  sockaddr_in a{};
  const auto c = host_port.rfind(':');
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<std::uint16_t>(to_ll(host_port.substr(c + 1))));
  if (inet_pton(AF_INET, host_port.substr(0, c).c_str(), &a.sin_addr) != 1) {
    (void)std::fprintf(stderr, "bad address %s\n", host_port.c_str());
  }
  return a;
}

class Recorder {
 public:
  explicit Recorder(const std::string& path) {
    if (!path.empty()) {
      f_.open(path, std::ios::binary);
    }
  }
  void put(std::uint8_t port, std::int64_t t, std::span<const std::uint8_t> b) {
    if (!f_.is_open()) {
      return;
    }
    std::array<std::uint8_t, 13> h{};
    h[0] = port;
    for (std::size_t i = 0; i < 8; ++i) {
      h[1 + i] = static_cast<std::uint8_t>((static_cast<std::uint64_t>(t) >> (8U * i)) & 0xFFU);
    }
    for (std::size_t i = 0; i < 4; ++i) {
      h[9 + i] = static_cast<std::uint8_t>((b.size() >> (8U * i)) & 0xFFU);
    }
    f_.write(reinterpret_cast<const char*>(h.data()), h.size());
    f_.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
  }
  void flush() {
    if (f_.is_open()) {
      f_.flush();
    }
  }

 private:
  std::ofstream f_;
};

}  // namespace

int main(int argc, char** argv) {
  std::map<std::string, std::string> a;
  for (int i = 1; i + 1 < argc; i += 2) {
    a[argv[i]] = argv[i + 1];
  }
  auto get = [&](const std::string& k) { return a.contains(k) ? a[k] : std::string{}; };
  (void)std::signal(SIGTERM, on_signal);
  (void)std::signal(SIGINT, on_signal);

  const std::uint64_t clock_id = a.contains("--clock-id") ? std::strtoull(get("--clock-id").c_str(), nullptr, 16) : 0x0A0B0C0D0E0F1011ULL;
  iqk::Dau dau{clock_id};
  Recorder rec{get("--record")};
  {
    std::array<std::uint8_t, 8> id{};
    for (int i = 0; i < 8; ++i) {
      id[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((clock_id >> (8 * i)) & 0xFFU);
    }
    rec.put(0xFC, 0, id);
  }

  const int imu = get("--imu-tty").empty() ? -1 : open_tty(get("--imu-tty"), iqk::icd::imu::kBaud);
  const int gnss = get("--gnss-tty").empty() ? -1 : open_tty(get("--gnss-tty"), iqk::icd::gnss::kBaud);
  const int pps = get("--pps-port").empty() ? -1 : udp_bind(static_cast<int>(to_ll(get("--pps-port"))));
  const int can = get("--can-port").empty() ? -1 : udp_bind(static_cast<int>(to_ll(get("--can-port"))));
  const int ptp_ev = get("--ptp-event-port").empty() ? -1 : udp_bind(static_cast<int>(to_ll(get("--ptp-event-port"))));
  const int ptp_gen = get("--ptp-general-port").empty() ? -1 : udp_bind(static_cast<int>(to_ll(get("--ptp-general-port"))));
  const int diag = get("--diag-port").empty() ? -1 : udp_bind(static_cast<int>(to_ll(get("--diag-port"))));
  const int out = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  const sockaddr_in can_peer = addr(get("--can-peer").empty() ? "127.0.0.1:0" : get("--can-peer"));
  const sockaddr_in ptp_master = addr(get("--ptp-master").empty() ? "127.0.0.1:0" : get("--ptp-master"));
  const sockaddr_in tlm_dest = addr(get("--tlm-dest").empty() ? "127.0.0.1:0" : get("--tlm-dest"));
  sockaddr_in diag_peer{};
  socklen_t diag_peer_len = 0;

  const iqk::Tx tx = [&](iqk::Port p, std::span<const std::uint8_t> b) {
    switch (p) {
      case iqk::Port::imu:
        if (imu >= 0 && ::write(imu, b.data(), b.size()) < 0) {
          std::perror("imu write");
        }
        break;
      case iqk::Port::can:
        ::sendto(out, b.data(), b.size(), 0, reinterpret_cast<const sockaddr*>(&can_peer), sizeof can_peer);
        break;
      case iqk::Port::ptp_event: {
        ::sendto(ptp_ev >= 0 ? ptp_ev : out, b.data(), b.size(), 0, reinterpret_cast<const sockaddr*>(&ptp_master),
                 sizeof ptp_master);
        const std::int64_t t3 = mono_ns();  // software transmit timestamp
        dau.on_ptp_sent(t3);
        rec.put(0xFE, t3, {});
        break;
      }
      case iqk::Port::tlm:
        ::sendto(out, b.data(), b.size(), 0, reinterpret_cast<const sockaddr*>(&tlm_dest), sizeof tlm_dest);
        break;
      case iqk::Port::diag:
        if (diag >= 0 && diag_peer_len > 0) {
          ::sendto(diag, b.data(), b.size(), 0, reinterpret_cast<const sockaddr*>(&diag_peer), diag_peer_len);
        }
        break;
      default:
        break;
    }
  };

  if (!get("--plan").empty()) {
    const std::string text = iqk::io::read_file(get("--plan"));
    std::vector<iqk::plan::Finding> findings;
    const std::int64_t t = mono_ns();
    rec.put(0xFD, t, std::span{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    if (!dau.load_plan(text, t, findings)) {
      for (const auto& x : findings) {
        (void)std::fprintf(stderr, "plan %s line %d: %s\n", x.rule.c_str(), x.line, x.detail.c_str());
      }
    }
  }

  struct Src {
    int fd;
    iqk::Port port;
    bool dgram;
  };
  std::vector<Src> srcs;
  for (const Src s : {Src{imu, iqk::Port::imu, false}, Src{gnss, iqk::Port::gnss, false}, Src{pps, iqk::Port::pps, true},
                      Src{can, iqk::Port::can, true}, Src{ptp_ev, iqk::Port::ptp_event, true},
                      Src{ptp_gen, iqk::Port::ptp_general, true}, Src{diag, iqk::Port::diag, true}}) {
    if (s.fd >= 0) {
      srcs.push_back(s);
    }
  }
  std::vector<pollfd> pfds;
  pfds.reserve(srcs.size());
  for (const auto& s : srcs) {
    pfds.push_back(pollfd{s.fd, POLLIN, 0});
  }

  const std::int64_t start = mono_ns();
  const std::int64_t run_ns = a.contains("--run-ms") ? to_ll(get("--run-ms")) * 1'000'000 : INT64_MAX;
  std::int64_t last_tick = 0;
  std::array<std::uint8_t, 2048> buf{};
  while (g_stop == 0 && mono_ns() - start < run_ns) {
    ::poll(pfds.data(), pfds.size(), 2);
    for (std::size_t i = 0; i < pfds.size(); ++i) {
      if ((pfds[i].revents & POLLIN) == 0) {
        continue;
      }
      for (;;) {
        ssize_t n = 0;
        if (srcs[i].port == iqk::Port::diag) {
          diag_peer_len = sizeof diag_peer;
          n = ::recvfrom(srcs[i].fd, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&diag_peer), &diag_peer_len);
        } else {
          n = ::read(srcs[i].fd, buf.data(), buf.size());
        }
        if (n <= 0) {
          break;
        }
        const std::int64_t t = mono_ns();
        const std::span<const std::uint8_t> b{buf.data(), static_cast<std::size_t>(n)};
        rec.put(static_cast<std::uint8_t>(srcs[i].port), t, b);
        dau.on_input(srcs[i].port, b, t, tx);
      }
    }
    const std::int64_t now = mono_ns();
    if (now - last_tick >= 2'000'000) {
      rec.put(0xFF, now, {});
      dau.tick(now, tx);
      last_tick = now;
    }
  }
  rec.flush();
  if (!get("--status").empty()) {
    const std::int64_t now = mono_ns();
    const auto s = dau.status(now);
    std::ofstream f(get("--status"));
    f << "{\"packets\":" << dau.packets_sent() << ",\"samples\":" << dau.samples_sent() << ",\"armed\":"
      << (dau.armed() ? "true" : "false") << ",\"plan_crc\":" << dau.plan_crc() << ",\"bit\":" << s.bit
      << ",\"time_state\":" << int{s.time_state} << ",\"time_source\":" << int{s.time_source}
      << ",\"offset_ns\":" << s.offset_ns << ",\"imu_crc_errors\":" << s.imu_crc_errors << ",\"imu_gaps\":" << s.imu_gaps
      << ",\"can_errors\":" << s.can_errors << ",\"faults\":" << dau.faults().total() << ",\"loader\":"
      << static_cast<int>(dau.loader().status()) << ",\"loader_failure\":" << static_cast<int>(dau.loader().failure())
      << ",\"fault_codes\":{";
    std::map<int, std::uint32_t> counts;
    for (std::size_t i = 0; i < dau.faults().size(); ++i) {
      counts[static_cast<int>(dau.faults()[i].code)] += dau.faults()[i].count;
    }
    bool first = true;
    for (const auto& [code, c] : counts) {
      f << (first ? "" : ",") << "\"" << code << "\":" << c;
      first = false;
    }
    f << "}}\n";
  }
  return 0;
}
