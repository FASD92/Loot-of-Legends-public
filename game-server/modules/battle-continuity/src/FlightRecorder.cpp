#include <lol/battle_continuity/BattleReplay.hpp>
#include <lol/battle_continuity/FlightRecorder.hpp>

#include <algorithm>
#include <bit>
#include <limits>
#include <type_traits>
#include <utility>

namespace lol::battle_continuity {
namespace {

void appendU16(Bytes &bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
  bytes.push_back(static_cast<std::uint8_t>(value));
}

void appendU32(Bytes &bytes, std::uint32_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
  bytes.push_back(static_cast<std::uint8_t>(value));
}

void appendU64(Bytes &bytes, std::uint64_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value >> 56U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 48U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 40U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 32U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
  bytes.push_back(static_cast<std::uint8_t>(value));
}

std::uint32_t readU32(std::span<const std::uint8_t> bytes) noexcept {
  return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
         (static_cast<std::uint32_t>(bytes[1]) << 16U) |
         (static_cast<std::uint32_t>(bytes[2]) << 8U) |
         static_cast<std::uint32_t>(bytes[3]);
}

bool isServerCommand(CanonicalCommandKind kind) noexcept {
  switch (kind) {
  case CanonicalCommandKind::ArenaLoadComplete:
  case CanonicalCommandKind::SuspendInput:
  case CanonicalCommandKind::ResumeInput:
  case CanonicalCommandKind::ParticipantExit:
  case CanonicalCommandKind::LoadBarrierDeadline:
  case CanonicalCommandKind::MovementTick:
  case CanonicalCommandKind::CombatDeadline:
  case CanonicalCommandKind::LootDeadline:
    return true;
  default:
    return false;
  }
}

bool hasTargetSlot(CanonicalCommandKind kind) noexcept {
  return kind == CanonicalCommandKind::ArenaLoadComplete ||
         kind == CanonicalCommandKind::SuspendInput ||
         kind == CanonicalCommandKind::ResumeInput ||
         kind == CanonicalCommandKind::ParticipantExit;
}

bool isKnownCommand(CanonicalCommandKind kind) noexcept {
  switch (kind) {
  case CanonicalCommandKind::ArenaLoadComplete:
  case CanonicalCommandKind::SuspendInput:
  case CanonicalCommandKind::ResumeInput:
  case CanonicalCommandKind::ParticipantExit:
  case CanonicalCommandKind::LoadBarrierDeadline:
  case CanonicalCommandKind::Move:
  case CanonicalCommandKind::MovementTick:
  case CanonicalCommandKind::Attack:
  case CanonicalCommandKind::CombatDeadline:
  case CanonicalCommandKind::ClaimLoot:
  case CanonicalCommandKind::LootDeadline:
    return true;
  }
  return false;
}

bool isEmptyId(CommandId id) noexcept { return id.high == 0U && id.low == 0U; }

bool isCriticalChange(const CanonicalCommand &command,
                      const battle::BattleDeterministicState &before,
                      const RoomRecoveryState &beforeRoom,
                      const battle::BattleDeterministicState &after,
                      const RoomRecoveryState &afterRoom) {
  if (command.kind != CanonicalCommandKind::Move &&
      command.kind != CanonicalCommandKind::MovementTick &&
      command.kind != CanonicalCommandKind::Attack) {
    return true;
  }
  if (beforeRoom != afterRoom || before.state != after.state ||
      before.lootResolution != after.lootResolution ||
      before.resultState != after.resultState ||
      before.candidates != after.candidates ||
      before.capturedParticipants != after.capturedParticipants ||
      before.drops != after.drops || before.holdings != after.holdings ||
      before.monster.has_value() != after.monster.has_value() ||
      (before.monster && after.monster &&
       before.monster->state != after.monster->state) ||
      before.participants.size() != after.participants.size()) {
    return true;
  }
  for (std::size_t index = 0U; index < before.participants.size(); ++index) {
    const auto &old = before.participants[index];
    const auto &now = after.participants[index];
    if (old.slot != now.slot || old.sessionId != now.sessionId ||
        old.inputEnabled != now.inputEnabled ||
        old.gameplayEligible != now.gameplayEligible ||
        old.exitStatus != now.exitStatus) {
      return true;
    }
  }
  return false;
}

bool validIdentity(const BattleIdentity &identity) noexcept {
  const auto room = identity.roomId.value();
  return identity.originRecoveryEpoch != 0U && room != 0U &&
         identity.battleInstanceId.value() != 0U &&
         static_cast<std::uint32_t>(room >> 32U) ==
             identity.originRecoveryEpoch &&
         static_cast<std::uint32_t>(room) != 0U;
}

bool validPayload(const CanonicalCommand &command) noexcept {
  const auto size = command.payload.size();
  switch (command.kind) {
  case CanonicalCommandKind::ArenaLoadComplete:
  case CanonicalCommandKind::SuspendInput:
  case CanonicalCommandKind::ResumeInput:
    return size == 2U && command.payload[0] == 0U && command.payload[1] != 0U &&
           command.payload[1] <= 10U;
  case CanonicalCommandKind::ParticipantExit:
    return size == 3U && command.payload[0] == 0U && command.payload[1] != 0U &&
           command.payload[1] <= 10U && command.payload[2] <= 1U;
  case CanonicalCommandKind::LoadBarrierDeadline:
  case CanonicalCommandKind::CombatDeadline:
  case CanonicalCommandKind::LootDeadline:
    return size == 0U;
  case CanonicalCommandKind::Move:
    return size == 10U;
  case CanonicalCommandKind::MovementTick:
    return size == 4U;
  case CanonicalCommandKind::Attack:
  case CanonicalCommandKind::ClaimLoot:
    return size == 8U;
  }
  return false;
}

bool validCommand(const CanonicalCommand &command) noexcept {
  if (!isKnownCommand(command.kind) || command.participantSlot > 10U ||
      !validPayload(command)) {
    return false;
  }
  if (isServerCommand(command.kind)) {
    return command.participantSlot == battle::systemParticipantSlot &&
           isEmptyId(command.commandId);
  }
  if (command.participantSlot == battle::systemParticipantSlot) {
    return false;
  }
  if (command.kind == CanonicalCommandKind::Move) {
    if (command.commandId.high != 0U) {
      return false;
    }
    const auto actionSequence =
        (static_cast<std::uint32_t>(command.payload[0]) << 24U) |
        (static_cast<std::uint32_t>(command.payload[1]) << 16U) |
        (static_cast<std::uint32_t>(command.payload[2]) << 8U) |
        static_cast<std::uint32_t>(command.payload[3]);
    return actionSequence == command.commandId.low;
  }
  if (command.kind == CanonicalCommandKind::Attack ||
      command.kind == CanonicalCommandKind::ClaimLoot) {
    return !isEmptyId(command.commandId);
  }
  // The source domain types for load, reconnect, and exit do not contain a
  // command id.  The recorder assigns their local record sequence below.
  return false;
}

std::uint16_t targetSlot(const CanonicalCommand &command) noexcept {
  return static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(command.payload[0]) << 8U) |
      static_cast<std::uint16_t>(command.payload[1]));
}

bool participantSlotExists(const battle::BattleDeterministicState &state,
                           std::uint16_t participantSlot) noexcept {
  return std::ranges::any_of(state.candidates,
                             [participantSlot](const auto &candidate) {
                               return candidate.slot == participantSlot;
                             });
}

RecordHeader headerFor(const BattleIdentity &identity,
                       std::uint32_t writerRecoveryEpoch, RecordType recordType,
                       std::uint64_t sequence, battle::BattleTime at) noexcept {
  return RecordHeader{.recordType = recordType,
                      .recordSequence = sequence,
                      .originRecoveryEpoch = identity.originRecoveryEpoch,
                      .writerRecoveryEpoch = writerRecoveryEpoch,
                      .roomId = identity.roomId,
                      .battleInstanceId = identity.battleInstanceId,
                      .logicalTick = at.logicalTick,
                      .battleElapsedNanos = at.battleElapsedNanos};
}

} // namespace

CanonicalCommand
CanonicalCommand::arenaLoadComplete(std::uint16_t targetParticipantSlot) {
  return CanonicalCommand{
      CanonicalCommandKind::ArenaLoadComplete, battle::systemParticipantSlot,
      CommandId{0U, 0U},
      Bytes{static_cast<std::uint8_t>(targetParticipantSlot >> 8U),
            static_cast<std::uint8_t>(targetParticipantSlot)}};
}

CanonicalCommand
CanonicalCommand::suspendInput(std::uint16_t targetParticipantSlot) {
  return CanonicalCommand{
      CanonicalCommandKind::SuspendInput, battle::systemParticipantSlot,
      CommandId{0U, 0U},
      Bytes{static_cast<std::uint8_t>(targetParticipantSlot >> 8U),
            static_cast<std::uint8_t>(targetParticipantSlot)}};
}

CanonicalCommand
CanonicalCommand::resumeInput(std::uint16_t targetParticipantSlot) {
  return CanonicalCommand{
      CanonicalCommandKind::ResumeInput, battle::systemParticipantSlot,
      CommandId{0U, 0U},
      Bytes{static_cast<std::uint8_t>(targetParticipantSlot >> 8U),
            static_cast<std::uint8_t>(targetParticipantSlot)}};
}

CanonicalCommand
CanonicalCommand::participantExit(std::uint16_t targetParticipantSlot,
                                  bool voluntary) {
  Bytes payload{static_cast<std::uint8_t>(targetParticipantSlot >> 8U),
                static_cast<std::uint8_t>(targetParticipantSlot),
                static_cast<std::uint8_t>(voluntary ? 1U : 0U)};
  return CanonicalCommand{CanonicalCommandKind::ParticipantExit,
                          battle::systemParticipantSlot, CommandId{0U, 0U},
                          std::move(payload)};
}

CanonicalCommand CanonicalCommand::loadBarrierDeadline() {
  return CanonicalCommand{CanonicalCommandKind::LoadBarrierDeadline,
                          battle::systemParticipantSlot,
                          CommandId{0U, 0U},
                          {}};
}

CanonicalCommand CanonicalCommand::move(std::uint16_t participantSlot,
                                        CommandId commandId,
                                        battle::DirectionIntent direction) {
  Bytes payload;
  payload.reserve(10U);
  appendU32(payload, static_cast<std::uint32_t>(commandId.low));
  appendU16(payload, std::bit_cast<std::uint16_t>(direction.desiredX));
  appendU16(payload, std::bit_cast<std::uint16_t>(direction.desiredY));
  appendU16(payload, direction.inputFlags);
  return CanonicalCommand{CanonicalCommandKind::Move, participantSlot,
                          commandId, std::move(payload)};
}

CanonicalCommand CanonicalCommand::movementTick(std::uint32_t serverTick) {
  Bytes payload;
  payload.reserve(4U);
  appendU32(payload, serverTick);
  return CanonicalCommand{CanonicalCommandKind::MovementTick,
                          battle::systemParticipantSlot, CommandId{0U, 0U},
                          std::move(payload)};
}

CanonicalCommand CanonicalCommand::attack(std::uint16_t participantSlot,
                                          CommandId commandId,
                                          std::uint64_t targetHint) {
  Bytes payload;
  payload.reserve(8U);
  appendU64(payload, targetHint);
  return CanonicalCommand{CanonicalCommandKind::Attack, participantSlot,
                          commandId, std::move(payload)};
}

CanonicalCommand CanonicalCommand::combatDeadline() {
  return CanonicalCommand{CanonicalCommandKind::CombatDeadline,
                          battle::systemParticipantSlot,
                          CommandId{0U, 0U},
                          {}};
}

CanonicalCommand CanonicalCommand::claimLoot(std::uint16_t participantSlot,
                                             CommandId commandId,
                                             std::uint64_t dropId) {
  Bytes payload;
  payload.reserve(8U);
  appendU64(payload, dropId);
  return CanonicalCommand{CanonicalCommandKind::ClaimLoot, participantSlot,
                          commandId, std::move(payload)};
}

CanonicalCommand CanonicalCommand::lootDeadline() {
  return CanonicalCommand{CanonicalCommandKind::LootDeadline,
                          battle::systemParticipantSlot,
                          CommandId{0U, 0U},
                          {}};
}

FlightRecorder::FlightRecorder(
    BattleIdentity identity, std::uint32_t writerRecoveryEpoch,
    std::vector<Record> records, std::optional<Hash> batchStateHash,
    std::uint64_t batchFirstSequence, std::uint32_t batchRecordCount,
    std::uint64_t batchLastTick, std::uint64_t nextSequence,
    bool terminalRecorded) noexcept
    : identity_(identity), writerRecoveryEpoch_(writerRecoveryEpoch),
      records_(std::move(records)), batchStateHash_(std::move(batchStateHash)),
      batchFirstSequence_(batchFirstSequence),
      batchRecordCount_(batchRecordCount), batchLastTick_(batchLastTick),
      nextSequence_(nextSequence), terminalRecorded_(terminalRecorded) {}

std::optional<FlightRecorder> FlightRecorder::start(
    const battle::BattleInstance &battle, BattleIdentity identity,
    std::uint32_t writerRecoveryEpoch, const RoomRecoveryState &roomState) {
  if (!validIdentity(identity) || writerRecoveryEpoch == 0U) {
    return std::nullopt;
  }
  const auto state = battle.exportDeterministicState();
  if (state.roomId != identity.roomId ||
      state.battleId != identity.battleInstanceId ||
      state.rulesetVersion != kSupportedBattleRulesetVersion ||
      state.seed == 0U || !state.battleTime.valid() ||
      state.state != battle::BattleLoadState::LoadBarrierOpen ||
      state.battleTime.logicalTick != 0U) {
    return std::nullopt;
  }
  const auto encodedState = encodeRecoveryState(state, roomState);
  if (!encodedState.ok()) {
    return std::nullopt;
  }
  const auto stateHash = recoveryStateHash(encodedState.bytes);
  if (!stateHash.has_value() || state.candidates.size() < 2U ||
      state.candidates.size() > 10U) {
    return std::nullopt;
  }
  std::vector<BattleStartParticipant> participants;
  participants.reserve(state.candidates.size());
  std::uint16_t previousSlot = 0U;
  for (const auto &candidate : state.candidates) {
    if (candidate.slot == 0U || candidate.slot <= previousSlot ||
        candidate.sessionId.value() == 0U) {
      return std::nullopt;
    }
    previousSlot = candidate.slot;
    participants.push_back(BattleStartParticipant{
        .participantSlot = candidate.slot, .sessionId = candidate.sessionId});
  }

  std::vector<Record> records;
  records.reserve(4U);
  const auto startAt = battle::BattleTime::fromLogicalTick(0U);
  records.push_back(
      Record{.header = headerFor(identity, writerRecoveryEpoch,
                                 RecordType::BattleStart, 1U, startAt),
             .payload = BattleStartPayload{
                 .rulesetVersion = state.rulesetVersion,
                 .battleSeed = state.seed,
                 .tickHertz = kTickHertz,
                 .initialStateHash = *stateHash,
                 .participants = std::move(participants),
             }});
  records.push_back(
      Record{.header = headerFor(identity, writerRecoveryEpoch,
                                 RecordType::Checkpoint, 2U, state.battleTime),
             .payload = CheckpointPayload{
                 .checkpointSchemaVersion = kCheckpointSchemaVersion,
                 .canonicalStateBytes = encodedState.bytes,
                 .stateHash = *stateHash,
             }});
  std::size_t encodedBytes = 0U;
  for (const auto &record : records) {
    const auto encoded = encodeRecord(record);
    if (!encoded.ok() ||
        encodedBytes > kMaximumJournalBytes - encoded.bytes.size()) {
      return std::nullopt;
    }
    encodedBytes += encoded.bytes.size();
  }
  auto recorder = FlightRecorder{identity,
                                 writerRecoveryEpoch,
                                 std::move(records),
                                 stateHash,
                                 1U,
                                 2U,
                                 state.battleTime.logicalTick,
                                 3U,
                                 false};
  recorder.encodedBytes_ = encodedBytes;
  return recorder;
}

RecorderAppendResult FlightRecorder::failure(RecorderErrorCode code) const {
  return RecorderAppendResult{
      .recorded = false,
      .record = std::nullopt,
      .error = RecorderError{.code = code, .recordSequence = nextSequence_}};
}

bool FlightRecorder::identityMatches(
    const battle::BattleDeterministicState &state) const noexcept {
  return state.roomId == identity_.roomId &&
         state.battleId == identity_.battleInstanceId &&
         state.rulesetVersion == kSupportedBattleRulesetVersion &&
         state.seed != 0U && state.battleTime.valid();
}

RecorderAppendResult FlightRecorder::appendDataRecord(Record record,
                                                      Hash stateHash) {
  if (nextSequence_ == 0U ||
      nextSequence_ == std::numeric_limits<std::uint64_t>::max()) {
    return failure(RecorderErrorCode::SequenceExhausted);
  }
  if (!records_.empty() &&
      record.header.logicalTick < records_.back().header.logicalTick) {
    return failure(RecorderErrorCode::CodecRejected);
  }
  if (batchRecordCount_ != 0U && record.header.logicalTick != batchLastTick_) {
    return failure(RecorderErrorCode::BatchNotCommitted);
  }
  const auto encoded = encodeRecord(record);
  if (record.header.recordSequence != nextSequence_ || !encoded.ok() ||
      encodedBytes_ > kMaximumJournalBytes - encoded.bytes.size()) {
    return failure(RecorderErrorCode::CodecRejected);
  }
  const auto copy = record;
  records_.push_back(std::move(record));
  encodedBytes_ += encoded.bytes.size();
  if (!batchStateHash_.has_value()) {
    batchFirstSequence_ = copy.header.recordSequence;
  }
  batchStateHash_ = stateHash;
  ++batchRecordCount_;
  batchLastTick_ = copy.header.logicalTick;
  ++nextSequence_;
  return RecorderAppendResult{
      .recorded = true, .record = copy, .error = std::nullopt};
}

RecorderAppendResult FlightRecorder::appendCommand(
    const CanonicalCommand &command,
    const battle::BattleDeterministicState &before,
    const RoomRecoveryState &beforeRoom, const battle::BattleInstance &after,
    const RoomRecoveryState &afterRoom, std::uint16_t decisionCode,
    Bytes outcomePayload) {
  if (!validCommand(command) || !outcomePayload.empty()) {
    // Result code is the only command outcome.  An opaque byte field would
    // let credentials or PII enter an otherwise value-only journal.
    return failure(RecorderErrorCode::InvalidCommand);
  }
  if (!identityMatches(before)) {
    return failure(RecorderErrorCode::InvalidIdentity);
  }
  const auto participant = hasTargetSlot(command.kind)
                               ? targetSlot(command)
                               : command.participantSlot;
  if ((hasTargetSlot(command.kind) || !isServerCommand(command.kind)) &&
      !participantSlotExists(before, participant)) {
    return failure(RecorderErrorCode::InvalidCommand);
  }
  const auto afterState = after.exportDeterministicState();
  if (!identityMatches(afterState)) {
    return failure(RecorderErrorCode::InvalidIdentity);
  }
  const auto beforeEncoded = encodeRecoveryState(before, beforeRoom);
  const auto afterEncoded = encodeRecoveryState(afterState, afterRoom);
  if (!beforeEncoded.ok() || !afterEncoded.ok()) {
    return failure(RecorderErrorCode::StateEncodingFailed);
  }
  const auto beforeHash = recoveryStateHash(beforeEncoded.bytes);
  const auto afterHash = recoveryStateHash(afterEncoded.bytes);
  if (!beforeHash.has_value() || !afterHash.has_value()) {
    return failure(RecorderErrorCode::StateEncodingFailed);
  }
  if (*beforeHash == *afterHash) {
    return RecorderAppendResult{
        .recorded = false, .record = std::nullopt, .error = std::nullopt};
  }

  if (command.kind == CanonicalCommandKind::MovementTick) {
    auto expectedSnapshotSequence = before.nextSnapshotSequence;
    if ((readU32(command.payload) % 2U) == 0U) {
      expectedSnapshotSequence =
          before.nextSnapshotSequence ==
                  std::numeric_limits<std::uint32_t>::max()
              ? 1U
              : before.nextSnapshotSequence + 1U;
    }
    if (afterState.nextSnapshotSequence != expectedSnapshotSequence) {
      return failure(RecorderErrorCode::InvalidCommand);
    }
  }

  auto commandId = command.commandId;
  if (isServerCommand(command.kind)) {
    commandId = CommandId{.high = 0U, .low = nextSequence_};
  }
  const auto at = afterState.battleTime;
  auto record = Record{
      .header = headerFor(identity_, writerRecoveryEpoch_,
                          RecordType::CommandDecision, nextSequence_, at),
      .payload = CommandDecisionPayload{
          .participantSlot = command.participantSlot,
          .commandKind = static_cast<std::uint16_t>(command.kind),
          .commandId = commandId,
          .decisionCode = decisionCode,
          .commandPayload = command.payload,
          .outcomePayload = {},
          .roomRecoveryStateBytes = encodeRoomRecoveryState(afterRoom).bytes,
          .postDecisionStateHash = *afterHash,
      }};
  return appendDataRecord(record, *afterHash);
}

RecorderAppendResult
FlightRecorder::appendCheckpoint(const battle::BattleInstance &battle,
                                 const RoomRecoveryState &roomState) {
  const auto state = battle.exportDeterministicState();
  if (!identityMatches(state)) {
    return failure(RecorderErrorCode::InvalidIdentity);
  }
  const auto encoded = encodeRecoveryState(state, roomState);
  const auto hash =
      encoded.ok() ? recoveryStateHash(encoded.bytes) : std::nullopt;
  if (!encoded.ok() || !hash.has_value()) {
    return failure(RecorderErrorCode::StateEncodingFailed);
  }
  const auto record =
      Record{.header = headerFor(identity_, writerRecoveryEpoch_,
                                 RecordType::Checkpoint, nextSequence_,
                                 state.battleTime),
             .payload = CheckpointPayload{.checkpointSchemaVersion =
                                              kCheckpointSchemaVersion,
                                          .canonicalStateBytes = encoded.bytes,
                                          .stateHash = *hash}};
  return appendDataRecord(record, *hash);
}

RecorderAppendResult FlightRecorder::appendTerminal(
    std::uint16_t terminalReason, const battle::BattleInstance &battle,
    const RoomRecoveryState &roomState,
    std::uint64_t resultCommittedUnixEpochMilliseconds,
    SettlementBatchId settlementBatchId,
    std::vector<SettlementReceipt> settlements) {
  if (terminalRecorded_) {
    return failure(RecorderErrorCode::TerminalAlreadyRecorded);
  }
  if (settlements.size() > std::numeric_limits<std::uint16_t>::max()) {
    return failure(RecorderErrorCode::TerminalNotReady);
  }
  const auto state = battle.exportDeterministicState();
  if (!identityMatches(state) ||
      state.resultState != battle::BattleResultState::Committed ||
      !state.committedResult.has_value()) {
    return failure(RecorderErrorCode::TerminalNotReady);
  }
  std::uint16_t previousSlot = 0U;
  for (const auto &settlement : settlements) {
    if (settlement.participantSlot == battle::systemParticipantSlot ||
        static_cast<std::size_t>(settlement.participantSlot) >
            state.candidates.size() ||
        settlement.participantSlot <= previousSlot) {
      return failure(RecorderErrorCode::TerminalNotReady);
    }
    previousSlot = settlement.participantSlot;
  }
  const auto encodedState = encodeRecoveryState(state, roomState);
  const auto stateHash =
      encodedState.ok() ? recoveryStateHash(encodedState.bytes) : std::nullopt;
  const auto encodedResult =
      encodeCanonicalBattleResult(*state.committedResult);
  if (!encodedState.ok() || !stateHash.has_value() || !encodedResult.ok()) {
    return failure(RecorderErrorCode::StateEncodingFailed);
  }
  auto record = Record{.header = headerFor(identity_, writerRecoveryEpoch_,
                                           RecordType::TerminalReceipt,
                                           nextSequence_, state.battleTime),
                       .payload = TerminalReceiptPayload{
                           .terminalReason = terminalReason,
                           .finalStateHash = Hash{},
                           .canonicalResultPayload = encodedResult.bytes,
                           .resultCommittedUnixEpochMilliseconds =
                               resultCommittedUnixEpochMilliseconds,
                           .resultCommittedBattleElapsedNanos =
                               state.battleTime.battleElapsedNanos,
                           .settlementBatchId = settlementBatchId,
                           .settlementIntentCount =
                               static_cast<std::uint16_t>(settlements.size()),
                           .settlements = std::move(settlements),
                       }};
  auto &terminal = std::get<TerminalReceiptPayload>(record.payload);
  const auto terminalHash =
      canonicalRecoveryTerminalHash(encodedState.bytes, terminal);
  if (!terminalHash.has_value()) {
    return failure(RecorderErrorCode::StateEncodingFailed);
  }
  terminal.finalStateHash = *terminalHash;
  auto appended = appendDataRecord(record, *terminalHash);
  if (appended.recorded) {
    terminalRecorded_ = true;
  }
  return appended;
}

RecorderAppendResult FlightRecorder::commitTick() {
  if (!batchStateHash_.has_value() || batchRecordCount_ == 0U) {
    return failure(RecorderErrorCode::BatchEmpty);
  }
  const auto batchBegin = records_.size() - batchRecordCount_;
  for (std::size_t index = batchBegin; index < records_.size(); ++index) {
    if (records_[index].header.recordType == RecordType::TerminalReceipt &&
        (index + 2U != records_.size() ||
         records_.back().header.recordType != RecordType::Checkpoint)) {
      return failure(RecorderErrorCode::BatchNotCommitted);
    }
  }
  if (nextSequence_ == 0U ||
      nextSequence_ == std::numeric_limits<std::uint64_t>::max()) {
    return failure(RecorderErrorCode::SequenceExhausted);
  }
  const auto record = Record{
      .header = headerFor(identity_, writerRecoveryEpoch_,
                          RecordType::TickCommit, nextSequence_,
                          battle::BattleTime::fromLogicalTick(batchLastTick_)),
      .payload = TickCommitPayload{
          .firstRecordSequence = batchFirstSequence_,
          .lastDataRecordSequence = nextSequence_ - 1U,
          .recordCount = batchRecordCount_,
          .committedStateHash = *batchStateHash_,
      }};
  const auto encoded = encodeRecord(record);
  if (!encoded.ok() ||
      encodedBytes_ > kMaximumJournalBytes - encoded.bytes.size()) {
    return failure(RecorderErrorCode::CodecRejected);
  }
  records_.push_back(record);
  encodedBytes_ += encoded.bytes.size();
  ++nextSequence_;
  batchStateHash_.reset();
  batchFirstSequence_ = 0U;
  batchRecordCount_ = 0U;
  batchLastTick_ = 0U;
  return RecorderAppendResult{
      .recorded = true, .record = record, .error = std::nullopt};
}

JournalEncodeResult FlightRecorder::encodeJournal() const {
  if (batchStateHash_.has_value() || batchRecordCount_ != 0U) {
    return JournalEncodeResult{
        .bytes = {},
        .error = RecorderError{.code = RecorderErrorCode::BatchNotCommitted,
                               .recordSequence = nextSequence_}};
  }
  Bytes bytes;
  for (const auto &record : records_) {
    const auto encoded = encodeRecord(record);
    if (!encoded.ok()) {
      return JournalEncodeResult{
          .bytes = {},
          .error =
              RecorderError{.code = RecorderErrorCode::CodecRejected,
                            .recordSequence = record.header.recordSequence}};
    }
    bytes.insert(bytes.end(), encoded.bytes.begin(), encoded.bytes.end());
  }
  return JournalEncodeResult{.bytes = std::move(bytes), .error = std::nullopt};
}

const std::vector<Record> &FlightRecorder::records() const noexcept {
  return records_;
}

const BattleIdentity &FlightRecorder::identity() const noexcept {
  return identity_;
}

std::uint32_t FlightRecorder::writerRecoveryEpoch() const noexcept {
  return writerRecoveryEpoch_;
}

BattleRecording::BattleRecording(
    FlightRecorder recorder, std::size_t submittedRecordCount,
    std::optional<RecordedTickBatch> pendingBatch,
    std::optional<RoomRecoveryState> roomState) noexcept
    : recorder_(std::move(recorder)),
      submittedRecordCount_(submittedRecordCount),
      pendingBatch_(std::move(pendingBatch)), roomState_(std::move(roomState)) {
}

std::optional<BattleRecording> BattleRecording::start(
    const battle::BattleInstance &battle, BattleIdentity identity,
    std::uint32_t writerRecoveryEpoch, const RoomRecoveryState &roomState) {
  auto recorder =
      FlightRecorder::start(battle, identity, writerRecoveryEpoch, roomState);
  if (!recorder.has_value() || !recorder->commitTick().ok()) {
    return std::nullopt;
  }
  BattleRecording recording{std::move(*recorder), 0U, std::nullopt, roomState};
  if (!recording.capturePendingBatch(false)) {
    return std::nullopt;
  }
  return recording;
}

std::optional<BattleRecording>
BattleRecording::resume(const battle::BattleInstance &battle,
                        std::vector<Record> committedRecords,
                        std::uint32_t writerRecoveryEpoch) {
  if (writerRecoveryEpoch == 0U || committedRecords.empty() ||
      committedRecords.back().header.recordType != RecordType::TickCommit ||
      committedRecords.back().header.recordSequence ==
          std::numeric_limits<std::uint64_t>::max()) {
    return std::nullopt;
  }

  const auto &first = committedRecords.front().header;
  const BattleIdentity identity{
      .originRecoveryEpoch = first.originRecoveryEpoch,
      .roomId = first.roomId,
      .battleInstanceId = first.battleInstanceId,
  };
  if (!validIdentity(identity)) {
    return std::nullopt;
  }
  std::uint32_t maximumPreviousWriterEpoch = 0U;
  std::size_t encodedBytes = 0U;
  bool checkpointSeen = false;
  bool terminalSeen = false;
  for (const auto &record : committedRecords) {
    const auto encoded = encodeRecord(record);
    if (!encoded.ok() ||
        encodedBytes > kMaximumJournalBytes - encoded.bytes.size()) {
      return std::nullopt;
    }
    encodedBytes += encoded.bytes.size();
    maximumPreviousWriterEpoch =
        std::max(maximumPreviousWriterEpoch, record.header.writerRecoveryEpoch);
    if (record.header.recordType == RecordType::Checkpoint) {
      checkpointSeen = true;
    } else if (record.header.recordType == RecordType::TerminalReceipt) {
      terminalSeen = true;
    }
  }
  if (!checkpointSeen || writerRecoveryEpoch <= maximumPreviousWriterEpoch) {
    return std::nullopt;
  }

  const auto replay = BattleReplayer::replayRecords(committedRecords);
  if (!replay.ok() || !replay.finalStateHash.has_value() ||
      !replay.finalRoomRecoveryState.has_value()) {
    return std::nullopt;
  }
  const auto encodedState = encodeRecoveryState(
      battle.exportDeterministicState(), *replay.finalRoomRecoveryState);
  const auto currentHash =
      encodedState.ok() ? recoveryStateHash(encodedState.bytes) : std::nullopt;
  const auto &lastCommit =
      std::get<TickCommitPayload>(committedRecords.back().payload);
  if (!currentHash.has_value() || *currentHash != *replay.finalStateHash ||
      *currentHash != lastCommit.committedStateHash) {
    return std::nullopt;
  }

  const auto nextSequence = committedRecords.back().header.recordSequence + 1U;
  const auto submittedRecordCount = committedRecords.size();
  FlightRecorder recorder{identity,
                          writerRecoveryEpoch,
                          std::move(committedRecords),
                          std::nullopt,
                          0U,
                          0U,
                          0U,
                          nextSequence,
                          terminalSeen};
  recorder.encodedBytes_ = encodedBytes;
  return BattleRecording{std::move(recorder), submittedRecordCount,
                         std::nullopt, replay.finalRoomRecoveryState};
}

bool BattleRecording::recordDecision(
    const CanonicalCommand &command,
    const battle::BattleDeterministicState &before,
    const RoomRecoveryState &beforeRoom, const battle::BattleInstance &after,
    const RoomRecoveryState &afterRoom, std::uint16_t decisionCode,
    std::optional<TerminalRecording> terminal) {
  if (!roomState_.has_value() || beforeRoom != *roomState_ ||
      pendingBatch_.has_value()) {
    return false;
  }
  const auto appended = recorder_.appendCommand(
      command, before, beforeRoom, after, afterRoom, decisionCode, {});
  if (!appended.ok()) {
    return false;
  }
  if (!appended.recorded) {
    return !terminal.has_value();
  }

  const auto state = after.exportDeterministicState();
  const bool critical =
      isCriticalChange(command, before, beforeRoom, state, afterRoom);

  if (terminal.has_value()) {
    if (!recorder_
             .appendTerminal(terminal->terminalReason, after, afterRoom,
                             terminal->resultCommittedUnixEpochMilliseconds,
                             terminal->settlementBatchId,
                             std::move(terminal->settlements))
             .recorded ||
        !recorder_.appendCheckpoint(after, afterRoom).recorded ||
        !recorder_.commitTick().ok()) {
      return false;
    }
    roomState_ = afterRoom;
    return capturePendingBatch(true);
  }

  if (critical && !recorder_.appendCheckpoint(after, afterRoom).recorded) {
    return false;
  }
  if (!recorder_.commitTick().ok()) {
    return false;
  }
  roomState_ = afterRoom;
  return !critical || capturePendingBatch(false);
}

bool BattleRecording::capturePendingBatch(bool terminal) {
  const auto &records = recorder_.records();
  if (pendingBatch_.has_value() || records.empty() ||
      records.back().header.recordType != RecordType::TickCommit) {
    return false;
  }
  const auto begin = submittedRecordCount_;
  if (begin >= records.size()) {
    return false;
  }
  Bytes bytes;
  for (std::size_t index = begin; index < records.size(); ++index) {
    const auto encoded = encodeRecord(records[index]);
    if (!encoded.ok() ||
        bytes.size() > kMaximumJournalBytes - encoded.bytes.size()) {
      return false;
    }
    bytes.insert(bytes.end(), encoded.bytes.begin(), encoded.bytes.end());
  }
  pendingBatch_ = RecordedTickBatch{
      .identity = recorder_.identity(),
      .writerRecoveryEpoch = recorder_.writerRecoveryEpoch(),
      .firstRecordSequence = records[begin].header.recordSequence,
      .lastRecordSequence = records.back().header.recordSequence,
      .logicalTick = records.back().header.logicalTick,
      .terminal = terminal,
      .encodedRecords = std::move(bytes),
  };
  submittedRecordCount_ = records.size();
  return true;
}

std::optional<RecordedTickBatch> BattleRecording::takePendingBatch() {
  return std::exchange(pendingBatch_, std::nullopt);
}

JournalEncodeResult BattleRecording::encodeJournal() const {
  return recorder_.encodeJournal();
}

const std::vector<Record> &BattleRecording::records() const noexcept {
  return recorder_.records();
}

const std::optional<RoomRecoveryState> &
BattleRecording::roomRecoveryState() const noexcept {
  return roomState_;
}

} // namespace lol::battle_continuity
