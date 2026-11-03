#include <gtest/gtest.h>

#include <algorithm>
#include <random>

#include "iqk/framing.hpp"
#include "support.hpp"

using iqk::Deframer;
using iqk::Frame;

namespace {
std::vector<std::vector<std::uint8_t>> feed_all(Deframer& d, std::span<const std::uint8_t> bytes, std::size_t chunk) {
  std::vector<std::vector<std::uint8_t>> out;
  for (std::size_t i = 0; i < bytes.size(); i += chunk) {
    const auto part = bytes.subspan(i, std::min(chunk, bytes.size() - i));
    d.feed(part, [&](const Frame& f) { out.emplace_back(f.data().begin(), f.data().end()); });
  }
  return out;
}

std::vector<std::uint8_t> stream_of(int n) {
  std::vector<std::uint8_t> s;
  for (int i = 0; i < n; ++i) {
    const auto f = iqk_test::imu_frame(iqk_test::imu_sample(static_cast<std::uint16_t>(i), static_cast<std::uint32_t>(i * 2500)));
    s.insert(s.end(), f.begin(), f.end());
  }
  return s;
}
}  // namespace

TEST(Framing, EveryChunkingDeliversEveryFrame) {
  IQK_VERIFIES("SRS-010");
  const auto s = stream_of(20);
  for (std::size_t chunk = 1; chunk <= s.size(); chunk += (chunk < 40 ? 1 : 37)) {
    Deframer d{33};
    const auto frames = feed_all(d, s, chunk);
    ASSERT_EQ(frames.size(), 20U) << "chunk " << chunk;
    EXPECT_EQ(d.stats().crc_errors, 0U);
    EXPECT_EQ(d.stats().dropped_bytes, 0U);
  }
}

TEST(Framing, AFlippedBitCostsOnlyThatFrame) {
  IQK_VERIFIES("SRS-011", "SRS-012");
  auto s = stream_of(5);
  s[39 + 10] ^= 0x04;  // inside the payload of frame 2 (each frame is 39 bytes)
  Deframer d{33};
  const auto frames = feed_all(d, s, 7);
  EXPECT_EQ(frames.size(), 4U);
  EXPECT_EQ(d.stats().crc_errors, 1U);
}

TEST(Framing, ASyncWordInsideCorruptDataDoesNotHideTheNextFrame) {
  IQK_VERIFIES("SRS-011");
  // A corrupt frame whose payload contains A5 5A followed by a valid frame: the false sync must not swallow it.
  std::vector<std::uint8_t> s = {0xA5, 0x5A, 0x01, 0x04, 0x00, 0xA5, 0x5A, 0x01, 0x00, 0x00};
  const auto good = iqk_test::imu_frame(iqk_test::imu_sample(9, 9));
  s.insert(s.end(), good.begin(), good.end());
  Deframer d{33};
  const auto frames = feed_all(d, s, 1);
  ASSERT_EQ(frames.size(), 1U);
  EXPECT_EQ(frames[0][0], 9);
}

TEST(Framing, ATruncatedFrameDoesNotTakeTheNextOneWithIt) {
  IQK_VERIFIES("SRS-011");
  // A frame that lost bytes on the line: its length field promises 33 payload bytes that never come, so the
  // next frame's first bytes complete it and its CRC fails. The next frame must still be delivered.
  auto cut = iqk_test::imu_frame(iqk_test::imu_sample(1, 1));
  cut.resize(14);
  const auto good = iqk_test::imu_frame(iqk_test::imu_sample(2, 2));
  std::vector<std::uint8_t> s = cut;
  s.insert(s.end(), good.begin(), good.end());
  Deframer d{33};
  const auto frames = feed_all(d, s, 5);
  ASSERT_EQ(frames.size(), 1U);
  EXPECT_EQ(frames[0][0], 2);
  EXPECT_EQ(d.stats().crc_errors, 1U);
}

TEST(Framing, GarbageBeforeAFrameIsDiscarded) {
  IQK_VERIFIES("SRS-011");
  std::vector<std::uint8_t> s = {0x00, 0xA5, 0x13, 0x5A, 0xA5};
  const auto good = stream_of(3);
  s.insert(s.end(), good.begin(), good.end());
  Deframer d{33};
  EXPECT_EQ(feed_all(d, s, 3).size(), 3U);
  EXPECT_EQ(d.stats().dropped_bytes, 5U);
}

TEST(Framing, CrcErrorsAreCounted) {
  IQK_VERIFIES("SRS-012");
  auto s = stream_of(10);
  for (int k : {0, 3, 7}) {
    s[static_cast<std::size_t>(k) * 39 + 37] ^= 0xFF;  // the CRC bytes
  }
  Deframer d{33};
  EXPECT_EQ(feed_all(d, s, 64).size(), 7U);
  EXPECT_EQ(d.stats().crc_errors, 3U);
}

TEST(Framing, AnImpossibleLengthDoesNotStallTheLink) {
  IQK_VERIFIES("SRS-013");
  // len 0xFF is longer than any IMU frame: treated as corruption at once, so the following frame is delivered
  // when it arrives instead of after 255 more bytes.
  std::vector<std::uint8_t> s = {0xA5, 0x5A, 0x01, 0xFF};
  const auto good = iqk_test::imu_frame(iqk_test::imu_sample(1, 1));
  s.insert(s.end(), good.begin(), good.end());
  Deframer d{33};
  EXPECT_EQ(feed_all(d, s, 1).size(), 1U);
}

TEST(Framing, RandomCorruptionNeverProducesABadFrame) {
  IQK_VERIFIES("SRS-011");
  std::mt19937 rng{42};
  const auto clean = stream_of(400);
  auto s = clean;
  std::uniform_int_distribution<std::size_t> pos(0, s.size() - 1);
  for (int i = 0; i < 60; ++i) {
    s[pos(rng)] ^= static_cast<std::uint8_t>(1U << (rng() % 8));
  }
  Deframer d{33};
  const auto frames = feed_all(d, s, 17);
  // Every delivered frame is one of the frames that was sent.
  Deframer ref{33};
  const auto sent = feed_all(ref, clean, 1000);
  for (const auto& f : frames) {
    EXPECT_NE(std::find(sent.begin(), sent.end(), f), sent.end());
  }
  EXPECT_GE(frames.size(), 400U - 60U);
}
