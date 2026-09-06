#pragma once

#include <lol/battle/BattleTime.hpp>
#include <lol/shared/Identifiers.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace lol::battle_continuity {

inline constexpr std::uint32_t kRecordMagic = 0x4C424331U; // LBC1
inline constexpr std::uint16_t kRecordSchemaVersion = 1U;
inline constexpr std::uint16_t kCheckpointSchemaVersion = 1U;
inline constexpr std::uint32_t kTickHertz = 20U;
inline constexpr std::uint64_t kTickNanos = battle::BattleTime::tickNanos;
static_assert(kTickHertz * kTickNanos == 1'000'000'000ULL);
inline constexpr std::uint32_t kSupportedBattleRulesetVersion = 1U;
inline constexpr std::size_t kRecordEnvelopeBytes = 68U;
inline constexpr std::size_t kMaximumRecordBytes = 1U << 20U;
inline constexpr std::size_t kMaximumJournalBytes = 64U << 20U;

using Bytes = std::vector<std::uint8_t>;
using Hash = std::array<std::uint8_t, 32>;
using SettlementId = std::array<std::uint8_t, 16>;
using SettlementBatchId = std::array<std::uint8_t, 16>;
struct CommandId final {
  std::uint64_t high;
  std::uint64_t low;

  bool operator==(const CommandId &) const = default;
};

enum class RecordType : std::uint16_t {
  BattleStart = 1U,
  CommandDecision = 2U,
  Checkpoint = 3U,
  TerminalReceipt = 4U,
  TickCommit = 5U,
};

struct BattleIdentity final {
  std::uint32_t originRecoveryEpoch;
  shared::RoomId roomId;
  shared::BattleInstanceId battleInstanceId;

  bool operator==(const BattleIdentity &) const = default;
};

struct RecordHeader final {
  RecordType recordType;
  std::uint64_t recordSequence;
  std::uint32_t originRecoveryEpoch;
  std::uint32_t writerRecoveryEpoch;
  shared::RoomId roomId;
  shared::BattleInstanceId battleInstanceId;
  std::uint64_t logicalTick;
  std::uint64_t battleElapsedNanos;

  bool operator==(const RecordHeader &) const = default;
};

struct BattleStartParticipant final {
  std::uint16_t participantSlot;
  shared::SessionId sessionId;

  bool operator==(const BattleStartParticipant &) const = default;
};

struct BattleStartPayload final {
  std::uint32_t rulesetVersion;
  std::uint64_t battleSeed;
  std::uint32_t tickHertz;
  Hash initialStateHash;
  std::vector<BattleStartParticipant> participants;

  bool operator==(const BattleStartPayload &) const = default;
};

struct CommandDecisionPayload final {
  std::uint16_t participantSlot;
  std::uint16_t commandKind;
  CommandId commandId;
  std::uint16_t decisionCode;
  Bytes commandPayload;
  Bytes outcomePayload;
  // Canonical, privacy-safe room projection associated with the resulting
  // state.  It is deliberately distinct from outcomePayload.
  Bytes roomRecoveryStateBytes;
  Hash postDecisionStateHash;

  bool operator==(const CommandDecisionPayload &) const = default;
};

struct CheckpointPayload final {
  std::uint16_t checkpointSchemaVersion;
  Bytes canonicalStateBytes;
  Hash stateHash;

  bool operator==(const CheckpointPayload &) const = default;
};

struct SettlementReceipt final {
  std::uint16_t participantSlot;
  SettlementId settlementId;
  Hash settlementPayloadHash;

  bool operator==(const SettlementReceipt &) const = default;
};

struct TerminalReceiptPayload final {
  std::uint16_t terminalReason;
  Hash finalStateHash;
  Bytes canonicalResultPayload;
  std::uint64_t resultCommittedUnixEpochMilliseconds;
  std::uint64_t resultCommittedBattleElapsedNanos;
  SettlementBatchId settlementBatchId;
  std::uint16_t settlementIntentCount;
  std::vector<SettlementReceipt> settlements;

  bool operator==(const TerminalReceiptPayload &) const = default;
};

struct TickCommitPayload final {
  std::uint64_t firstRecordSequence;
  std::uint64_t lastDataRecordSequence;
  std::uint32_t recordCount;
  Hash committedStateHash;

  bool operator==(const TickCommitPayload &) const = default;
};

using RecordPayload =
    std::variant<BattleStartPayload, CommandDecisionPayload, CheckpointPayload,
                 TerminalReceiptPayload, TickCommitPayload>;

struct Record final {
  RecordHeader header;
  RecordPayload payload;

  bool operator==(const Record &) const = default;
};

enum class CodecErrorCode : std::uint8_t {
  Truncated,
  TrailingBytes,
  InvalidMagic,
  UnsupportedSchemaVersion,
  UnsupportedRecordType,
  InvalidLength,
  RecordTooLarge,
  JournalTooLarge,
  ChecksumMismatch,
  InvalidIdentity,
  InvalidLogicalTime,
  InvalidPayload,
  UnsupportedRulesetVersion,
  InvalidSequence,
  IdentityMismatch,
  CheckpointHashMismatch,
  HashUnavailable,
  BatchInvariantViolation,
  WriterEpochRegression,
  WriterEpochPerBatch,
  JournalNotCommitted,
};

struct CodecError final {
  CodecErrorCode code;
  std::size_t offset;
};

struct EncodeResult final {
  Bytes bytes;
  std::optional<CodecError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

struct RecordDecodeResult final {
  std::optional<Record> record;
  std::optional<CodecError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

struct JournalDecodeResult final {
  std::vector<Record> records;
  // Byte offset through the last fully committed TickCommit.  A malformed,
  // torn, or open tail is never exposed through records; callers may truncate
  // the file safely at this boundary.
  std::size_t committedBytes{0U};
  std::optional<CodecError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

struct HashObservation final {
  std::uint64_t recordSequence;
  std::uint64_t logicalTick;
  Hash stateHash;
};

struct HashDivergence final {
  std::size_t index;
  std::uint64_t recordSequence;
  std::uint64_t logicalTick;
  Hash expectedHash;
  Hash actualHash;
  bool expectedPresent;
  bool actualPresent;
};

[[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept;

// An unavailable crypto backend is reported as nullopt; no placeholder hash
// is returned and callers must fail closed.
[[nodiscard]] std::optional<Hash>
canonicalStateHash(std::span<const std::uint8_t> canonicalStateBytes) noexcept;

[[nodiscard]] EncodeResult encodeRecord(const Record &record);

[[nodiscard]] RecordDecodeResult
decodeRecord(std::span<const std::uint8_t> encodedRecord);

[[nodiscard]] JournalDecodeResult
decodeJournal(std::span<const std::uint8_t> encodedJournal);

[[nodiscard]] std::optional<HashDivergence>
firstDivergence(std::span<const HashObservation> expected,
                std::span<const HashObservation> actual) noexcept;

} // namespace lol::battle_continuity
