#include <gtest/gtest.h>

#include "iqk/icd/ptp.hpp"
#include "iqk/timesync.hpp"
#include "support.hpp"

namespace ptp = iqk::icd::ptp;
using iqk::timesync::PtpSlave;
using iqk::timesync::TimeSync;
using iqk_test::kMs;
using iqk_test::kS;

namespace {
constexpr std::uint64_t kMe = 0x0A0B0C0D0E0F1011ULL;
constexpr std::int64_t kUtc = 1'790'000'000LL * kS;

template <class M>
std::vector<std::uint8_t> bytes(const M& m) {
  std::vector<std::uint8_t> b(M::kSize);
  (void)ptp::encode(m, b);
  return b;
}
template <class M>
M header(std::uint8_t type, std::uint16_t seq) {
  M m{};
  m.type_sdo = type;
  m.version = 2;
  m.length = static_cast<std::uint16_t>(M::kSize);
  m.sequence_id = seq;
  m.clock_identity = 0x00163EFFFE4D5A01ULL;
  return m;
}
std::vector<std::uint8_t> sync(std::uint16_t seq) { return bytes(header<ptp::PtpSync>(0x0, seq)); }
std::vector<std::uint8_t> follow_up(std::uint16_t seq, std::int64_t t1) {
  auto m = header<ptp::PtpFollowUp>(0x8, seq);
  m.origin_s = static_cast<std::uint64_t>(t1 / kS);
  m.origin_ns = static_cast<std::uint32_t>(t1 % kS);
  return bytes(m);
}
std::vector<std::uint8_t> delay_resp(std::uint16_t seq, std::int64_t t4, std::uint64_t requester = kMe) {
  auto m = header<ptp::PtpDelayResp>(0x9, seq);
  m.receive_s = static_cast<std::uint64_t>(t4 / kS);
  m.receive_ns = static_cast<std::uint32_t>(t4 % kS);
  m.req_clock_identity = requester;
  m.req_port_number = 1;
  return bytes(m);
}
}  // namespace

TEST(Ptp, AnExchangeProducesADelayRequestAndAMeasurement) {
  IQK_VERIFIES("SRS-060");
  PtpSlave s{kMe, 0};
  TimeSync ts;
  std::array<std::uint8_t, 64> out{};
  // The master is kUtc - 5 s ahead of the local clock; 100 us each way. t1 kUtc, t2 local 5 s + 100 us,
  // t3 local 5 s + 400 us, t4 kUtc + 500 us.
  EXPECT_EQ(s.on_datagram(sync(7), 5 * kS + 100'000, ts, out), 0U);
  const std::size_t n = s.on_datagram(follow_up(7, kUtc), 5 * kS + 300'000, ts, out);
  ASSERT_EQ(n, ptp::PtpDelayReq::kSize);
  ptp::PtpDelayReq req{};
  ASSERT_TRUE(ptp::decode(std::span{out}.first(n), req));
  EXPECT_EQ(req.type_sdo & 0x0F, 0x1);
  EXPECT_EQ(req.clock_identity, kMe);
  s.on_sent(5 * kS + 400'000);
  EXPECT_FALSE(ts.has_time());
  EXPECT_EQ(s.on_datagram(delay_resp(req.sequence_id, kUtc + 500'000), 5 * kS + 600'000, ts, out), 0U);
  ASSERT_TRUE(ts.has_time());
  // offset: master = local + (kUtc - 5 s); delay 100 us: at local t2 (5 s + 100 us) master time is kUtc + 100 us.
  EXPECT_EQ(ts.utc_ns(5 * kS + 100'000), kUtc + 100'000);
}

TEST(Ptp, MismatchedSequencesAndForeignAnswersAreRejected) {
  IQK_VERIFIES("SRS-060");
  PtpSlave s{kMe, 0};
  TimeSync ts;
  std::array<std::uint8_t, 64> out{};
  (void)s.on_datagram(sync(7), 5 * kS, ts, out);
  EXPECT_EQ(s.on_datagram(follow_up(8, kUtc), 5 * kS, ts, out), 0U);  // follow-up for another sync
  EXPECT_EQ(s.rejected(), 1U);
  (void)s.on_datagram(sync(9), 5 * kS, ts, out);
  const std::size_t n = s.on_datagram(follow_up(9, kUtc), 5 * kS, ts, out);
  ASSERT_GT(n, 0U);
  ptp::PtpDelayReq req{};
  ASSERT_TRUE(ptp::decode(std::span{out}.first(n), req));
  s.on_sent(5 * kS + 100);
  (void)s.on_datagram(delay_resp(req.sequence_id, kUtc, kMe + 1), 5 * kS, ts, out);  // answer to someone else
  (void)s.on_datagram(delay_resp(static_cast<std::uint16_t>(req.sequence_id + 1), kUtc), 5 * kS, ts, out);
  EXPECT_FALSE(ts.has_time());
  EXPECT_EQ(s.rejected(), 3U);
}

TEST(Ptp, OtherDomainsAndVersionsAreIgnored) {
  IQK_VERIFIES("SRS-060");
  PtpSlave s{kMe, 0};
  TimeSync ts;
  std::array<std::uint8_t, 64> out{};
  auto other_domain = header<ptp::PtpSync>(0x0, 1);
  other_domain.domain = 4;
  (void)s.on_datagram(bytes(other_domain), kS, ts, out);
  auto v1 = header<ptp::PtpSync>(0x0, 1);
  v1.version = 1;
  (void)s.on_datagram(bytes(v1), kS, ts, out);
  EXPECT_EQ(s.rejected(), 2U);
  EXPECT_EQ(s.on_datagram(follow_up(1, kUtc), kS, ts, out), 0U);  // no accepted sync before it
}
