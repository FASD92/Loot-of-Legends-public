#include <lol/battle_continuity/BattleReplay.hpp>

#include <lol/battle/BattleAdmission.hpp>
#include <lol/battle/BattleDeterministicState.hpp>
#include <lol/battle/BattleTime.hpp>
#include <lol/battle/CombatApi.hpp>
#include <lol/battle/LootApi.hpp>
#include <lol/battle/MovementApi.hpp>
#include <lol/battle_continuity/BattleStateCodec.hpp>
#include <lol/battle_continuity/FlightRecorder.hpp>
#include <lol/battle_continuity/RecoveryStateCodec.hpp>
#include <lol/shared/Identifiers.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <utility>
#include <vector>

namespace lol::battle_continuity {
namespace {

using battle::ParticipantSlot;
using shared::AccountId;

struct Cursor final {
  std::span<const std::uint8_t> bytes;
  std::size_t position{0U};

  [[nodiscard]] std::size_t remaining() const noexcept {
    return bytes.size() - position;
  }

  [[nodiscard]] bool u8(std::uint8_t &value) noexcept {
    if (remaining() < 1U) {
      return false;
    }
    value = bytes[position++];
    return true;
  }

  [[nodiscard]] bool u16(std::uint16_t &value) noexcept {
    if (remaining() < 2U) {
      return false;
    }
    value = static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(bytes[position]) << 8U) |
        static_cast<std::uint32_t>(bytes[position + 1U]));
    position += 2U;
    return true;
  }

  [[nodiscard]] bool u32(std::uint32_t &value) noexcept {
    if (remaining() < 4U) {
      return false;
    }
    value = (static_cast<std::uint32_t>(bytes[position]) << 24U) |
            (static_cast<std::uint32_t>(bytes[position + 1U]) << 16U) |
            (static_cast<std::uint32_t>(bytes[position + 2U]) << 8U) |
            static_cast<std::uint32_t>(bytes[position + 3U]);
    position += 4U;
    return true;
  }

  [[nodiscard]] bool u64(std::uint64_t &value) noexcept {
    if (remaining() < 8U) {
      return false;
    }
    value = (static_cast<std::uint64_t>(bytes[position]) << 56U) |
            (static_cast<std::uint64_t>(bytes[position + 1U]) << 48U) |
            (static_cast<std::uint64_t>(bytes[position + 2U]) << 40U) |
            (static_cast<std::uint64_t>(bytes[position + 3U]) << 32U) |
            (static_cast<std::uint64_t>(bytes[position + 4U]) << 24U) |
            (static_cast<std::uint64_t>(bytes[position + 5U]) << 16U) |
            (static_cast<std::uint64_t>(bytes[position + 6U]) << 8U) |
            static_cast<std::uint64_t>(bytes[position + 7U]);
    position += 8U;
    return true;
  }
};

struct DecodedCommand final {
  CanonicalCommandKind kind;
  ParticipantSlot targetParticipantSlot{battle::systemParticipantSlot};
  bool voluntary{false};
  std::uint32_t actionSequence{0U};
  battle::DirectionIntent direction{};
  std::uint32_t serverTick{0U};
  std::uint64_t targetHint{0U};
  std::uint64_t dropId{0U};
};

bool knownCommandKind(CanonicalCommandKind kind) noexcept {
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

bool serverCommand(CanonicalCommandKind kind) noexcept {
  return kind == CanonicalCommandKind::ArenaLoadComplete ||
         kind == CanonicalCommandKind::SuspendInput ||
         kind == CanonicalCommandKind::ResumeInput ||
         kind == CanonicalCommandKind::ParticipantExit ||
         kind == CanonicalCommandKind::LoadBarrierDeadline ||
         kind == CanonicalCommandKind::MovementTick ||
         kind == CanonicalCommandKind::CombatDeadline ||
         kind == CanonicalCommandKind::LootDeadline;
}

bool emptyCommandId(CommandId id) noexcept {
  return id.high == 0U && id.low == 0U;
}

bool decodeCommand(const CommandDecisionPayload &payload,
                   std::uint64_t recordSequence,
                   DecodedCommand &decoded) noexcept {
  decoded.kind = static_cast<CanonicalCommandKind>(payload.commandKind);
  if (!knownCommandKind(decoded.kind) || payload.participantSlot > 10U ||
      !payload.outcomePayload.empty()) {
    return false;
  }
  if (serverCommand(decoded.kind)) {
    if (payload.participantSlot != battle::systemParticipantSlot ||
        payload.commandId.high != 0U ||
        payload.commandId.low != recordSequence) {
      return false;
    }
  } else if (payload.participantSlot == battle::systemParticipantSlot) {
    return false;
  } else if (decoded.kind == CanonicalCommandKind::Move) {
    if (payload.commandId.high != 0U) {
      return false;
    }
  } else if (decoded.kind == CanonicalCommandKind::Attack ||
             decoded.kind == CanonicalCommandKind::ClaimLoot) {
    if (emptyCommandId(payload.commandId)) {
      return false;
    }
  }

  Cursor cursor{.bytes = payload.commandPayload};
  switch (decoded.kind) {
  case CanonicalCommandKind::LoadBarrierDeadline:
  case CanonicalCommandKind::CombatDeadline:
  case CanonicalCommandKind::LootDeadline:
    return cursor.remaining() == 0U;
  case CanonicalCommandKind::ArenaLoadComplete:
  case CanonicalCommandKind::SuspendInput:
  case CanonicalCommandKind::ResumeInput: {
    if (!cursor.u16(decoded.targetParticipantSlot) ||
        decoded.targetParticipantSlot == battle::systemParticipantSlot ||
        decoded.targetParticipantSlot > 10U || cursor.remaining() != 0U) {
      return false;
    }
    return true;
  }
  case CanonicalCommandKind::ParticipantExit: {
    if (!cursor.u16(decoded.targetParticipantSlot) ||
        decoded.targetParticipantSlot == battle::systemParticipantSlot ||
        decoded.targetParticipantSlot > 10U) {
      return false;
    }
    std::uint8_t reason = 0U;
    if (!cursor.u8(reason) || reason > 1U || cursor.remaining() != 0U) {
      return false;
    }
    decoded.voluntary = reason == 1U;
    return true;
  }
  case CanonicalCommandKind::Move: {
    std::uint32_t actionSequence = 0U;
    std::uint16_t desiredX = 0U;
    std::uint16_t desiredY = 0U;
    std::uint16_t inputFlags = 0U;
    if (!cursor.u32(actionSequence) || !cursor.u16(desiredX) ||
        !cursor.u16(desiredY) || !cursor.u16(inputFlags) ||
        cursor.remaining() != 0U || payload.commandId.low != actionSequence) {
      return false;
    }
    decoded.actionSequence = actionSequence;
    decoded.direction = battle::DirectionIntent{
        .desiredX = std::bit_cast<std::int16_t>(desiredX),
        .desiredY = std::bit_cast<std::int16_t>(desiredY),
        .inputFlags = inputFlags};
    return true;
  }
  case CanonicalCommandKind::MovementTick:
    return cursor.u32(decoded.serverTick) && cursor.remaining() == 0U;
  case CanonicalCommandKind::Attack:
    return cursor.u64(decoded.targetHint) && cursor.remaining() == 0U;
  case CanonicalCommandKind::ClaimLoot:
    return cursor.u64(decoded.dropId) && cursor.remaining() == 0U;
  }
  return false;
}

ReplayResult failure(ReplayErrorCode code, std::size_t recordIndex,
                     std::uint64_t recordSequence, std::uint64_t logicalTick,
                     std::optional<Hash> expected = std::nullopt,
                     std::optional<Hash> actual = std::nullopt,
                     std::optional<BattleIdentity> identity = std::nullopt,
                     std::optional<std::uint32_t> rulesetVersion =
                         std::nullopt) {
  return ReplayResult{.battle = std::nullopt,
                      .finalStateHash = std::nullopt,
                      .finalRoomRecoveryState = std::nullopt,
                      .error = ReplayError{.code = code,
                                           .recordIndex = recordIndex,
                                           .recordSequence = recordSequence,
                                           .logicalTick = logicalTick,
                                           .expectedHash = std::move(expected),
                                           .actualHash = std::move(actual),
                                           .identity = std::move(identity),
                                           .rulesetVersion = rulesetVersion}};
}

std::optional<Hash> stateHash(const battle::BattleInstance &battle,
                              const RoomRecoveryState &roomState) {
  const auto encoded = encodeRecoveryState(battle.exportDeterministicState(),
                                            roomState);
  if (!encoded.ok()) {
    return std::nullopt;
  }
  return recoveryStateHash(encoded.bytes);
}

std::optional<Bytes> encodedState(const battle::BattleInstance &battle,
                                  const RoomRecoveryState &roomState) {
  const auto encoded = encodeRecoveryState(battle.exportDeterministicState(),
                                            roomState);
  if (!encoded.ok()) {
    return std::nullopt;
  }
  return encoded.bytes;
}

bool settlementSlotsMatch(const TerminalReceiptPayload &payload,
                          const battle::BattleDeterministicState &state) {
  if (payload.settlementIntentCount != payload.settlements.size()) {
    return false;
  }
  std::uint16_t previousSlot = 0U;
  for (const auto &settlement : payload.settlements) {
    if (settlement.participantSlot == battle::systemParticipantSlot ||
        static_cast<std::size_t>(settlement.participantSlot) >
            state.candidates.size() ||
        settlement.participantSlot <= previousSlot) {
      return false;
    }
    previousSlot = settlement.participantSlot;
  }
  return true;
}

std::optional<shared::SessionId>
sessionForSlot(const battle::BattleInstance &battle, ParticipantSlot slot) {
  const auto state = battle.exportDeterministicState();
  for (const auto &participant : state.participants) {
    if (participant.slot == slot) {
      return participant.sessionId;
    }
  }
  for (const auto &candidate : state.candidates) {
    if (candidate.slot == slot) {
      return candidate.sessionId;
    }
  }
  return std::nullopt;
}

std::optional<std::uint16_t> applyCommand(battle::BattleInstance &battle,
                                          const BattleIdentity &identity,
                                          const Record &record,
                                          const CommandDecisionPayload &payload,
                                          const DecodedCommand &command) {
  const auto at = battle::BattleTime{.logicalTick = record.header.logicalTick,
                                     .battleElapsedNanos =
                                         record.header.battleElapsedNanos};
  const auto participantSlot =
      command.targetParticipantSlot != battle::systemParticipantSlot
          ? command.targetParticipantSlot
          : payload.participantSlot;
  const bool needsParticipant =
      participantSlot != battle::systemParticipantSlot;
  const auto session =
      needsParticipant ? sessionForSlot(battle, participantSlot) : std::nullopt;
  if (!at.valid() || (needsParticipant && !session.has_value())) {
    return std::nullopt;
  }
  constexpr std::uint64_t replayGeneration = 1U;
  const auto generation = shared::SessionGeneration{replayGeneration};
  const auto commandId = battle::CommandId{.high = payload.commandId.high,
                                           .low = payload.commandId.low};
  switch (command.kind) {
  case CanonicalCommandKind::ArenaLoadComplete: {
    const auto result = battle.completeLoad(
        battle::ArenaLoadCompleteCommand{.sessionId = *session,
                                         .generation = generation,
                                         .roomId = identity.roomId,
                                         .battleId = identity.battleInstanceId},
        true, at);
    return static_cast<std::uint16_t>(result);
  }
  case CanonicalCommandKind::SuspendInput:
    return static_cast<std::uint16_t>(
        battle.suspendInput(*session, generation, at));
  case CanonicalCommandKind::ResumeInput:
    return static_cast<std::uint16_t>(
        battle.resumeInput(*session, generation, at));
  case CanonicalCommandKind::ParticipantExit: {
    const battle::CandidateDisconnectedCommand disconnect{
        .sessionId = *session,
        .generation = generation,
        .roomId = identity.roomId,
        .battleId = identity.battleInstanceId};
    const auto result = command.voluntary ? battle.leave(disconnect, at)
                                          : battle.disconnect(disconnect, at);
    return static_cast<std::uint16_t>(result);
  }
  case CanonicalCommandKind::LoadBarrierDeadline:
    return static_cast<std::uint16_t>(battle.expireLoadBarrier(
        battle::LoadBarrierDeadlineCommand{
            .roomId = identity.roomId, .battleId = identity.battleInstanceId},
        at));
  case CanonicalCommandKind::Move:
    return static_cast<std::uint16_t>(battle.acceptMove(
        battle::MoveCommand{.sessionId = *session,
                            .generation = generation,
                            .battleId = identity.battleInstanceId,
                            .actionSequence = command.actionSequence,
                            .direction = command.direction},
        at));
  case CanonicalCommandKind::MovementTick: {
    const auto result = battle.integrateMovement(
        battle::MovementTickCommand{.battleId = identity.battleInstanceId,
                                    .serverTick = command.serverTick},
        at);
    if (result == battle::MovementResultCode::Ok &&
        (command.serverTick % 2U) == 0U &&
        !battle.captureStateSnapshot().has_value()) {
      return std::nullopt;
    }
    return static_cast<std::uint16_t>(result);
  }
  case CanonicalCommandKind::Attack:
    return static_cast<std::uint16_t>(
        battle
            .attackWithApplied(
                battle::AttackCommand{.commandId = commandId,
                                      .sessionId = *session,
                                      .generation = generation,
                                      .battleId = identity.battleInstanceId,
                                      .targetHint = command.targetHint},
                at)
            .result.code);
  case CanonicalCommandKind::CombatDeadline:
    return static_cast<std::uint16_t>(battle.expireCombat(
        battle::CombatDeadlineCommand{.battleId = identity.battleInstanceId},
        at));
  case CanonicalCommandKind::ClaimLoot:
    return static_cast<std::uint16_t>(
        battle
            .claimLoot(
                battle::ClaimLootCommand{.commandId = commandId,
                                         .sessionId = *session,
                                         .generation = generation,
                                         .battleId = identity.battleInstanceId,
                                         .dropId =
                                             battle::DropId{command.dropId}},
                at)
            .code);
  case CanonicalCommandKind::LootDeadline:
    return static_cast<std::uint16_t>(battle.expireLoot(
        battle::LootDeadlineCommand{.battleId = identity.battleInstanceId},
        at));
  }
  return std::nullopt;
}

std::optional<battle::BattleInstance>
createReplayBattle(const BattleIdentity &identity,
                   const BattleStartPayload &startPayload,
                   const battle::BattleDeterministicState &state) {
  constexpr std::uint64_t replayGeneration = 1U;
  std::vector<battle::BattleStartCandidate> candidates;
  candidates.reserve(startPayload.participants.size());
  for (const auto &participant : startPayload.participants) {
    AccountId::Bytes accountBytes{};
    accountBytes[0] = 0x52U;
    accountBytes[14] =
        static_cast<std::uint8_t>(participant.participantSlot >> 8U);
    accountBytes[15] = static_cast<std::uint8_t>(participant.participantSlot);
    candidates.push_back(battle::BattleStartCandidate{
        .accountId = AccountId{accountBytes},
        .sessionId = participant.sessionId,
        .generation = shared::SessionGeneration{replayGeneration},
        .nickname = "replay"});
  }
  auto created = battle::BattleInstance::create(battle::BattleAdmissionSnapshot{
      .roomId = identity.roomId,
      .battleId = identity.battleInstanceId,
      .candidates = std::move(candidates),
      .rulesetVersion = startPayload.rulesetVersion,
      .seed = startPayload.battleSeed});
  if (created.code != battle::BattleLoadResultCode::Ok ||
      !created.battle.has_value() ||
      created.battle->importDeterministicState(state) !=
          battle::BattleStateImportResultCode::Ok) {
    return std::nullopt;
  }
  return std::move(created.battle);
}

ReplayResult replayDecoded(std::vector<Record> records) {
  if (records.empty()) {
    return failure(ReplayErrorCode::EmptyJournal, 0U, 0U, 0U);
  }
  if (records.front().header.recordType != RecordType::BattleStart) {
    return failure(ReplayErrorCode::InvalidStart, 0U,
                   records.front().header.recordSequence,
                   records.front().header.logicalTick);
  }
  if (records.size() < 2U ||
      records[1].header.recordType != RecordType::Checkpoint) {
    return failure(ReplayErrorCode::InitialCheckpointMissing, 1U,
                   records.front().header.recordSequence,
                   records.front().header.logicalTick);
  }

  const auto &start = records.front();
  const auto &startPayload = std::get<BattleStartPayload>(start.payload);
  const auto &initialCheckpoint = records[1];
  const auto &initialPayload =
      std::get<CheckpointPayload>(initialCheckpoint.payload);
  const BattleIdentity identity{
      .originRecoveryEpoch = start.header.originRecoveryEpoch,
      .roomId = start.header.roomId,
      .battleInstanceId = start.header.battleInstanceId};
  if (startPayload.participants.size() < 2U ||
      startPayload.participants.size() > 10U ||
      startPayload.initialStateHash != initialPayload.stateHash ||
      startPayload.rulesetVersion != kSupportedBattleRulesetVersion ||
      startPayload.battleSeed == 0U) {
    return failure(ReplayErrorCode::InvalidStart, 0U,
                   start.header.recordSequence, start.header.logicalTick);
  }
  const auto initialState =
      decodeRecoveryState(initialPayload.canonicalStateBytes);
  if (!initialState.ok() || !initialState.state.has_value()) {
    return failure(ReplayErrorCode::StateDecodeRejected, 1U,
                   initialCheckpoint.header.recordSequence,
                   initialCheckpoint.header.logicalTick, std::nullopt,
                   std::nullopt, identity, startPayload.rulesetVersion);
  }
  const auto initialHash =
      recoveryStateHash(initialPayload.canonicalStateBytes);
  if (!initialHash.has_value() || *initialHash != initialPayload.stateHash) {
    return failure(ReplayErrorCode::StateHashMismatch, 1U,
                   initialCheckpoint.header.recordSequence,
                   initialCheckpoint.header.logicalTick,
                   initialPayload.stateHash, initialHash, identity,
                   startPayload.rulesetVersion);
  }
  const auto &state = initialState.state->battle;
  if (state.roomId != identity.roomId ||
      state.battleId != identity.battleInstanceId ||
      state.rulesetVersion != startPayload.rulesetVersion ||
      state.seed != startPayload.battleSeed ||
      state.battleTime.logicalTick != initialCheckpoint.header.logicalTick ||
      initialCheckpoint.header.logicalTick != 0U) {
    return failure(ReplayErrorCode::InvalidStart, 1U,
                   initialCheckpoint.header.recordSequence,
                   initialCheckpoint.header.logicalTick, std::nullopt,
                   std::nullopt, identity, startPayload.rulesetVersion);
  }

  auto battle = createReplayBattle(identity, startPayload, state);
  if (!battle.has_value()) {
    return failure(ReplayErrorCode::InvalidStart, 0U,
                   start.header.recordSequence, start.header.logicalTick,
                   std::nullopt, std::nullopt, identity,
                   startPayload.rulesetVersion);
  }
  const auto importedHash = stateHash(*battle, initialState.state->room);
  if (!importedHash.has_value() || *importedHash != initialPayload.stateHash) {
    return failure(ReplayErrorCode::StateHashMismatch, 1U,
                   initialCheckpoint.header.recordSequence,
                   initialCheckpoint.header.logicalTick,
                   initialPayload.stateHash, importedHash, identity,
                   startPayload.rulesetVersion);
  }

  RoomRecoveryState roomState = initialState.state->room;
  const auto failAfterStart =
      [&](ReplayErrorCode code, std::size_t index,
          std::optional<Hash> expected = std::nullopt,
          std::optional<Hash> actual = std::nullopt) {
        const auto &record = records[index];
        return failure(code, index, record.header.recordSequence,
                       record.header.logicalTick, std::move(expected),
                       std::move(actual), identity,
                       startPayload.rulesetVersion);
      };

  bool terminalSeen = false;
  for (std::size_t index = 2U; index < records.size(); ++index) {
    const auto &record = records[index];
    if (record.header.roomId != identity.roomId ||
        record.header.battleInstanceId != identity.battleInstanceId ||
        record.header.originRecoveryEpoch != identity.originRecoveryEpoch) {
      return failAfterStart(ReplayErrorCode::InvalidStart, index);
    }
    if (record.header.recordType == RecordType::CommandDecision) {
      const auto &payload = std::get<CommandDecisionPayload>(record.payload);
      if (!knownCommandKind(
              static_cast<CanonicalCommandKind>(payload.commandKind))) {
        return failAfterStart(ReplayErrorCode::UnknownCommandKind, index);
      }
      DecodedCommand command{};
      if (!decodeCommand(payload, record.header.recordSequence, command)) {
        return failAfterStart(ReplayErrorCode::InvalidCommand, index);
      }
      const auto before = stateHash(*battle, roomState);
      const auto decision =
          applyCommand(*battle, identity, record, payload, command);
      if (!decision.has_value() || *decision != payload.decisionCode) {
        return failAfterStart(ReplayErrorCode::CommandDecisionMismatch, index);
      }
      const auto decodedRoom =
          decodeRoomRecoveryState(payload.roomRecoveryStateBytes);
      if (!decodedRoom.ok() || !decodedRoom.state.has_value()) {
        return failAfterStart(ReplayErrorCode::StateDecodeRejected, index);
      }
      const auto afterState = battle->exportDeterministicState();
      if (afterState.roomId != decodedRoom.state->roomId) {
        return failAfterStart(ReplayErrorCode::StateDivergence, index);
      }
      const auto actual = stateHash(*battle, *decodedRoom.state);
      if (!before.has_value() || !actual.has_value()) {
        return failAfterStart(ReplayErrorCode::StateHashMismatch, index,
                              payload.postDecisionStateHash, actual);
      }
      if (*before == *actual) {
        return failAfterStart(ReplayErrorCode::NoMutationRecord, index,
                              payload.postDecisionStateHash, actual);
      }
      if (*actual != payload.postDecisionStateHash) {
        return failAfterStart(ReplayErrorCode::StateDivergence, index,
                              payload.postDecisionStateHash, actual);
      }
      roomState = *decodedRoom.state;
    } else if (record.header.recordType == RecordType::Checkpoint) {
      const auto &payload = std::get<CheckpointPayload>(record.payload);
      const auto decodedCheckpoint =
          decodeRecoveryState(payload.canonicalStateBytes);
      if (!decodedCheckpoint.ok() || !decodedCheckpoint.state.has_value()) {
        return failAfterStart(ReplayErrorCode::StateDecodeRejected, index);
      }
      const auto actualBytes = encodedState(*battle, roomState);
      const auto actualHash = stateHash(*battle, roomState);
      const auto currentState = battle->exportDeterministicState();
      if (!actualBytes.has_value() || !actualHash.has_value() ||
          currentState.battleTime.logicalTick != record.header.logicalTick ||
          currentState.battleTime.battleElapsedNanos !=
              record.header.battleElapsedNanos ||
          *actualHash != payload.stateHash ||
          *actualBytes != payload.canonicalStateBytes) {
        return failAfterStart(ReplayErrorCode::CheckpointDivergence, index,
                              payload.stateHash, actualHash);
      }
      roomState = decodedCheckpoint.state->room;
    } else if (record.header.recordType == RecordType::TerminalReceipt) {
      const auto &payload = std::get<TerminalReceiptPayload>(record.payload);
      if (terminalSeen) {
        return failAfterStart(ReplayErrorCode::DuplicateTerminalReceipt,
                              index);
      }
      terminalSeen = true;
      const auto actualBytes = encodedState(*battle, roomState);
      const auto actualHash = stateHash(*battle, roomState);
      const auto currentState = battle->exportDeterministicState();
      const auto actualResult = currentState.committedResult;
      const auto encodedResult =
          actualResult.has_value() ? encodeCanonicalBattleResult(*actualResult)
                                   : BattleResultEncodeResult{};
      const auto terminalHash =
          actualBytes.has_value()
              ? canonicalRecoveryTerminalHash(*actualBytes, payload)
              : std::nullopt;
      if (!actualHash.has_value() || !terminalHash.has_value() ||
          currentState.battleTime.logicalTick != record.header.logicalTick ||
          currentState.battleTime.battleElapsedNanos !=
              record.header.battleElapsedNanos ||
          !settlementSlotsMatch(payload, currentState) ||
          *terminalHash != payload.finalStateHash ||
          !actualResult.has_value() || !encodedResult.ok() ||
          encodedResult.bytes != payload.canonicalResultPayload) {
        return failAfterStart(ReplayErrorCode::TerminalReceiptMismatch, index,
                              payload.finalStateHash, terminalHash);
      }
    }
  }

  const auto finalHash = stateHash(*battle, roomState);
  if (!finalHash.has_value()) {
    return failAfterStart(ReplayErrorCode::StateHashMismatch,
                          records.size() - 1U);
  }
  return ReplayResult{.battle = std::move(*battle),
                      .finalStateHash = finalHash,
                      .finalRoomRecoveryState = std::move(roomState),
                      .error = std::nullopt};
}

} // namespace

ReplayResult
BattleReplayer::replayJournal(std::span<const std::uint8_t> encodedJournal) {
  const auto decoded = decodeJournal(encodedJournal);
  if (!decoded.ok()) {
    const auto code =
        decoded.error->code == CodecErrorCode::UnsupportedRulesetVersion
            ? ReplayErrorCode::UnsupportedRuleset
            : ReplayErrorCode::CodecRejected;
    return failure(code, 0U, 0U, 0U);
  }
  return replayDecoded(decoded.records);
}

ReplayResult
BattleReplayer::restoreJournal(std::span<const std::uint8_t> encodedJournal) {
  // Validate every committed record first.  This keeps a later checkpoint
  // from hiding an earlier command, checkpoint, or terminal divergence.
  auto fullValidation = replayJournal(encodedJournal);
  if (!fullValidation.ok()) {
    return fullValidation;
  }

  const auto decoded = decodeJournal(encodedJournal);
  if (!decoded.ok()) {
    const auto code =
        decoded.error->code == CodecErrorCode::UnsupportedRulesetVersion
            ? ReplayErrorCode::UnsupportedRuleset
            : ReplayErrorCode::CodecRejected;
    return failure(code, 0U, 0U, 0U);
  }
  auto records = decoded.records;
  if (records.empty() || !fullValidation.finalStateHash.has_value() ||
      !fullValidation.finalRoomRecoveryState.has_value()) {
    return failure(ReplayErrorCode::EmptyJournal, 0U, 0U, 0U);
  }

  std::size_t lastCheckpointIndex = 0U;
  bool checkpointFound = false;
  for (std::size_t index = 0U; index < records.size(); ++index) {
    if (records[index].header.recordType == RecordType::Checkpoint) {
      lastCheckpointIndex = index;
      checkpointFound = true;
    }
  }
  if (!checkpointFound) {
    return failure(ReplayErrorCode::InitialCheckpointMissing, 1U,
                   records.front().header.recordSequence,
                   records.front().header.logicalTick);
  }

  const auto &start = records.front();
  const auto &startPayload = std::get<BattleStartPayload>(start.payload);
  const BattleIdentity identity{
      .originRecoveryEpoch = start.header.originRecoveryEpoch,
      .roomId = start.header.roomId,
      .battleInstanceId = start.header.battleInstanceId};
  const auto &checkpoint = records[lastCheckpointIndex];
  const auto &checkpointPayload =
      std::get<CheckpointPayload>(checkpoint.payload);
  const auto checkpointState =
      decodeRecoveryState(checkpointPayload.canonicalStateBytes);
  if (!checkpointState.ok() || !checkpointState.state.has_value()) {
    return failure(ReplayErrorCode::StateDecodeRejected, lastCheckpointIndex,
                   checkpoint.header.recordSequence,
                   checkpoint.header.logicalTick, std::nullopt, std::nullopt,
                   identity, startPayload.rulesetVersion);
  }
  const auto checkpointHash =
      recoveryStateHash(checkpointPayload.canonicalStateBytes);
  if (!checkpointHash.has_value() ||
      *checkpointHash != checkpointPayload.stateHash ||
      checkpointState.state->battle.battleTime.logicalTick !=
          checkpoint.header.logicalTick ||
      checkpointState.state->battle.battleTime.battleElapsedNanos !=
          checkpoint.header.battleElapsedNanos) {
    return failure(ReplayErrorCode::CheckpointDivergence, lastCheckpointIndex,
                   checkpoint.header.recordSequence,
                   checkpoint.header.logicalTick, checkpointPayload.stateHash,
                   checkpointHash, identity, startPayload.rulesetVersion);
  }

  auto restored =
      createReplayBattle(identity, startPayload, checkpointState.state->battle);
  if (!restored.has_value()) {
    return failure(ReplayErrorCode::StateDecodeRejected, lastCheckpointIndex,
                   checkpoint.header.recordSequence,
                   checkpoint.header.logicalTick, std::nullopt, std::nullopt,
                   identity, startPayload.rulesetVersion);
  }
  RoomRecoveryState roomState = checkpointState.state->room;
  const auto importedCheckpointHash = stateHash(*restored, roomState);
  if (!importedCheckpointHash.has_value() ||
      *importedCheckpointHash != checkpointPayload.stateHash) {
    return failure(ReplayErrorCode::CheckpointDivergence, lastCheckpointIndex,
                   checkpoint.header.recordSequence,
                   checkpoint.header.logicalTick, checkpointPayload.stateHash,
                   importedCheckpointHash, identity,
                   startPayload.rulesetVersion);
  }

  const auto failAfterStart =
      [&](ReplayErrorCode code, std::size_t index,
          std::optional<Hash> expected = std::nullopt,
          std::optional<Hash> actual = std::nullopt) {
        const auto &record = records[index];
        return failure(code, index, record.header.recordSequence,
                       record.header.logicalTick, std::move(expected),
                       std::move(actual), identity,
                       startPayload.rulesetVersion);
      };

  for (std::size_t index = lastCheckpointIndex + 1U; index < records.size();
       ++index) {
    const auto &record = records[index];
    if (record.header.recordType != RecordType::CommandDecision) {
      continue;
    }
    const auto &payload = std::get<CommandDecisionPayload>(record.payload);
    if (!knownCommandKind(
            static_cast<CanonicalCommandKind>(payload.commandKind))) {
      return failAfterStart(ReplayErrorCode::UnknownCommandKind, index);
    }
    DecodedCommand command{};
    if (!decodeCommand(payload, record.header.recordSequence, command)) {
      return failAfterStart(ReplayErrorCode::InvalidCommand, index);
    }
    const auto before = stateHash(*restored, roomState);
    const auto decision =
        applyCommand(*restored, identity, record, payload, command);
    if (!decision.has_value() || *decision != payload.decisionCode) {
      return failAfterStart(ReplayErrorCode::CommandDecisionMismatch, index);
    }
    const auto decodedRoom =
        decodeRoomRecoveryState(payload.roomRecoveryStateBytes);
    if (!decodedRoom.ok() || !decodedRoom.state.has_value()) {
      return failAfterStart(ReplayErrorCode::StateDecodeRejected, index);
    }
    const auto actual = stateHash(*restored, *decodedRoom.state);
    if (!before.has_value() || !actual.has_value()) {
      return failAfterStart(ReplayErrorCode::StateHashMismatch, index,
                            payload.postDecisionStateHash, actual);
    }
    if (*before == *actual) {
      return failAfterStart(ReplayErrorCode::NoMutationRecord, index,
                            payload.postDecisionStateHash, actual);
    }
    if (*actual != payload.postDecisionStateHash) {
      return failAfterStart(ReplayErrorCode::StateDivergence, index,
                            payload.postDecisionStateHash, actual);
    }
    roomState = *decodedRoom.state;
  }

  const auto restoredFinalHash = stateHash(*restored, roomState);
  if (!restoredFinalHash.has_value() ||
      *restoredFinalHash != *fullValidation.finalStateHash) {
    return failure(ReplayErrorCode::StateDivergence, records.size() - 1U,
                   records.back().header.recordSequence,
                   records.back().header.logicalTick,
                   fullValidation.finalStateHash, restoredFinalHash, identity,
                   startPayload.rulesetVersion);
  }
  return ReplayResult{.battle = std::move(*restored),
                      .finalStateHash = restoredFinalHash,
                      .finalRoomRecoveryState = std::move(roomState),
                      .error = std::nullopt};
}

ReplayResult BattleReplayer::replayRecords(std::span<const Record> records) {
  Bytes encoded;
  for (const auto &record : records) {
    const auto result = encodeRecord(record);
    if (!result.ok()) {
      return failure(ReplayErrorCode::CodecRejected, 0U,
                     record.header.recordSequence, record.header.logicalTick);
    }
    encoded.insert(encoded.end(), result.bytes.begin(), result.bytes.end());
  }
  return replayJournal(encoded);
}

} // namespace lol::battle_continuity
