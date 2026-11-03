#include "iqk/framing.hpp"

#include <algorithm>

#include "iqk/crc.hpp"

namespace iqk {

std::size_t encode_frame(std::uint8_t id, std::span<const std::uint8_t> payload,
                         std::span<std::uint8_t> out) noexcept {
  if (payload.size() > 255U || out.size() < payload.size() + kFrameOverhead) {
    return 0;
  }
  out[0] = kSync0;
  out[1] = kSync1;
  out[2] = id;
  out[3] = static_cast<std::uint8_t>(payload.size());
  std::copy(payload.begin(), payload.end(), out.begin() + 4);
  const std::uint16_t c = crc::crc16(out.subspan(2, payload.size() + 2U));
  out[4 + payload.size()] = static_cast<std::uint8_t>(c >> 8U);
  out[5 + payload.size()] = static_cast<std::uint8_t>(c & 0xFFU);
  return payload.size() + kFrameOverhead;
}

void Deframer::push(std::uint8_t b) noexcept {
  if (n_ == buf_.size()) {  // cannot happen with max_len <= 255, but never overflow
    drop(1);
  }
  buf_[n_++] = b;
}

void Deframer::drop(std::size_t n) noexcept {
  n = std::min(n, n_);
  std::copy(buf_.begin() + static_cast<std::ptrdiff_t>(n), buf_.begin() + static_cast<std::ptrdiff_t>(n_),
            buf_.begin());
  n_ -= n;
}

bool Deframer::try_extract() noexcept {
  for (;;) {
    // Find the sync word; keep a trailing A5 that may be the first half of one.
    std::size_t i = 0;
    while (i + 1 < n_ && !(buf_[i] == kSync0 && buf_[i + 1] == kSync1)) {
      ++i;
    }
    if (i + 1 >= n_ && !(n_ > 0 && buf_[n_ - 1] == kSync0)) {
      i = n_;
    }
    if (i > 0) {
      stats_.dropped_bytes += i;
      drop(i);
    }
    if (n_ < 4) {
      return false;
    }
    const std::uint8_t len = buf_[3];
    if (len > max_len_) {  // impossible length: this sync word was payload, search again past it
      stats_.dropped_bytes += 1;
      drop(1);
      continue;
    }
    const std::size_t total = len + kFrameOverhead;
    if (n_ < total) {
      return false;
    }
    const std::uint16_t want = crc::crc16(std::span<const std::uint8_t>{buf_.data() + 2, len + 2U});
    const auto got = static_cast<std::uint16_t>((buf_[4 + len] << 8U) | buf_[5 + len]);
    if (want != got) {
      // Corrupt (or a false sync inside data): drop only the sync byte so a real frame that starts inside
      // the bytes we just looked at is still found.
      ++stats_.crc_errors;
      stats_.dropped_bytes += 1;
      drop(1);
      continue;
    }
    frame_.id = buf_[2];
    frame_.len = len;
    std::copy(buf_.begin() + 4, buf_.begin() + 4 + len, frame_.payload.begin());
    drop(total);
    ++stats_.frames;
    return true;
  }
}

}  // namespace iqk
