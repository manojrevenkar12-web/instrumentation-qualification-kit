#include <algorithm>

#include "support.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;

struct Got {
  std::uint8_t id;
  Bytes payload;
};

Bytes make(std::uint8_t id, const Bytes& p) {
  Bytes out(p.size() + iqk::kFrameOverhead);
  out.resize(iqk::encode_frame(id, p, out));
  return out;
}

Bytes cat(std::initializer_list<Bytes> parts) {
  Bytes r;
  for (const auto& p : parts) r.insert(r.end(), p.begin(), p.end());
  return r;
}

std::vector<Got> run(iqk::Deframer& d, const Bytes& in, std::size_t chunk = 0) {
  std::vector<Got> got;
  auto cb = [&](const iqk::Frame& f) { got.push_back({f.id, Bytes(f.data().begin(), f.data().end())}); };
  if (chunk == 0) chunk = in.size();
  for (std::size_t i = 0; i < in.size(); i += chunk) {
    d.feed(std::span<const std::uint8_t>(in).subspan(i, std::min(chunk, in.size() - i)), cb);
  }
  return got;
}

Bytes payload(std::uint8_t seed, std::size_t n) {
  Bytes p(n);
  for (std::size_t i = 0; i < n; ++i) p[i] = static_cast<std::uint8_t>(seed + i * 7);
  return p;
}

const Bytes kPa = payload(1, 12);
const Bytes kPb = payload(100, 20);
const Bytes kPc = payload(50, 5);

}  // namespace

TEST(Resync, ValidFramesBaseline) {
  IQK_VERIFIES("SRS-011");
  iqk::Deframer d{64};
  auto got = run(d, cat({make(1, kPa), make(2, kPb)}));
  ASSERT_EQ(got.size(), 2U);
  EXPECT_EQ(got[0].payload, kPa);
  EXPECT_EQ(got[1].id, 2);
  EXPECT_EQ(got[1].payload, kPb);
}

// Flip a byte of the first frame at every position, in several chunkings: the following frames must arrive and
// the corrupted one must never be delivered with altered content.
TEST(Resync, AnySingleByteCorruptionCostsAtMostThatFrame) {
  IQK_VERIFIES("SRS-011");
  const Bytes f1 = make(1, kPa), f2 = make(2, kPb), f3 = make(3, kPc);
  for (std::size_t chunk : {std::size_t{0}, std::size_t{1}, std::size_t{3}}) {
    for (std::size_t pos = 0; pos < f1.size(); ++pos) {
      for (std::uint8_t x : {std::uint8_t{0x01}, std::uint8_t{0xFF}, std::uint8_t{0x5A}}) {
        Bytes c = f1;
        c[pos] ^= x;
        iqk::Deframer d{64};
        auto got = run(d, cat({c, f2, f3}), chunk);
        ASSERT_GE(got.size(), 2U) << "pos " << pos << " x " << int(x) << " chunk " << chunk;
        EXPECT_EQ(got[got.size() - 2].id, 2) << "pos " << pos;
        EXPECT_EQ(got[got.size() - 2].payload, kPb);
        EXPECT_EQ(got.back().id, 3);
        EXPECT_EQ(got.back().payload, kPc);
        EXPECT_LE(got.size(), 3U);
        for (const auto& g : got) {
          if (g.id == 1) {
            EXPECT_EQ(g.payload, kPa);
          }
        }
      }
    }
  }
}

// A corrupted length byte that swallows the following frame must not cost that following frame.
TEST(Resync, InflatedLengthDoesNotSwallowNextFrame) {
  IQK_VERIFIES("SRS-011");
  const Bytes f2 = make(2, kPb), f3 = make(3, kPc);
  for (std::uint8_t badlen : {std::uint8_t{40}, std::uint8_t{60}, std::uint8_t{64}}) {
    Bytes c = make(1, kPa);
    c[3] = badlen;
    iqk::Deframer d{64};
    // Enough trailing traffic that the bogus length can be satisfied (the stream keeps flowing).
    auto got = run(d, cat({c, f2, f3, f3, f3, f3, f3, f3, f3, f3}));
    ASSERT_GE(got.size(), 3U) << int(badlen);
    EXPECT_EQ(got[0].id, 2) << int(badlen);
    EXPECT_EQ(got[0].payload, kPb);
    EXPECT_EQ(got[1].id, 3);
  }
}

// Length above max_len is corruption; the next frame still arrives.
TEST(Resync, LengthAboveMaxIsCorruption) {
  IQK_VERIFIES("SRS-011");
  Bytes c = make(1, kPa);
  c[3] = 200;
  iqk::Deframer d{64};
  auto got = run(d, cat({c, make(2, kPb)}));
  ASSERT_EQ(got.size(), 1U);
  EXPECT_EQ(got[0].id, 2);
  EXPECT_EQ(got[0].payload, kPb);
}

// Corrupt bytes containing sync words (partial headers, bad-CRC headers, lone 0xA5 right before the real sync).
TEST(Resync, SyncWordInsideCorruptBytes) {
  IQK_VERIFIES("SRS-011");
  const Bytes good = make(2, kPb);
  const std::vector<Bytes> junk = {
      {0xA5, 0x5A},
      {0xA5, 0x5A, 0x01},
      {0xA5, 0x5A, 0x01, 0x04},
      {0xA5, 0x5A, 0x01, 0x04, 0x00, 0x00},
      {0xA5, 0x5A, 0xA5, 0x5A, 0xA5, 0x5A},
      {0xA5},
      {0xA5, 0xA5},
      {0x5A, 0xA5, 0x5A, 0xA5},
      {0xA5, 0x5A, 0x01, 0xFF},
      {0xA5, 0x5A, 0x02, 0x30, 0xA5, 0x5A},
  };
  for (const auto& j : junk) {
    for (std::size_t chunk : {std::size_t{0}, std::size_t{1}}) {
      iqk::Deframer d{64};
      auto got = run(d, cat({j, good, good}), chunk);
      ASSERT_EQ(got.size(), 2U);
      EXPECT_EQ(got[0].id, 2);
      EXPECT_EQ(got[0].payload, kPb);
      EXPECT_EQ(got[1].payload, kPb);
    }
  }
}

// A corrupt frame whose payload embeds a complete valid frame, followed by a real frame.
TEST(Resync, CorruptFrameWrapsValidFrameAndNextFollows) {
  IQK_VERIFIES("SRS-011");
  const Bytes inner = make(5, kPc);
  Bytes outer = make(1, cat({inner, payload(9, 6)}));
  outer.back() ^= 0x10;  // break outer CRC
  const Bytes after = make(2, kPb);
  iqk::Deframer d{64};
  auto got = run(d, cat({outer, after}));
  ASSERT_FALSE(got.empty());
  EXPECT_EQ(got.back().id, 2);
  EXPECT_EQ(got.back().payload, kPb);
  for (const auto& g : got) {
    EXPECT_NE(g.id, 1);
  }
}

TEST(Resync, TruncatedFrameThenGoodFrame) {
  IQK_VERIFIES("SRS-011");
  const Bytes f1 = make(1, kPa);
  for (std::size_t keep = 2; keep < f1.size(); ++keep) {
    Bytes t(f1.begin(), f1.begin() + static_cast<std::ptrdiff_t>(keep));
    iqk::Deframer d{64};
    auto got = run(d, cat({t, make(2, kPb), make(3, kPc)}));
    ASSERT_GE(got.size(), 1U) << keep;
    EXPECT_EQ(got.back().id, 3) << keep;
  }
}

TEST(Resync, RepeatedCorruptionStatsAndRecovery) {
  IQK_VERIFIES("SRS-011");
  iqk::Deframer d{64};
  Bytes stream;
  for (int i = 0; i < 20; ++i) {
    Bytes f = make(1, payload(static_cast<std::uint8_t>(i), 10));
    if (i % 2 == 0) f[5] ^= 0x80;
    stream.insert(stream.end(), f.begin(), f.end());
  }
  auto got = run(d, stream, 7);
  ASSERT_EQ(got.size(), 10U);
  for (std::size_t k = 0; k < got.size(); ++k) {
    EXPECT_EQ(got[k].payload, payload(static_cast<std::uint8_t>(2 * k + 1), 10));
  }
  EXPECT_EQ(d.stats().frames, 10U);
  EXPECT_EQ(d.stats().crc_errors, 10U);
}
