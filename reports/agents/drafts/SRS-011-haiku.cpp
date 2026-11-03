// SRS-011 Resynchronisation after corruption: a corrupted frame costs at most that frame; the next valid frame is
// delivered, including when the corrupt bytes contain a sync word.
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <span>
#include <vector>

#include "support.hpp"
#include "iqk/framing.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;
using Frames = std::vector<struct Got>;

struct Got {
  std::uint8_t id{};
  Bytes payload;
  friend bool operator==(const Got&, const Got&) = default;
};

Bytes wire(std::uint8_t id, const Bytes& payload) {
  Bytes out(payload.size() + iqk::kFrameOverhead);
  out.resize(iqk::encode_frame(id, payload, out));
  return out;
}

// Deterministic payload of n bytes. Every byte is below 0x80, so no byte is 0xA5 and the payload never contains a
// sync word; sync words are placed deliberately where a test needs them.
Bytes plain(std::size_t n, std::uint8_t seed) {
  Bytes p(n);
  for (std::size_t i = 0; i < n; ++i) {
    p[i] = static_cast<std::uint8_t>((i * 31U + static_cast<std::size_t>(seed)) & 0x7FU);
  }
  return p;
}

Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes& p : parts) {
    out.insert(out.end(), p.begin(), p.end());
  }
  return out;
}

// Feeds bytes to the deframer in chunks of the given size and appends each delivered frame to got.
void feed_chunked(iqk::Deframer& d, const Bytes& bytes, std::size_t chunk, Frames& got) {
  for (std::size_t off = 0; off < bytes.size(); off += chunk) {
    const std::size_t n = std::min(chunk, bytes.size() - off);
    d.feed(std::span<const std::uint8_t>{bytes.data() + off, n}, [&](const iqk::Frame& f) {
      got.push_back(Got{f.id, Bytes(f.data().begin(), f.data().end())});
    });
  }
}

Frames feed_all(iqk::Deframer& d, const Bytes& bytes) {
  Frames got;
  feed_chunked(d, bytes, bytes.size(), got);
  return got;
}

}  // namespace

// A payload byte is flipped: the frame's CRC fails, the frames on either side are delivered unchanged.
TEST(Resync, CorruptFrameCostsOnlyItself) {
  IQK_VERIFIES("SRS-011");
  Bytes bad = wire(0x02, plain(20, 2));
  bad[10] ^= 0x40;  // payload byte; the CRC no longer matches
  iqk::Deframer d{33};
  const Frames got = feed_all(d, cat({wire(0x01, plain(20, 1)), bad, wire(0x03, plain(20, 3))}));
  ASSERT_EQ(got.size(), 2U);
  EXPECT_EQ(got[0], (Got{0x01, plain(20, 1)}));
  EXPECT_EQ(got[1], (Got{0x03, plain(20, 3)}));
  EXPECT_EQ(d.stats().crc_errors, 1U);
  EXPECT_EQ(d.stats().frames, 2U);
}

// Every byte of an IMU-sized frame, corrupted with three different masks (one bit, the top bit, all bits): only
// that frame may be lost, the valid frame after it is always delivered.
TEST(Resync, CorruptionAtAnyByteOfAFrameCostsOnlyThatFrame) {
  IQK_VERIFIES("SRS-011");
  const Bytes victim = wire(0x02, plain(33, 4));
  const Bytes next = wire(0x01, plain(33, 9));
  const Frames expected{Got{0x01, plain(33, 9)}};
  const std::array<std::uint8_t, 3> masks{0x01, 0x80, 0xFF};
  for (std::size_t i = 0; i < victim.size(); ++i) {
    for (const std::uint8_t m : masks) {
      Bytes bad = victim;
      bad[i] ^= m;
      iqk::Deframer d{33};
      const Frames got = feed_all(d, cat({bad, next}));
      EXPECT_EQ(got, expected) << "corrupted byte " << i << " with mask 0x" << std::hex << static_cast<int>(m);
    }
  }
}

// The corrupt length byte claims more payload than the frame has, so the bogus frame runs into the next frame.
// The next frame must still be delivered, and the one after it too.
TEST(Resync, CorruptLengthClaimingNextFrameStillDeliversIt) {
  IQK_VERIFIES("SRS-011");
  Bytes bad = wire(0x02, plain(20, 2));
  bad[3] = 30;  // true length 20; the claimed frame extends 10 bytes into the next frame
  iqk::Deframer d{33};
  const Frames got = feed_all(d, cat({bad, wire(0x03, plain(20, 3)), wire(0x04, plain(20, 4))}));
  ASSERT_EQ(got.size(), 2U);
  EXPECT_EQ(got[0], (Got{0x03, plain(20, 3)}));
  EXPECT_EQ(got[1], (Got{0x04, plain(20, 4)}));
}

// The corrupt payload contains a complete-looking sync word with a length (28) whose claimed frame runs past the
// corrupt frame into the next one. The real frame fails its CRC; the next valid frame must still come out.
TEST(Resync, FalseSyncInCorruptPayloadDoesNotSwallowNextFrame) {
  IQK_VERIFIES("SRS-011");
  Bytes payload = plain(20, 2);
  payload[0] = 0xA5;
  payload[1] = 0x5A;
  payload[2] = 0x02;  // id of the false frame
  payload[3] = 28;    // length of the false frame: 34 bytes, which reaches into the next frame
  Bytes bad = wire(0x02, payload);
  bad[4 + 15] ^= 0x01;  // corrupt the real frame's payload so its CRC fails
  iqk::Deframer d{33};
  const Frames got = feed_all(d, cat({bad, wire(0x03, plain(20, 3))}));
  EXPECT_EQ(got, (Frames{Got{0x03, plain(20, 3)}}));
}

// The two CRC bytes of a frame are overwritten with a sync word, so the frame's own tail is a false sync. The next
// valid frame starts right after the tail; its id and length bytes make the false header an impossible length.
TEST(Resync, SyncWordOverwritingCrcDoesNotSwallowNextFrame) {
  IQK_VERIFIES("SRS-011");
  const Bytes original = wire(0x02, plain(20, 2));
  Bytes bad = original;
  bad[24] = 0xA5;
  bad[25] = 0x5A;
  ASSERT_NE(bad, original) << "test precondition: the CRC must actually change";
  iqk::Deframer d{33};
  const Frames got = feed_all(d, cat({bad, wire(0x03, plain(20, 3))}));
  EXPECT_EQ(got, (Frames{Got{0x03, plain(20, 3)}}));
}

// The sync word of the corrupt frame is destroyed, so the frame is not recognised at all: the next frame is found.
TEST(Resync, DamagedSyncWordDoesNotLoseNextFrame) {
  IQK_VERIFIES("SRS-011");
  const Bytes original = wire(0x02, plain(20, 2));
  const Bytes next = wire(0x03, plain(20, 3));
  for (const std::size_t pos : {std::size_t{0}, std::size_t{1}}) {
    Bytes bad = original;
    bad[pos] ^= 0xFF;
    iqk::Deframer d{33};
    const Frames got = feed_all(d, cat({bad, next}));
    EXPECT_EQ(got, (Frames{Got{0x03, plain(20, 3)}})) << "sync byte " << pos << " damaged";
  }
}

// Leading junk that contains a sync word and a plausible-looking header ahead of the first valid frame.
TEST(Resync, JunkContainingSyncWordBeforeFrameIsSkipped) {
  IQK_VERIFIES("SRS-011");
  const Bytes junk{0xA5, 0x5A, 0x01, 0x05, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77};
  iqk::Deframer d{33};
  const Frames got = feed_all(d, cat({junk, wire(0x03, plain(20, 3))}));
  EXPECT_EQ(got, (Frames{Got{0x03, plain(20, 3)}}));
}

// The corrupt frame ends in 0xA5, the first half of the next frame's sync word. Fed byte by byte, the deframer must
// keep that trailing byte and not lose the next frame.
TEST(Resync, TrailingSyncHalfOfCorruptFrameIsKeptAcrossChunks) {
  IQK_VERIFIES("SRS-011");
  const Bytes original = wire(0x02, plain(20, 2));
  Bytes bad = original;
  bad.back() = 0xA5;
  ASSERT_NE(bad, original) << "test precondition: the CRC must actually change";
  const Bytes next = wire(0x03, plain(20, 3));
  for (const std::size_t chunk : {std::size_t{1}, std::size_t{2}, std::size_t{5}, std::size_t{26}}) {
    iqk::Deframer d{33};
    Frames got;
    feed_chunked(d, cat({bad, next}), chunk, got);
    EXPECT_EQ(got, (Frames{Got{0x03, plain(20, 3)}})) << "chunk size " << chunk;
  }
}

// At the device's maximum payload (GNSS time pulse, 13 bytes): a length one above the maximum is rejected at once,
// a length equal to the maximum is a candidate frame that the CRC rejects; the frame after is delivered both times.
TEST(Resync, LengthBoundaryAtDeviceMaximumDoesNotLoseNextFrame) {
  IQK_VERIFIES("SRS-011");
  const Bytes next = wire(0x20, plain(13, 6));
  const Frames expected{Got{0x20, plain(13, 6)}};
  {
    Bytes bad = wire(0x21, plain(8, 5));
    bad[3] = 14;  // one above the maximum
    iqk::Deframer d{13};
    EXPECT_EQ(feed_all(d, cat({bad, next})), expected) << "length 14";
  }
  {
    Bytes bad = wire(0x21, plain(8, 5));
    bad[3] = 13;  // exactly the maximum: the claimed frame ends inside the next frame
    iqk::Deframer d{13};
    EXPECT_EQ(feed_all(d, cat({bad, next})), expected) << "length 13";
  }
}

// A long stream of IMU frames, every third one corrupted by a payload-byte flip. Some valid frames carry a sync word
// in their payload. The stream is fed in 7-byte chunks: exactly the uncorrupted frames must come out, in order.
TEST(Resync, ManyCorruptFramesInStreamCostOnlyThemselves) {
  IQK_VERIFIES("SRS-011");
  constexpr std::size_t kFrames = 60;
  Bytes stream;
  Frames expected;
  std::size_t corrupted = 0;
  for (std::size_t k = 0; k < kFrames; ++k) {
    // seq 0x5AA5 puts the sync word 0xA5 0x5A at the start of the payload.
    const auto seq = static_cast<std::uint16_t>(k % 4 == 0 ? 0x5AA5U : k);
    Bytes w = iqk_test::imu_frame(iqk_test::imu_sample(seq, static_cast<std::uint32_t>(k * 2500U)));
    if (k % 3 == 1) {
      w[4 + (k % 33U)] ^= 0x10;
      ++corrupted;
    } else {
      expected.push_back(Got{0x01, Bytes(w.begin() + 4, w.begin() + 4 + 33)});
    }
    stream.insert(stream.end(), w.begin(), w.end());
  }
  iqk::Deframer d{33};
  Frames got;
  feed_chunked(d, stream, 7, got);
  EXPECT_EQ(got, expected);
  EXPECT_GE(d.stats().crc_errors, corrupted);
}
