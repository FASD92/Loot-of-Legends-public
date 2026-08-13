#include "JournalCodec.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace lol::settlement_storage::detail {
namespace {

constexpr std::array<std::uint32_t, 64> kSha256RoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

constexpr std::array<std::uint32_t, 8> kSha256InitialHash = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

constexpr std::uint32_t rotateRight(std::uint32_t value, unsigned shift) {
  return (value >> shift) | (value << (32u - shift));
}

void appendU32(std::vector<std::uint8_t> &out, std::uint32_t value) {
  for (std::int32_t shift = 24; shift >= 0; shift -= 8) {
    out.push_back(
        static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
  }
}

} // namespace

std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> bytes) {
  if (bytes.size() > std::numeric_limits<std::uint64_t>::max() / 8u) {
    return {};
  }

  const auto bitLength = static_cast<std::uint64_t>(bytes.size()) * 8u;
  std::vector<std::uint8_t> padded(bytes.begin(), bytes.end());
  padded.push_back(0x80u);
  while ((padded.size() % 64u) != 56u) {
    padded.push_back(0u);
  }
  appendU64(padded, bitLength);

  auto hash = kSha256InitialHash;
  for (std::size_t offset = 0; offset < padded.size(); offset += 64u) {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16u; ++index) {
      const auto base = offset + index * 4u;
      words[index] = (static_cast<std::uint32_t>(padded[base]) << 24u) |
                     (static_cast<std::uint32_t>(padded[base + 1u]) << 16u) |
                     (static_cast<std::uint32_t>(padded[base + 2u]) << 8u) |
                     static_cast<std::uint32_t>(padded[base + 3u]);
    }
    for (std::size_t index = 16u; index < words.size(); ++index) {
      const auto sigma0 = rotateRight(words[index - 15u], 7u) ^
                          rotateRight(words[index - 15u], 18u) ^
                          (words[index - 15u] >> 3u);
      const auto sigma1 = rotateRight(words[index - 2u], 17u) ^
                          rotateRight(words[index - 2u], 19u) ^
                          (words[index - 2u] >> 10u);
      words[index] = words[index - 16u] + sigma0 + words[index - 7u] + sigma1;
    }

    auto a = hash[0];
    auto b = hash[1];
    auto c = hash[2];
    auto d = hash[3];
    auto e = hash[4];
    auto f = hash[5];
    auto g = hash[6];
    auto h = hash[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const auto bigSigma1 =
          rotateRight(e, 6u) ^ rotateRight(e, 11u) ^ rotateRight(e, 25u);
      const auto choose = (e & f) ^ ((~e) & g);
      const auto temp1 =
          h + bigSigma1 + choose + kSha256RoundConstants[index] + words[index];
      const auto bigSigma0 =
          rotateRight(a, 2u) ^ rotateRight(a, 13u) ^ rotateRight(a, 22u);
      const auto majority = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = bigSigma0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    hash[0] += a;
    hash[1] += b;
    hash[2] += c;
    hash[3] += d;
    hash[4] += e;
    hash[5] += f;
    hash[6] += g;
    hash[7] += h;
  }

  std::array<std::uint8_t, 32> digest{};
  for (std::size_t index = 0; index < hash.size(); ++index) {
    digest[index * 4u] = static_cast<std::uint8_t>(hash[index] >> 24u);
    digest[index * 4u + 1u] = static_cast<std::uint8_t>(hash[index] >> 16u);
    digest[index * 4u + 2u] = static_cast<std::uint8_t>(hash[index] >> 8u);
    digest[index * 4u + 3u] = static_cast<std::uint8_t>(hash[index]);
  }
  return digest;
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
  std::uint32_t crc = UINT32_C(0xFFFFFFFF);
  for (const auto byte : bytes) {
    crc ^= byte;
    for (unsigned bit = 0; bit < 8u; ++bit) {
      const auto mask = static_cast<std::uint32_t>(
          -static_cast<std::int32_t>(crc & UINT32_C(1)));
      crc = (crc >> 1u) ^ (UINT32_C(0xEDB88320) & mask);
    }
  }
  return crc ^ UINT32_C(0xFFFFFFFF);
}

std::vector<std::uint8_t> encodeRecord(JournalRecordType type,
                                       std::uint64_t sequence,
                                       std::span<const std::uint8_t> payload) {
  if (sequence == 0u ||
      payload.size() >
          std::numeric_limits<std::uint32_t>::max() - kJournalRecordOverhead) {
    return {};
  }

  std::vector<std::uint8_t> record;
  record.reserve(kJournalRecordOverhead + payload.size());
  appendU32(record, kJournalMagic);
  appendU16(record, kJournalVersion);
  appendU16(record, static_cast<std::uint16_t>(type));
  appendU32(record, static_cast<std::uint32_t>(kJournalRecordOverhead +
                                               payload.size()));
  appendU64(record, sequence);
  appendU32(record, static_cast<std::uint32_t>(payload.size()));
  const auto payloadHash = sha256(payload);
  record.insert(record.end(), payloadHash.begin(), payloadHash.end());
  record.insert(record.end(), payload.begin(), payload.end());
  appendU32(record, crc32(record));
  return record;
}

void appendU16(std::vector<std::uint8_t> &out, std::uint16_t value) {
  out.push_back(static_cast<std::uint8_t>(value >> 8u));
  out.push_back(static_cast<std::uint8_t>(value));
}

void appendU64(std::vector<std::uint8_t> &out, std::uint64_t value) {
  for (std::int32_t shift = 56; shift >= 0; shift -= 8) {
    out.push_back(
        static_cast<std::uint8_t>(value >> static_cast<unsigned>(shift)));
  }
}

} // namespace lol::settlement_storage::detail
