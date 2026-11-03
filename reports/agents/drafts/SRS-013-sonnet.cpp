#include <vector>

#include "support.hpp"

namespace {

using iqk::Deframer;
using iqk::Frame;

std::vector<std::uint8_t> frame_of(std::uint8_t id, std::size_t len, std::uint8_t fill = 0x11) {
  std::vector<std::uint8_t> p(len, fill);
  std::vector<std::uint8_t> out(len + iqk::kFrameOverhead);
  out.resize(iqk::encode_frame(id, p, out));
  return out;
}

struct Collector {
  std::vector<std::pair<int, std::size_t>> frames;
  void feed(Deframer& d, const std::vector<std::uint8_t>& b) {
    d.feed(b, [&](const Frame& f) { frames.emplace_back(f.id, f.len); });
  }
};

// Sync word + id + a length, with nothing after it.
std::vector<std::uint8_t> bad_header(std::uint8_t len) { return {iqk::kSync0, iqk::kSync1, 0x01, len}; }

}  // namespace

TEST(Srs013, LengthAboveMaxDoesNotSwallowFollowingFrame) {
  IQK_VERIFIES("SRS-013");
  for (const std::uint8_t max : {std::uint8_t{33}, std::uint8_t{10}}) {
    for (const int excess : {1, 2, 100}) {
      const auto bad_len = static_cast<std::uint8_t>(max + excess);
      Deframer d{max};
      Collector c;
      auto bytes = bad_header(bad_len);
      const auto good = frame_of(0x07, 4);  // far shorter than the claimed length
      bytes.insert(bytes.end(), good.begin(), good.end());
      c.feed(d, bytes);
      ASSERT_EQ(c.frames.size(), 1U) << "max " << int{max} << " len " << int{bad_len};
      EXPECT_EQ(c.frames[0], std::make_pair(0x07, std::size_t{4}));
      EXPECT_GT(d.stats().dropped_bytes, 0U);
    }
  }
}

TEST(Srs013, MaxAllowedLengthIsAcceptedButOneMoreIsNot) {
  IQK_VERIFIES("SRS-013");
  constexpr std::uint8_t kMax = 33;
  {
    Deframer d{kMax};
    Collector c;
    c.feed(d, frame_of(0x01, kMax));
    ASSERT_EQ(c.frames.size(), 1U);
    EXPECT_EQ(c.frames[0].second, std::size_t{kMax});
    EXPECT_EQ(d.stats().dropped_bytes, 0U);
  }
  {
    // Fully formed, CRC-correct frame whose length exceeds the maximum by one: corruption, not a frame.
    Deframer d{kMax};
    Collector c;
    c.feed(d, frame_of(0x01, kMax + 1));
    EXPECT_TRUE(c.frames.empty());
    EXPECT_GT(d.stats().dropped_bytes, 0U);
  }
}

TEST(Srs013, ImpossibleLengthRecoversWithoutWaitingForClaimedBytes) {
  IQK_VERIFIES("SRS-013");
  // Claimed length 255 with only a short good frame following: must not wait for 255 bytes.
  Deframer d{33};
  Collector c;
  c.feed(d, bad_header(255));
  EXPECT_TRUE(c.frames.empty());
  c.feed(d, frame_of(0x02, 3));
  ASSERT_EQ(c.frames.size(), 1U);
  c.feed(d, frame_of(0x03, 5));
  ASSERT_EQ(c.frames.size(), 2U);
  EXPECT_EQ(c.frames[1], std::make_pair(0x03, std::size_t{5}));
}

TEST(Srs013, ImpossibleLengthRecoversWhenFedByteByByte) {
  IQK_VERIFIES("SRS-013");
  Deframer d{33};
  std::vector<std::pair<int, std::size_t>> got;
  auto bytes = bad_header(34);
  const auto good = frame_of(0x09, 2);
  bytes.insert(bytes.end(), good.begin(), good.end());
  for (const std::uint8_t b : bytes) {
    d.feed(std::span<const std::uint8_t>{&b, 1}, [&](const Frame& f) { got.emplace_back(f.id, f.len); });
  }
  ASSERT_EQ(got.size(), 1U);
  EXPECT_EQ(got[0], std::make_pair(0x09, std::size_t{2}));
}

TEST(Srs013, ImpossibleLengthFollowedByGoodFrameInsideClaimedPayload) {
  IQK_VERIFIES("SRS-013");
  Deframer d{33};
  Collector c;
  auto bytes = bad_header(200);
  const auto good = frame_of(0x05, 6);
  bytes.insert(bytes.end(), good.begin(), good.end());
  c.feed(d, bytes);
  ASSERT_EQ(c.frames.size(), 1U);
  EXPECT_EQ(c.frames[0], std::make_pair(0x05, std::size_t{6}));
}
