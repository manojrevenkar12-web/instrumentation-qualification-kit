// Whole-file reads for the tools (std::FILE, so no iostream buffering paths in the binaries).
#pragma once

#include <array>
#include <cstdio>
#include <string>

namespace iqk::io {

[[nodiscard]] inline std::string read_file(const std::string& path) {
  std::string out;
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) {
    return out;
  }
  std::array<char, 65536> buf{};
  std::size_t n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
    out.append(buf.data(), n);
  }
  (void)std::fclose(f);
  return out;
}

}  // namespace iqk::io
