#include <lol/settlement_storage/JournalRecovery.hpp>

#include "JournalCodec.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace lol::settlement_storage {
namespace {

using detail::JournalRecordType;

std::uint16_t readU16(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(bytes[offset]) << 8u) |
      static_cast<std::uint16_t>(bytes[offset + 1u]));
}

std::uint32_t readU32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return (static_cast<std::uint32_t>(bytes[offset]) << 24u) |
         (static_cast<std::uint32_t>(bytes[offset + 1u]) << 16u) |
         (static_cast<std::uint32_t>(bytes[offset + 2u]) << 8u) |
         static_cast<std::uint32_t>(bytes[offset + 3u]);
}

std::uint64_t readU64(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint64_t value{};
  for (std::size_t index = 0; index < 8u; ++index) {
    value = (value << 8u) | bytes[offset + index];
  }
  return value;
}

struct PendingIntent {
  std::uint64_t sequence{};
  std::array<std::uint8_t, 32> payloadHash{};
  std::vector<std::uint8_t> payload;
};

JournalRecoveryResult quarantine(JournalRecoveryStatus status,
                                 std::size_t offset,
                                 std::uint64_t lastDurableSequence,
                                 std::vector<RecoveredBatch> batches) {
  return {
      .status = status,
      .durableBytes = offset,
      .quarantineOffset = offset,
      .lastSequence = lastDurableSequence,
      .batches = std::move(batches),
  };
}

std::optional<JournalBatchId>
batchIdFrom(std::span<const std::uint8_t> payload) {
  if (payload.size() < JournalBatchId{}.size()) {
    return std::nullopt;
  }
  JournalBatchId bytes{};
  std::copy_n(payload.begin(), bytes.size(), bytes.begin());
  if (std::none_of(bytes.begin(), bytes.end(),
                   [](std::uint8_t value) { return value != 0u; })) {
    return std::nullopt;
  }
  return bytes;
}

} // namespace

JournalRecoveryResult recoverJournal(std::span<const std::uint8_t> bytes) {
  std::vector<RecoveredBatch> batches;
  std::vector<PendingIntent> pending;
  std::optional<std::size_t> pendingOffset;
  std::size_t offset{};
  std::size_t durableBytes{};
  std::uint64_t lastParsedSequence{};
  std::uint64_t lastDurableSequence{};

  while (offset < bytes.size()) {
    const auto remaining = bytes.size() - offset;
    if (remaining < detail::kJournalRecordOverhead) {
      const auto quarantineOffset = pendingOffset.value_or(offset);
      return quarantine(JournalRecoveryStatus::IncompleteTailQuarantined,
                        quarantineOffset, lastDurableSequence,
                        std::move(batches));
    }

    const auto magic = readU32(bytes, offset);
    const auto version = readU16(bytes, offset + 4u);
    const auto rawType = readU16(bytes, offset + 6u);
    const auto recordLength = readU32(bytes, offset + 8u);
    const auto sequence = readU64(bytes, offset + 12u);
    const auto payloadLength = readU32(bytes, offset + 20u);
    const auto typeValid =
        rawType >= static_cast<std::uint16_t>(JournalRecordType::Intent) &&
        rawType <= static_cast<std::uint16_t>(JournalRecordType::Retired);
    if (magic != detail::kJournalMagic || version != detail::kJournalVersion ||
        !typeValid || sequence == 0u || sequence <= lastParsedSequence ||
        recordLength < detail::kJournalRecordOverhead ||
        payloadLength != recordLength - detail::kJournalRecordOverhead) {
      const auto quarantineOffset = pendingOffset.value_or(offset);
      return quarantine(JournalRecoveryStatus::CorruptTailQuarantined,
                        quarantineOffset, lastDurableSequence,
                        std::move(batches));
    }
    if (recordLength > remaining) {
      const auto quarantineOffset = pendingOffset.value_or(offset);
      return quarantine(JournalRecoveryStatus::IncompleteTailQuarantined,
                        quarantineOffset, lastDurableSequence,
                        std::move(batches));
    }

    const auto record = bytes.subspan(offset, recordLength);
    const auto payload = record.subspan(56u, payloadLength);
    const auto storedHash = record.subspan(24u, 32u);
    const auto computedHash = detail::sha256(payload);
    const auto storedCrc = readU32(record, recordLength - 4u);
    if (!std::equal(storedHash.begin(), storedHash.end(),
                    computedHash.begin()) ||
        storedCrc != detail::crc32(record.first(recordLength - 4u))) {
      const auto quarantineOffset = pendingOffset.value_or(offset);
      return quarantine(JournalRecoveryStatus::CorruptTailQuarantined,
                        quarantineOffset, lastDurableSequence,
                        std::move(batches));
    }

    const auto type = static_cast<JournalRecordType>(rawType);
    if (type == JournalRecordType::Intent) {
      if (payload.empty() || pending.size() == 10u) {
        const auto quarantineOffset = pendingOffset.value_or(offset);
        return quarantine(JournalRecoveryStatus::CorruptTailQuarantined,
                          quarantineOffset, lastDurableSequence,
                          std::move(batches));
      }
      if (!pendingOffset.has_value()) {
        pendingOffset = offset;
      }
      pending.push_back({
          .sequence = sequence,
          .payloadHash = computedHash,
          .payload = std::vector<std::uint8_t>(payload.begin(), payload.end()),
      });
    } else if (type == JournalRecordType::BatchCommit) {
      const auto expectedSize = 18u + pending.size() * 40u;
      const auto batchId = batchIdFrom(payload);
      const auto count = payload.size() >= 18u ? readU16(payload, 16u) : 0u;
      bool matches = pending.size() >= 2u && batchId.has_value() &&
                     payload.size() == expectedSize && count == pending.size();
      for (std::size_t index = 0; matches && index < pending.size(); ++index) {
        const auto tupleOffset = 18u + index * 40u;
        matches = readU64(payload, tupleOffset) == pending[index].sequence &&
                  std::equal(payload.begin() +
                                 static_cast<std::ptrdiff_t>(tupleOffset + 8u),
                             payload.begin() +
                                 static_cast<std::ptrdiff_t>(tupleOffset + 40u),
                             pending[index].payloadHash.begin());
      }
      if (!matches ||
          std::any_of(batches.begin(), batches.end(), [&](const auto &batch) {
            return batch.batchId == *batchId;
          })) {
        return quarantine(JournalRecoveryStatus::CorruptTailQuarantined,
                          pendingOffset.value_or(offset), lastDurableSequence,
                          std::move(batches));
      }

      std::vector<std::vector<std::uint8_t>> intents;
      intents.reserve(pending.size());
      for (auto &intent : pending) {
        intents.push_back(std::move(intent.payload));
      }
      batches.push_back({
          .batchId = *batchId,
          .canonicalIntents = std::move(intents),
          .commitSequence = sequence,
          .retired = false,
      });
      pending.clear();
      pendingOffset.reset();
      durableBytes = offset + recordLength;
      lastDurableSequence = sequence;
    } else {
      const auto batchId = batchIdFrom(payload);
      if (!pending.empty() || payload.size() != 24u || !batchId.has_value()) {
        return quarantine(JournalRecoveryStatus::CorruptTailQuarantined,
                          pendingOffset.value_or(offset), lastDurableSequence,
                          std::move(batches));
      }
      const auto commitSequence = readU64(payload, 16u);
      const auto batch = std::find_if(
          batches.begin(), batches.end(), [&](const auto &candidate) {
            return candidate.batchId == *batchId &&
                   candidate.commitSequence == commitSequence &&
                   !candidate.retired;
          });
      if (batch == batches.end()) {
        return quarantine(JournalRecoveryStatus::CorruptTailQuarantined, offset,
                          lastDurableSequence, std::move(batches));
      }
      batch->retired = true;
      durableBytes = offset + recordLength;
      lastDurableSequence = sequence;
    }

    lastParsedSequence = sequence;
    offset += recordLength;
  }

  if (!pending.empty()) {
    return quarantine(JournalRecoveryStatus::IncompleteTailQuarantined,
                      pendingOffset.value(), lastDurableSequence,
                      std::move(batches));
  }
  return {
      .status = JournalRecoveryStatus::Clean,
      .durableBytes = durableBytes,
      .quarantineOffset = std::nullopt,
      .lastSequence = lastDurableSequence,
      .batches = std::move(batches),
  };
}

} // namespace lol::settlement_storage
