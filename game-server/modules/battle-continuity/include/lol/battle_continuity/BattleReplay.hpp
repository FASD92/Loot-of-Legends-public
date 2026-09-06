#pragma once

#include <lol/battle/BattleLoadApi.hpp>
#include <lol/battle_continuity/RecordCodec.hpp>
#include <lol/battle_continuity/RecoveryStateCodec.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace lol::battle_continuity {

enum class ReplayErrorCode : std::uint8_t {
  EmptyJournal,
  CodecRejected,
  UnsupportedRuleset,
  InvalidStart,
  InitialCheckpointMissing,
  StateDecodeRejected,
  StateHashMismatch,
  InvalidCommand,
  UnknownCommandKind,
  CommandDecisionMismatch,
  NoMutationRecord,
  StateDivergence,
  CheckpointDivergence,
  TerminalReceiptMismatch,
  DuplicateTerminalReceipt,
};

struct ReplayError final {
  ReplayErrorCode code;
  std::size_t recordIndex;
  std::uint64_t recordSequence;
  std::uint64_t logicalTick;
  std::optional<Hash> expectedHash;
  std::optional<Hash> actualHash;
  // Populated for failures after a valid BattleStart has established the
  // identity.  Pre-start codec failures leave both fields unavailable.
  std::optional<BattleIdentity> identity;
  std::optional<std::uint32_t> rulesetVersion;
};

struct ReplayResult final {
  std::optional<battle::BattleInstance> battle;
  std::optional<Hash> finalStateHash;
  std::optional<RoomRecoveryState> finalRoomRecoveryState;
  std::optional<ReplayError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

// Replays the encoded, committed journal.  No durable writer is involved.
class BattleReplayer final {
public:
  [[nodiscard]] static ReplayResult
  replayJournal(std::span<const std::uint8_t> encodedJournal);

  // Validates the complete committed journal, then restores from its last
  // checkpoint and applies only the command decisions after that checkpoint.
  [[nodiscard]] static ReplayResult
  restoreJournal(std::span<const std::uint8_t> encodedJournal);

  [[nodiscard]] static ReplayResult
  replayRecords(std::span<const Record> records);
};

} // namespace lol::battle_continuity
