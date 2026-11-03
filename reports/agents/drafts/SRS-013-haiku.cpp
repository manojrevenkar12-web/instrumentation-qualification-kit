// SRS-013: a length longer than the device can send is treated as corruption at once, so it never stalls a link.
#include <cstddef>
#include <cstdint>
#include <vector>

#include "iqk/drivers.hpp"
#include "iqk/faultlog.hpp"
#include "iqk/framing.hpp"
#include "support.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;

// A complete, CRC-correct serial frame with a zero payload of the given length (any length up to 255).
Bytes make_frame(std::uint8_t id, std::size_t len) {
  const Bytes payload(len, 0x00);
  Bytes out(len + iqk::kFrameOverhead);
  const std::size_t n = iqk::encode_frame(id, payload, out);
  out.resize(n);
  return out;
}

// A header that claims a payload length, with no bytes behind it.
Bytes header(std::uint8_t id, std::uint8_t len) { return Bytes{iqk::kSync0, iqk::kSync1, id, len}; }

Bytes concat(std::initializer_list<Bytes> parts) {
  Bytes out;
  for (const Bytes& p : parts) {
    out.insert(out.end(), p.begin(), p.end());
  }
  return out;
}

std::vector<iqk::Frame> deliver(iqk::Deframer& d, const Bytes& bytes) {
  std::vector<iqk::Frame> got;
  d.feed(bytes, [&](const iqk::Frame& f) { got.push_back(f); });
  return got;
}

}  // namespace

// A frame that is longer than the device can send is not delivered.
TEST(Srs013, FrameLongerThanDeviceMaxIsRejected) {
  IQK_VERIFIES("SRS-013");
  iqk::Deframer d{33};
  const auto got = deliver(d, make_frame(0x01, 34));  // CRC-valid, so only the length can reject it
  EXPECT_TRUE(got.empty());
  EXPECT_EQ(d.stats().frames, 0U);
  EXPECT_GE(d.stats().dropped_bytes, 1U);
}

// The impossible length must not stall the link: a valid frame that follows the bogus header is delivered in the
// same feed call, without waiting for the bytes the bogus header claimed.
TEST(Srs013, ImpossibleLengthDoesNotStallFollowingFrame) {
  IQK_VERIFIES("SRS-013");
  iqk::Deframer d{33};
  const Bytes good = make_frame(0x02, 4);
  const auto got = deliver(d, concat({header(0x01, 0xFF), good}));  // 255 claimed; 4 + 6 bytes actually sent
  ASSERT_EQ(got.size(), 1U);
  EXPECT_EQ(got[0].id, 0x02);
  EXPECT_EQ(got[0].len, 4);
  EXPECT_EQ(d.stats().frames, 1U);
}

// Same, one byte at a time: the deframer must not hold on to the bogus header while waiting for more bytes.
TEST(Srs013, ImpossibleLengthDoesNotStallWhenBytesArriveOneByOne) {
  IQK_VERIFIES("SRS-013");
  iqk::Deframer d{33};
  const Bytes stream = concat({header(0x01, 0xFF), make_frame(0x02, 4)});
  std::vector<iqk::Frame> got;
  std::size_t delivered_at = 0;
  for (std::size_t i = 0; i < stream.size(); ++i) {
    const Bytes one{stream[i]};
    d.feed(one, [&](const iqk::Frame& f) {
      got.push_back(f);
      delivered_at = i + 1;
    });
  }
  ASSERT_EQ(got.size(), 1U);
  EXPECT_EQ(got[0].id, 0x02);
  EXPECT_EQ(delivered_at, stream.size());  // delivered on its last byte, not later
}

// Every length above the maximum is rejected, from the first impossible value to the largest a byte can hold.
TEST(Srs013, EveryLengthAboveMaxIsRejected) {
  IQK_VERIFIES("SRS-013");
  for (std::size_t len = 34; len <= 255; ++len) {
    iqk::Deframer d{33};
    const Bytes good = make_frame(0x02, 4);
    const auto got = deliver(d, concat({make_frame(0x01, len), good}));
    ASSERT_EQ(got.size(), 1U) << "claimed length " << len;
    EXPECT_EQ(got[0].id, 0x02) << "claimed length " << len;
    EXPECT_EQ(got[0].len, 4) << "claimed length " << len;
  }
}

// A length equal to the maximum is possible and must still be accepted (boundary).
TEST(Srs013, LengthEqualToMaxIsAccepted) {
  IQK_VERIFIES("SRS-013");
  iqk::Deframer d{33};
  const auto got = deliver(d, make_frame(0x01, 33));
  ASSERT_EQ(got.size(), 1U);
  EXPECT_EQ(got[0].id, 0x01);
  EXPECT_EQ(got[0].len, 33);
  EXPECT_EQ(d.stats().crc_errors, 0U);
}

// With the largest length a byte can carry as the maximum, nothing is impossible: a 255-byte frame is accepted.
TEST(Srs013, MaxOf255AcceptsFullLengthFrame) {
  IQK_VERIFIES("SRS-013");
  iqk::Deframer d{255};
  const auto got = deliver(d, make_frame(0x05, 255));
  ASSERT_EQ(got.size(), 1U);
  EXPECT_EQ(got[0].len, 255);
}

// Boundary at zero: a device that sends no payload rejects any non-empty length, and an empty frame is fine.
TEST(Srs013, MaxOfZeroRejectsAnyPayload) {
  IQK_VERIFIES("SRS-013");
  {
    iqk::Deframer d{0};
    const auto got = deliver(d, make_frame(0x01, 1));
    EXPECT_TRUE(got.empty());
  }
  {
    iqk::Deframer d{0};
    const auto got = deliver(d, make_frame(0x01, 0));
    ASSERT_EQ(got.size(), 1U);
    EXPECT_EQ(got[0].len, 0);
  }
}

// On the IMU link (max 33 bytes), an impossible length does not stop the next real sample from being decoded.
TEST(Srs013, ImuLinkRecoversFromImpossibleLengthAtOnce) {
  IQK_VERIFIES("SRS-013");
  iqk::ImuDriver drv;
  iqk::FaultLog log;
  std::size_t readings = 0;
  const Bytes sample = iqk_test::imu_frame(iqk_test::imu_sample(7, 1000));
  const Bytes stream = concat({header(0x01, 34), sample});  // 34 is one past the IMU payload size
  drv.on_bytes(stream, 5 * iqk_test::kMs, log, [&](const iqk::ImuReading&) { ++readings; });
  EXPECT_EQ(readings, 1U);
  EXPECT_EQ(drv.samples(), 1U);
  EXPECT_GE(drv.link().dropped_bytes, 1U);
}
