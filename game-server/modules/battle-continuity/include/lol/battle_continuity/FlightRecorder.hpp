#pragma once

#include <lol/battle/BattleLoadApi.hpp>
#include <lol/battle_continuity/BattleStateCodec.hpp>
#include <lol/battle_continuity/RecordCodec.hpp>
#include <lol/battle_continuity/RecoveryStateCodec.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace lol::battle_continuity {

// These are the only domain decisions that may cross the Battle continuity
// boundary.  A transport packet is never a canonical command.
enum class CanonicalCommandKind : std::uint16_t {
  ArenaLoadComplete = 1U,
  SuspendInput = 2U,
  ResumeInput = 3U,
  ParticipantExit = 4U,
  LoadBarrierDeadline = 5U,
  Move = 6U,
  MovementTick = 7U,
  Attack = 8U,
  CombatDeadline = 9U,
  ClaimLoot = 10U,
  LootDeadline = 11U,
};

// Canonical payloads contain only normalized domain fields.  The factories
// below intentionally do not accept credentials, generation, account data,
// or display names.
struct CanonicalCommand final {
  // Server-originated decisions reserve slot 0.  Commands that target a
  // participant carry that target in their fixed payload instead.
  CanonicalCommandKind kind;
  std::uint16_t participantSlot;
  CommandId commandId;
  Bytes payload;

  [[nodiscard]] static CanonicalCommand
  arenaLoadComplete(std::uint16_t targetParticipantSlot);
  [[nodiscard]] static CanonicalCommand
  suspendInput(std::uint16_t targetParticipantSlot);
  [[nodiscard]] static CanonicalCommand
  resumeInput(std::uint16_t targetParticipantSlot);
  [[nodiscard]] static CanonicalCommand
  participantExit(std::uint16_t targetParticipantSlot, bool voluntary);
  [[nodiscard]] static CanonicalCommand loadBarrierDeadline();
  [[nodiscard]] static CanonicalCommand move(std::uint16_t participantSlot,
                                             CommandId commandId,
                                             battle::DirectionIntent direction);
  [[nodiscard]] static CanonicalCommand movementTick(std::uint32_t serverTick);
  [[nodiscard]] static CanonicalCommand attack(std::uint16_t participantSlot,
                                               CommandId commandId,
                                               std::uint64_t targetHint);
  [[nodiscard]] static CanonicalCommand combatDeadline();
  [[nodiscard]] static CanonicalCommand claimLoot(std::uint16_t participantSlot,
                                                  CommandId commandId,
                                                  std::uint64_t dropId);
  [[nodiscard]] static CanonicalCommand lootDeadline();
};

enum class RecorderErrorCode : std::uint8_t {
  InvalidIdentity,
  UnsupportedRuleset,
  InvalidCommand,
  StateEncodingFailed,
  SequenceExhausted,
  BatchEmpty,
  BatchNotCommitted,
  CodecRejected,
  TerminalAlreadyRecorded,
  TerminalNotReady,
};

struct RecorderError final {
  RecorderErrorCode code;
  std::uint64_t recordSequence;
};

struct RecorderAppendResult final {
  bool recorded{false};
  std::optional<Record> record;
  std::optional<RecorderError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

struct JournalEncodeResult final {
  Bytes bytes;
  std::optional<RecorderError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

// In-memory semantic recorder.  Durable storage belongs to the continuity
// platform layer; this type owns neither a file descriptor nor a worker.
class FlightRecorder final {
public:
  [[nodiscard]] static std::optional<FlightRecorder>
  start(const battle::BattleInstance &battle, BattleIdentity identity,
        std::uint32_t writerRecoveryEpoch,
        const RoomRecoveryState &roomState);

  FlightRecorder(FlightRecorder &&) noexcept = default;
  FlightRecorder &operator=(FlightRecorder &&) noexcept = default;
  FlightRecorder(const FlightRecorder &) = delete;
  FlightRecorder &operator=(const FlightRecorder &) = delete;
  ~FlightRecorder() = default;

  [[nodiscard]] RecorderAppendResult
  appendCommand(const CanonicalCommand &command,
                const battle::BattleDeterministicState &before,
                const RoomRecoveryState &beforeRoom,
                const battle::BattleInstance &after,
                const RoomRecoveryState &afterRoom,
                std::uint16_t decisionCode, Bytes outcomePayload);

  [[nodiscard]] RecorderAppendResult
  appendCheckpoint(const battle::BattleInstance &battle,
                   const RoomRecoveryState &roomState);

  [[nodiscard]] RecorderAppendResult
  appendTerminal(std::uint16_t terminalReason,
                 const battle::BattleInstance &battle,
                 const RoomRecoveryState &roomState,
                 std::uint64_t resultCommittedUnixEpochMilliseconds,
                 SettlementBatchId settlementBatchId,
                 std::vector<SettlementReceipt> settlements);

  // Closes the current logical-tick batch with a TickCommit record.  This is
  // still an in-memory operation; the durable writer decides when to encode
  // and fsync the returned journal bytes.
  [[nodiscard]] RecorderAppendResult commitTick();

  [[nodiscard]] JournalEncodeResult encodeJournal() const;
  [[nodiscard]] const std::vector<Record> &records() const noexcept;
  [[nodiscard]] const BattleIdentity &identity() const noexcept;
  [[nodiscard]] std::uint32_t writerRecoveryEpoch() const noexcept;

private:
  friend class BattleRecording;

  FlightRecorder(BattleIdentity identity, std::uint32_t writerRecoveryEpoch,
                 std::vector<Record> records,
                 std::optional<Hash> batchStateHash,
                 std::uint64_t batchFirstSequence,
                 std::uint32_t batchRecordCount, std::uint64_t batchLastTick,
                 std::uint64_t nextSequence, bool terminalRecorded) noexcept;

  [[nodiscard]] RecorderAppendResult appendDataRecord(Record record,
                                                      Hash stateHash);
  [[nodiscard]] RecorderAppendResult failure(RecorderErrorCode code) const;
  [[nodiscard]] bool
  identityMatches(const battle::BattleDeterministicState &state) const noexcept;

  BattleIdentity identity_;
  std::uint32_t writerRecoveryEpoch_;
  std::vector<Record> records_;
  std::optional<Hash> batchStateHash_;
  std::uint64_t batchFirstSequence_;
  std::uint32_t batchRecordCount_;
  std::uint64_t batchLastTick_;
  std::uint64_t nextSequence_;
  bool terminalRecorded_;
};

struct TerminalRecording final {
  std::uint16_t terminalReason;
  std::uint64_t resultCommittedUnixEpochMilliseconds;
  SettlementBatchId settlementBatchId;
  std::vector<SettlementReceipt> settlements;
};

struct RecordedTickBatch final {
  BattleIdentity identity;
  std::uint32_t writerRecoveryEpoch;
  std::uint64_t firstRecordSequence;
  std::uint64_t lastRecordSequence;
  std::uint64_t logicalTick;
  bool terminal;
  Bytes encodedRecords;
};

// Owns the production checkpoint policy around the semantic recorder.  The
// caller remains the single Battle writer and supplies each admitted mutation.
class BattleRecording final {
public:
  [[nodiscard]] static std::optional<BattleRecording>
  start(const battle::BattleInstance &battle, BattleIdentity identity,
        std::uint32_t writerRecoveryEpoch,
        const RoomRecoveryState &roomState);
  [[nodiscard]] static std::optional<BattleRecording>
  resume(const battle::BattleInstance &battle,
         std::vector<Record> committedRecords,
         std::uint32_t writerRecoveryEpoch);

  BattleRecording(BattleRecording &&) noexcept = default;
  BattleRecording &operator=(BattleRecording &&) noexcept = default;
  BattleRecording(const BattleRecording &) = delete;
  BattleRecording &operator=(const BattleRecording &) = delete;
  ~BattleRecording() = default;

  [[nodiscard]] bool
  recordDecision(const CanonicalCommand &command,
                 const battle::BattleDeterministicState &before,
                 const RoomRecoveryState &beforeRoom,
                 const battle::BattleInstance &after,
                 const RoomRecoveryState &afterRoom,
                 std::uint16_t decisionCode,
                 std::optional<TerminalRecording> terminal);

  [[nodiscard]] std::optional<RecordedTickBatch> takePendingBatch();
  [[nodiscard]] JournalEncodeResult encodeJournal() const;
  [[nodiscard]] const std::vector<Record> &records() const noexcept;
  [[nodiscard]] const std::optional<RoomRecoveryState> &
  roomRecoveryState() const noexcept;

private:
  BattleRecording(FlightRecorder recorder, std::uint64_t lastCheckpointTick,
                  std::optional<RecordedTickBatch> pendingBatch,
                  std::optional<RoomRecoveryState> roomState) noexcept;
  [[nodiscard]] bool capturePendingBatch(bool terminal);

  FlightRecorder recorder_;
  std::uint64_t lastCheckpointTick_;
  std::optional<RecordedTickBatch> pendingBatch_;
  std::optional<RoomRecoveryState> roomState_;
};

} // namespace lol::battle_continuity
