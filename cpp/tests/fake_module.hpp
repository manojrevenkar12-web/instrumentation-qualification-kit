// An in-process acquisition module answering the register protocol, for loader and unit tests.
#pragma once

#include <array>
#include <functional>
#include <optional>
#include <vector>

#include "iqk/can.hpp"
#include "iqk/crc.hpp"
#include "iqk/icd/aam.hpp"

namespace iqk_test {

struct FakeModule {
  int node{1};
  struct In {
    std::uint32_t enable{0}, range{0}, divisor{1}, filter{0};
  };
  std::array<In, 12> regs{};
  std::array<In, 12> applied{};
  std::uint8_t state{1};
  // Fault knobs
  int ignore_next{0};            // drop this many requests
  bool abort_all{false};
  bool corrupt_range_readback{false};
  bool wrong_crc{false};
  int requests{0};

  std::uint32_t crc() const {
    std::array<std::uint8_t, 12 * 16> b{};
    std::size_t off = 0;
    for (const auto& c : applied) {
      for (const std::uint32_t v : {c.enable, c.range, c.divisor, c.filter}) {
        for (int k = 0; k < 4; ++k) {
          b[off++] = static_cast<std::uint8_t>((v >> (8 * k)) & 0xFFU);
        }
      }
    }
    return iqk::crc::crc32(b);
  }

  // Handles one frame from the unit; returns the module's answer, if any.
  std::optional<iqk::CanFrame> on_frame(const iqk::CanFrame& f) {
    namespace aam = iqk::icd::aam;
    if (f.id == aam::AamNmt::kId && f.dlc == 2 && f.data[1] == node && f.data[0] == 0x01) {
      state = 2;
      return std::nullopt;
    }
    if (f.id != aam::AamRegRequest::can_id(static_cast<std::uint8_t>(node))) {
      return std::nullopt;
    }
    ++requests;
    if (ignore_next > 0) {
      --ignore_next;
      return std::nullopt;
    }
    aam::AamRegRequest r{};
    if (!aam::decode(f.payload(), r)) {
      return std::nullopt;
    }
    aam::AamRegResponse a{};
    a.index = r.index;
    a.sub = r.sub;
    a.cmd = 0x80;
    a.value = 0x06020000;
    if (!abort_all) {
      if (r.index >= 0x2000 && r.index < 0x200C && r.sub >= 1 && r.sub <= 4) {
        In& in = regs[r.index - 0x2000U];
        std::uint32_t* field = r.sub == 1 ? &in.enable : r.sub == 2 ? &in.range : r.sub == 3 ? &in.divisor : &in.filter;
        if (r.cmd == 0x23) {
          *field = r.value;
          a.cmd = 0x60;
          a.value = 0;
        } else if (r.cmd == 0x40) {
          a.cmd = 0x43;
          a.value = *field ^ ((corrupt_range_readback && r.sub == 2) ? 1U : 0U);
        }
      } else if (r.index == 0x1F00 && r.cmd == 0x23) {
        applied = regs;
        a.cmd = 0x60;
        a.value = 0;
      } else if (r.index == 0x1F01 && r.cmd == 0x40) {
        a.cmd = 0x43;
        a.value = crc() ^ (wrong_crc ? 1U : 0U);
      }
    }
    iqk::CanFrame out{};
    out.id = aam::AamRegResponse::can_id(static_cast<std::uint8_t>(node));
    out.dlc = static_cast<std::uint8_t>(aam::encode(a, out.data));
    return out;
  }
};

}  // namespace iqk_test
