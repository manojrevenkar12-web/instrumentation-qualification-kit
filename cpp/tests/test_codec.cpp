#include <gtest/gtest.h>

#include "iqk/crc.hpp"
#include "iqk/icd/aam.hpp"
#include "iqk/icd/gnss.hpp"
#include "iqk/icd/imu.hpp"
#include "iqk/icd/ptp.hpp"
#include "iqk/icd/tlm.hpp"
#include "support.hpp"

namespace imu = iqk::icd::imu;
namespace ptp = iqk::icd::ptp;
namespace tlm = iqk::icd::tlm;
namespace gnss = iqk::icd::gnss;
namespace aam = iqk::icd::aam;

TEST(Crc, CheckValues) {
  IQK_VERIFIES("SRS-001");
  const std::uint8_t d[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  EXPECT_EQ(iqk::crc::crc16(d), 0x29B1);
  EXPECT_EQ(iqk::crc::crc32(d), 0xCBF43926U);
}

TEST(Codec, ImuSampleLayoutIsLittleEndianAsInTheIcd) {
  IQK_VERIFIES("SRS-001");
  imu::ImuSample s{};
  s.seq = 0x0102;
  s.t_us = 0x0A0B0C0D;
  s.gyro_x = -2;
  s.temp = -4000;
  s.status = 0x0F;
  std::array<std::uint8_t, imu::ImuSample::kSize> b{};
  ASSERT_EQ(imu::encode(s, b), imu::ImuSample::kSize);
  EXPECT_EQ(b[0], 0x02);
  EXPECT_EQ(b[1], 0x01);
  EXPECT_EQ(b[2], 0x0D);
  EXPECT_EQ(b[5], 0x0A);
  EXPECT_EQ(b[6], 0xFE);  // -2 as two's complement, little-endian
  EXPECT_EQ(b[9], 0xFF);
  EXPECT_EQ(b[30], 0x60);  // -4000 = 0xF060
  EXPECT_EQ(b[31], 0xF0);
  EXPECT_EQ(b[32], 0x0F);
  imu::ImuSample back{};
  ASSERT_TRUE(imu::decode(b, back));
  EXPECT_EQ(back.gyro_x, -2);
  EXPECT_EQ(back.temp, -4000);
  EXPECT_EQ(back.t_us, 0x0A0B0C0DU);
}

TEST(Codec, PtpIsBigEndianWith48BitSeconds) {
  IQK_VERIFIES("SRS-001");
  ptp::PtpFollowUp m{};
  m.type_sdo = 0x8;
  m.version = 2;
  m.length = 44;
  m.sequence_id = 0x1234;
  m.correction = -1;
  m.origin_s = 0x0000665544332211ULL;
  m.origin_ns = 999'999'999;
  std::array<std::uint8_t, ptp::PtpFollowUp::kSize> b{};
  ASSERT_EQ(ptp::encode(m, b), 44U);
  EXPECT_EQ(b[2], 0x00);
  EXPECT_EQ(b[3], 44);
  EXPECT_EQ(b[8], 0xFF);   // correction -1, big-endian
  EXPECT_EQ(b[30], 0x12);  // sequence_id at offset 30
  EXPECT_EQ(b[31], 0x34);
  EXPECT_EQ(b[34], 0x66);  // origin_s, 6 bytes big-endian
  EXPECT_EQ(b[39], 0x11);
  ptp::PtpFollowUp back{};
  ASSERT_TRUE(ptp::decode(b, back));
  EXPECT_EQ(back.origin_s, 0x665544332211ULL);
  EXPECT_EQ(back.correction, -1);
  EXPECT_TRUE(ptp::in_range(back));
}

TEST(Codec, TelemetryPacketRoundTripsWithItsRecords) {
  IQK_VERIFIES("SRS-001");
  tlm::TlmPacket p{};
  p.magic = 0x4B514954U;
  p.seq = 7;
  p.time_ns = 1'700'000'000'123'456'789ULL;
  p.time_quality = 3;
  p.samples_count = 2;
  p.samples[0] = {5, 0, -123456, 0};
  p.samples[1] = {6, 2500, 42, 8};
  std::array<std::uint8_t, tlm::TlmPacket::kMaxSize> b{};
  const std::size_t n = tlm::encode(p, b);
  ASSERT_EQ(n, tlm::TlmPacket::kFixedSize + 2 * tlm::TlmPacket::kRecordSize);
  tlm::TlmPacket back{};
  ASSERT_TRUE(tlm::decode(std::span{b}.first(n), back));
  EXPECT_EQ(back.samples_count, 2);
  EXPECT_EQ(back.samples[0].value, -123456);
  EXPECT_EQ(back.samples[1].dt_us, 2500);
  EXPECT_EQ(back.samples[1].flags, 8);
  EXPECT_EQ(back.time_ns, p.time_ns);
}

TEST(Codec, WrongLengthsAreRejected) {
  IQK_VERIFIES("SRS-002");
  std::array<std::uint8_t, imu::ImuSample::kSize + 1> b{};
  imu::ImuSample s{};
  EXPECT_FALSE(imu::decode(std::span{b}, s));
  EXPECT_FALSE(imu::decode(std::span{b}.first(imu::ImuSample::kSize - 1), s));
  EXPECT_TRUE(imu::decode(std::span{b}.first(imu::ImuSample::kSize), s));
  aam::AamData d{};
  std::array<std::uint8_t, 8> c{};
  EXPECT_FALSE(aam::decode(std::span{c}.first(7), d));
  EXPECT_TRUE(aam::decode(std::span{c}, d));
}

TEST(Codec, ArrayCountMustMatchTheLength) {
  IQK_VERIFIES("SRS-002");
  tlm::TlmPacket p{};
  p.samples_count = 3;
  std::array<std::uint8_t, tlm::TlmPacket::kMaxSize> b{};
  const std::size_t n = tlm::encode(p, b);
  tlm::TlmPacket back{};
  EXPECT_TRUE(tlm::decode(std::span{b}.first(n), back));
  EXPECT_FALSE(tlm::decode(std::span{b}.first(n - 1), back));
  EXPECT_FALSE(tlm::decode(std::span{b}.first(n + tlm::TlmPacket::kRecordSize), back));
  b[tlm::TlmPacket::kFixedSize - 1] = 49;  // more records than the ICD allows
  EXPECT_FALSE(tlm::decode(std::span{b}.first(tlm::TlmPacket::kFixedSize + 49 * tlm::TlmPacket::kRecordSize), back));
}

TEST(Codec, RangesComeFromTheIcd) {
  IQK_VERIFIES("SRS-003");
  imu::ImuSample s{};
  EXPECT_TRUE(imu::in_range(s));
  s.gyro_x = 8'727'000;
  EXPECT_TRUE(imu::in_range(s));
  s.gyro_x = 8'727'001;
  EXPECT_FALSE(imu::in_range(s));
  s.gyro_x = 0;
  s.temp = -4001;
  EXPECT_FALSE(imu::in_range(s));
  gnss::TimePulse t{};
  t.tow_ms = 604'800'000;
  EXPECT_FALSE(gnss::in_range(t));
  t.tow_ms = 604'799'999;
  EXPECT_TRUE(gnss::in_range(t));
  aam::AamHeartbeat h{};
  h.state = 4;
  EXPECT_FALSE(aam::in_range(h));
}
