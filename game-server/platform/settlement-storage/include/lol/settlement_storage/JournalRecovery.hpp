#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace lol::settlement_storage {

using JournalBatchId = std::array<std::uint8_t, 16>;

enum class JournalRecoveryStatus {
  Clean,
  IncompleteTailQuarantined,
  CorruptTailQuarantined,
};

struct RecoveredBatch {
  JournalBatchId batchId;
  std::vector<std::vector<std::uint8_t>> canonicalIntents;
  std::uint64_t commitSequence{};
  bool retired{};
};

struct JournalRecoveryResult {
  JournalRecoveryStatus status{JournalRecoveryStatus::Clean};
  std::size_t durableBytes{};
  std::optional<std::size_t> quarantineOffset;
  std::uint64_t lastSequence{};
  std::vector<RecoveredBatch> batches;
};

JournalRecoveryResult recoverJournal(std::span<const std::uint8_t> bytes);

} // namespace lol::settlement_storage
