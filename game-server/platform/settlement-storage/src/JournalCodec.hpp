#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace lol::settlement_storage::detail {

constexpr std::uint32_t kJournalMagic = UINT32_C(0x4C4F4F32);
constexpr std::uint16_t kJournalVersion = 1u;
constexpr std::size_t kJournalRecordOverhead = 60u;

enum class JournalRecordType : std::uint16_t {
  Intent = 1,
  BatchCommit = 2,
  Retired = 3,
};

std::array<std::uint8_t, 32> sha256(std::span<const std::uint8_t> bytes);
std::uint32_t crc32(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> encodeRecord(JournalRecordType type,
                                       std::uint64_t sequence,
                                       std::span<const std::uint8_t> payload);

void appendU16(std::vector<std::uint8_t> &out, std::uint16_t value);
void appendU64(std::vector<std::uint8_t> &out, std::uint64_t value);

} // namespace lol::settlement_storage::detail
