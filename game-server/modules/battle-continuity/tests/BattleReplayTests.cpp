#include <lol/battle/BattleLoadApi.hpp>
#include <lol/battle_continuity/BattleReplay.hpp>
#include <lol/battle_continuity/BattleStateCodec.hpp>
#include <lol/battle_continuity/FlightRecorder.hpp>
#include <lol/battle_continuity/RecordCodec.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {

using lol::battle::ArenaLoadCompleteCommand;
using lol::battle::AttackCommand;
using lol::battle::BattleAdmissionSnapshot;
using lol::battle::BattleInstance;
using lol::battle::BattleLoadResultCode;
using lol::battle::BattleStartCandidate;
using lol::battle::BattleTime;
using lol::battle::CombatRuleset;
using lol::battle::DirectionIntent;
using lol::battle::MoveCommand;
using lol::battle::MovementTickCommand;
using lol::battle_continuity::BattleIdentity;
using lol::battle_continuity::BattleReplayer;
using lol::battle_continuity::BattleRecording;
using lol::battle_continuity::CanonicalCommand;
using lol::battle_continuity::FlightRecorder;
using lol::battle_continuity::Record;
using lol::battle_continuity::RecordType;
using lol::battle_continuity::RoomRecoveryPhase;
using lol::battle_continuity::RoomRecoveryState;
using lol::shared::AccountId;
using lol::shared::BattleInstanceId;
using lol::shared::RoomId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;

constexpr std::uint64_t roomValue = (9ULL << 32U) | 1ULL;

AccountId account(std::uint8_t suffix) {
  AccountId::Bytes bytes{};
  bytes.back() = suffix;
  return AccountId{bytes};
}

BattleIdentity identity() {
  return BattleIdentity{.originRecoveryEpoch = 9,
                        .roomId = RoomId{roomValue},
                        .battleInstanceId = BattleInstanceId{1}};
}

RoomRecoveryState roomState() {
  return RoomRecoveryState{.roomId = identity().roomId,
                           .capacity = 2U,
                           .hostParticipantSlot = 1U,
                           .memberSlots = {1U, 2U},
                           .phase = RoomRecoveryPhase::Loading,
                           .nextBattleOrdinal = 2U};
}

BattleInstance openedBattle() {
  auto result = BattleInstance::create(BattleAdmissionSnapshot{
      .roomId = RoomId{roomValue},
      .battleId = BattleInstanceId{1},
      .candidates =
          {
              BattleStartCandidate{account(1), SessionId{11},
                                   SessionGeneration{3}, "private-one"},
              BattleStartCandidate{account(2), SessionId{12},
                                   SessionGeneration{4}, "private-two"},
          },
      .rulesetVersion = 1,
      .seed = 17,
  });
  if (result.code != BattleLoadResultCode::Ok || !result.battle.has_value()) {
    std::abort();
  }
  auto battle = std::move(*result.battle);
  if (battle.openLoadBarrier(BattleTime::fromLogicalTick(0)) !=
      BattleLoadResultCode::Ok) {
    std::abort();
  }
  return battle;
}

bool appendLoad(FlightRecorder &recorder, BattleInstance &battle,
                std::uint64_t tick, std::uint16_t slot, std::uint64_t session,
                std::uint64_t generation) {
  const auto before = battle.exportDeterministicState();
  if (battle.completeLoad(
          ArenaLoadCompleteCommand{.sessionId = SessionId{session},
                                   .generation = SessionGeneration{generation},
                                   .roomId = RoomId{roomValue},
                                   .battleId = BattleInstanceId{1}},
          true,
          BattleTime::fromLogicalTick(tick)) != BattleLoadResultCode::Ok) {
    return false;
  }
  return recorder
      .appendCommand(CanonicalCommand::arenaLoadComplete(slot), before,
                     roomState(), battle, roomState(), 0, {})
      .recorded;
}

bool buildMovementJournal(FlightRecorder &recorder, BattleInstance &battle) {
  if (!recorder.commitTick().ok() ||
      !appendLoad(recorder, battle, 1, 1, 11, 3) ||
      !appendLoad(recorder, battle, 1, 2, 12, 4) ||
      !recorder.commitTick().ok()) {
    return false;
  }
  const auto moveBefore = battle.exportDeterministicState();
  const MoveCommand move{.sessionId = SessionId{11},
                         .generation = SessionGeneration{3},
                         .battleId = BattleInstanceId{1},
                         .actionSequence = 0,
                         .direction = DirectionIntent{1, 0, 0}};
  if (battle.acceptMove(move, BattleTime::fromLogicalTick(2)) !=
          lol::battle::MovementResultCode::Ok ||
      !recorder
           .appendCommand(
               CanonicalCommand::move(
                   1, lol::battle_continuity::CommandId{0, 0}, move.direction),
               moveBefore, roomState(), battle, roomState(), 0, {})
           .recorded) {
    return false;
  }

  const auto tickBefore = battle.exportDeterministicState();
  if (battle.integrateMovement(MovementTickCommand{BattleInstanceId{1}, 2},
                               BattleTime::fromLogicalTick(2)) !=
          lol::battle::MovementResultCode::Ok ||
      !battle.captureStateSnapshot().has_value()) {
    return false;
  }
  if (!recorder
           .appendCommand(CanonicalCommand::movementTick(2), tickBefore,
                          roomState(), battle, roomState(), 0, {})
           .recorded ||
      !recorder.commitTick().ok()) {
    return false;
  }

  const auto moveAgainBefore = battle.exportDeterministicState();
  const MoveCommand moveAgain{.sessionId = SessionId{11},
                              .generation = SessionGeneration{3},
                              .battleId = BattleInstanceId{1},
                              .actionSequence = 2,
                              .direction = DirectionIntent{-1, 0, 0}};
  if (battle.acceptMove(moveAgain, BattleTime::fromLogicalTick(3)) !=
          lol::battle::MovementResultCode::Ok ||
      !recorder
           .appendCommand(CanonicalCommand::move(
                              1, lol::battle_continuity::CommandId{0, 2},
                              moveAgain.direction),
                          moveAgainBefore, roomState(), battle, roomState(), 0,
                          {})
           .recorded) {
    return false;
  }
  return true;
}

bool replayUsesProductionBattleAndMatchesFinalHash() {
  auto battle = openedBattle();
  auto recorder = FlightRecorder::start(battle, identity(), 2, roomState());
  if (!recorder.has_value() || !buildMovementJournal(*recorder, battle) ||
      !recorder->commitTick().ok()) {
    return false;
  }
  const auto expected = lol::battle_continuity::recoveryStateHash(
                            lol::battle_continuity::encodeRecoveryState(
                                battle.exportDeterministicState(), roomState())
                                .bytes)
                            .value();
  const auto replay =
      BattleReplayer::replayJournal(recorder->encodeJournal().bytes);
  return replay.ok() && replay.finalStateHash.has_value() &&
         *replay.finalStateHash == expected && replay.battle.has_value() &&
         replay.finalRoomRecoveryState.has_value() &&
         *replay.finalRoomRecoveryState == roomState();
}

bool oddMovementTickDoesNotInventSnapshotMutation() {
  auto battle = openedBattle();
  auto recorder = FlightRecorder::start(battle, identity(), 9U, roomState());
  if (!recorder.has_value() || !recorder->commitTick().ok() ||
      !appendLoad(*recorder, battle, 1, 1, 11, 3) ||
      !appendLoad(*recorder, battle, 1, 2, 12, 4) ||
      !recorder->commitTick().ok()) {
    return false;
  }
  const auto before = battle.exportDeterministicState();
  if (battle.integrateMovement(MovementTickCommand{BattleInstanceId{1}, 1},
                               BattleTime::fromLogicalTick(2)) !=
          lol::battle::MovementResultCode::Ok ||
      !recorder
           ->appendCommand(CanonicalCommand::movementTick(1), before,
                           roomState(), battle, roomState(), 0, {})
           .recorded ||
      !recorder->commitTick().ok()) {
    return false;
  }
  const auto replay =
      BattleReplayer::replayJournal(recorder->encodeJournal().bytes);
  return replay.ok() && replay.battle.has_value() &&
         replay.battle->exportDeterministicState() ==
             battle.exportDeterministicState();
}

bool tamperedCommandReportsFirstDivergence() {
  auto battle = openedBattle();
  auto recorder = FlightRecorder::start(battle, identity(), 2, roomState());
  if (!recorder.has_value() || !buildMovementJournal(*recorder, battle) ||
      !recorder->commitTick().ok()) {
    return false;
  }
  auto records = recorder->records();
  for (auto &record : records) {
    if (record.header.recordType != RecordType::CommandDecision) {
      continue;
    }
    auto &command = std::get<lol::battle_continuity::CommandDecisionPayload>(
        record.payload);
    if (command.commandKind ==
        static_cast<std::uint16_t>(
            lol::battle_continuity::CanonicalCommandKind::Move)) {
      command.commandPayload[6] = 1;
      break;
    }
  }
  const auto replay = BattleReplayer::replayRecords(records);
  return !replay.ok() && replay.error.has_value() &&
         replay.error->code ==
             lol::battle_continuity::ReplayErrorCode::StateDivergence &&
         replay.error->recordSequence != 0 && replay.error->logicalTick == 2;
}

bool tamperedRoomStateReportsFirstDivergenceWithContext() {
  auto battle = openedBattle();
  auto recorder = FlightRecorder::start(battle, identity(), 2, roomState());
  if (!recorder.has_value() || !buildMovementJournal(*recorder, battle) ||
      !recorder->commitTick().ok()) {
    return false;
  }
  auto records = recorder->records();
  std::uint64_t expectedSequence = 0U;
  std::uint64_t expectedTick = 0U;
  for (auto &record : records) {
    if (record.header.recordType != RecordType::CommandDecision) {
      continue;
    }
    auto &payload =
        std::get<lol::battle_continuity::CommandDecisionPayload>(
            record.payload);
    auto room =
        lol::battle_continuity::decodeRoomRecoveryState(
            payload.roomRecoveryStateBytes);
    if (!room.ok() || !room.state.has_value()) {
      return false;
    }
    room.state->phase = RoomRecoveryPhase::InProgress;
    const auto encodedRoom =
        lol::battle_continuity::encodeRoomRecoveryState(*room.state);
    if (!encodedRoom.ok()) {
      return false;
    }
    payload.roomRecoveryStateBytes = encodedRoom.bytes;
    expectedSequence = record.header.recordSequence;
    expectedTick = record.header.logicalTick;
    break;
  }
  const auto replay = BattleReplayer::replayRecords(records);
  return expectedSequence != 0U && !replay.ok() && replay.error.has_value() &&
         replay.error->code ==
             lol::battle_continuity::ReplayErrorCode::StateDivergence &&
         replay.error->recordSequence == expectedSequence &&
         replay.error->logicalTick == expectedTick &&
         replay.error->expectedHash.has_value() &&
         replay.error->actualHash.has_value() &&
         replay.error->identity.has_value() &&
         *replay.error->identity == identity() &&
         replay.error->rulesetVersion.has_value() &&
         *replay.error->rulesetVersion == 1U;
}

bool unsupportedRecordSchemaIsExplicitlyRejected() {
  auto battle = openedBattle();
  auto recorder = FlightRecorder::start(battle, identity(), 2, roomState());
  if (!recorder.has_value() || !recorder->commitTick().ok()) {
    return false;
  }
  auto bytes = recorder->encodeJournal().bytes;
  if (bytes.size() < 6U) {
    return false;
  }
  bytes[4] = 0U;
  bytes[5] = 2U;
  const auto replay = BattleReplayer::replayJournal(bytes);
  return !replay.ok() && replay.error.has_value() &&
         replay.error->code ==
             lol::battle_continuity::ReplayErrorCode::CodecRejected &&
         !replay.error->identity.has_value() &&
         !replay.error->rulesetVersion.has_value();
}

bool resumeRejectsTamperedRoomState() {
  auto battle = openedBattle();
  auto recorder = FlightRecorder::start(battle, identity(), 2, roomState());
  if (!recorder.has_value() || !buildMovementJournal(*recorder, battle) ||
      !recorder->commitTick().ok()) {
    return false;
  }
  auto records = recorder->records();
  for (auto &record : records) {
    if (record.header.recordType != RecordType::CommandDecision) {
      continue;
    }
    auto &payload =
        std::get<lol::battle_continuity::CommandDecisionPayload>(
            record.payload);
    auto room = lol::battle_continuity::decodeRoomRecoveryState(
        payload.roomRecoveryStateBytes);
    if (!room.ok() || !room.state.has_value()) {
      return false;
    }
    room.state->nextBattleOrdinal++;
    payload.roomRecoveryStateBytes =
        lol::battle_continuity::encodeRoomRecoveryState(*room.state).bytes;
    break;
  }
  return !BattleRecording::resume(battle, records, 3U).has_value();
}

bool commandOrderReportsFirstDivergence() {
  auto battle = openedBattle();
  auto recorder = FlightRecorder::start(battle, identity(), 2, roomState());
  if (!recorder.has_value() || !buildMovementJournal(*recorder, battle) ||
      !recorder->commitTick().ok()) {
    return false;
  }
  auto records = recorder->records();
  std::vector<std::size_t> moveRecords;
  for (std::size_t index = 0U; index < records.size(); ++index) {
    if (records[index].header.recordType != RecordType::CommandDecision) {
      continue;
    }
    const auto &payload =
        std::get<lol::battle_continuity::CommandDecisionPayload>(
            records[index].payload);
    if (payload.commandKind ==
        static_cast<std::uint16_t>(
            lol::battle_continuity::CanonicalCommandKind::Move)) {
      moveRecords.push_back(index);
    }
  }
  if (moveRecords.size() != 2U) {
    return false;
  }
  std::swap(std::get<lol::battle_continuity::CommandDecisionPayload>(
                records[moveRecords[0]].payload),
            std::get<lol::battle_continuity::CommandDecisionPayload>(
                records[moveRecords[1]].payload));
  auto &commit = std::get<lol::battle_continuity::TickCommitPayload>(
      records.back().payload);
  commit.committedStateHash =
      std::get<lol::battle_continuity::CommandDecisionPayload>(
          records[moveRecords[1]].payload)
          .postDecisionStateHash;
  const auto replay = BattleReplayer::replayRecords(records);
  return !replay.ok() && replay.error.has_value() &&
         replay.error->code ==
             lol::battle_continuity::ReplayErrorCode::StateDivergence &&
         replay.error->recordSequence ==
             records[moveRecords[0]].header.recordSequence &&
         replay.error->logicalTick ==
             records[moveRecords[0]].header.logicalTick;
}

bool unknownRulesetIsRefusedBeforeReplay() {
  auto battle = openedBattle();
  auto recorder = FlightRecorder::start(battle, identity(), 2, roomState());
  if (!recorder.has_value() || !recorder->commitTick().ok()) {
    return false;
  }
  auto bytes = recorder->encodeJournal().bytes;
  // BattleStart payload rulesetVersion starts immediately after the 64-byte
  // record envelope. Recompute the record checksum so the ruleset check, not
  // the checksum check, is the observed failure.
  bytes[64] = 0;
  bytes[65] = 0;
  bytes[66] = 0;
  bytes[67] = 2;
  const auto length = (static_cast<std::uint32_t>(bytes[8]) << 24U) |
                      (static_cast<std::uint32_t>(bytes[9]) << 16U) |
                      (static_cast<std::uint32_t>(bytes[10]) << 8U) |
                      static_cast<std::uint32_t>(bytes[11]);
  const auto checksum = lol::battle_continuity::crc32(
      std::span<const std::uint8_t>{bytes.data(), length - 4U});
  bytes[length - 4U] = static_cast<std::uint8_t>(checksum >> 24U);
  bytes[length - 3U] = static_cast<std::uint8_t>(checksum >> 16U);
  bytes[length - 2U] = static_cast<std::uint8_t>(checksum >> 8U);
  bytes[length - 1U] = static_cast<std::uint8_t>(checksum);
  const auto replay = BattleReplayer::replayJournal(bytes);
  return !replay.ok() && replay.error.has_value() &&
         replay.error->code ==
             lol::battle_continuity::ReplayErrorCode::UnsupportedRuleset;
}

bool terminalReceiptReplaysResultAndSettlementIdentity() {
  auto battle = openedBattle();
  auto recorder = FlightRecorder::start(battle, identity(), 2, roomState());
  if (!recorder.has_value() || !recorder->commitTick().ok() ||
      !appendLoad(*recorder, battle, 1, 1, 11, 3) ||
      !appendLoad(*recorder, battle, 1, 2, 12, 4) ||
      !recorder->commitTick().ok()) {
    return false;
  }
  for (std::uint64_t index = 0U; index < 16U; ++index) {
    const auto tick = 1U + index * 16U;
    const auto before = battle.exportDeterministicState();
    const auto commandId = lol::battle::CommandId{.high = 0x1234000000000000ULL,
                                                  .low = index + 1U};
    const auto result = battle.attackWithApplied(
        lol::battle::AttackCommand{.commandId = commandId,
                                   .sessionId = SessionId{11},
                                   .generation = SessionGeneration{3},
                                   .battleId = BattleInstanceId{1},
                                   .targetHint = CombatRuleset::monsterId},
        BattleTime::fromLogicalTick(tick));
    if (result.result.code != lol::battle::AttackResultCode::Ok ||
        !recorder
             ->appendCommand(
                 CanonicalCommand::attack(
                     1,
                     lol::battle_continuity::CommandId{.high = commandId.high,
                                                       .low = commandId.low},
                     CombatRuleset::monsterId),
                 before, roomState(), battle, roomState(), 0, {})
             .recorded ||
        !recorder->commitTick().ok()) {
      return false;
    }
  }

  const auto claimBefore = battle.exportDeterministicState();
  const auto claimId =
      lol::battle::CommandId{.high = 0x9988000000000000ULL, .low = 1U};
  const auto claim = battle.claimLoot(
      lol::battle::ClaimLootCommand{.commandId = claimId,
                                    .sessionId = SessionId{11},
                                    .generation = SessionGeneration{3},
                                    .battleId = BattleInstanceId{1},
                                    .dropId = lol::battle::DropId{0U}},
      BattleTime::fromLogicalTick(300U));
  if (claim.code != lol::battle::ClaimLootResultCode::InvalidDrop) {
    return false;
  }
  const auto claimRecord = recorder->appendCommand(
      CanonicalCommand::claimLoot(1,
                                  lol::battle_continuity::CommandId{
                                      .high = claimId.high, .low = claimId.low},
                                  0U),
      claimBefore, roomState(), battle, roomState(),
      static_cast<std::uint16_t>(claim.code), {});
  if (!claimRecord.recorded || !recorder->commitTick().ok()) {
    return false;
  }

  const auto deadlineBefore = battle.exportDeterministicState();
  const auto deadlineTick = BattleTime::fromLogicalTick(541U);
  if (battle.expireLoot(
          lol::battle::LootDeadlineCommand{.battleId = BattleInstanceId{1}},
          deadlineTick) != lol::battle::LootDeadlineResultCode::Ok ||
      !recorder
           ->appendCommand(CanonicalCommand::lootDeadline(), deadlineBefore,
                           roomState(), battle, roomState(), 0, {})
           .recorded) {
    return false;
  }
  const auto settlements =
      std::vector<lol::battle_continuity::SettlementReceipt>{
          {.participantSlot = 1U,
           .settlementId = lol::battle_continuity::SettlementId{0x11U},
           .settlementPayloadHash = lol::battle_continuity::Hash{0xa1U}},
          {.participantSlot = 2U,
           .settlementId = lol::battle_continuity::SettlementId{0x12U},
           .settlementPayloadHash = lol::battle_continuity::Hash{0xa2U}}};
  if (!recorder
           ->appendTerminal(7U, battle, roomState(), 1'700'000'000'000ULL,
                            lol::battle_continuity::SettlementBatchId{0xb1U},
                            settlements)
           .recorded) {
    return false;
  }
  const auto prematureCommit = recorder->commitTick();
  if (prematureCommit.ok() || !prematureCommit.error.has_value() ||
      prematureCommit.error->code !=
          lol::battle_continuity::RecorderErrorCode::BatchNotCommitted ||
      !recorder->appendCheckpoint(battle, roomState()).recorded ||
      !recorder->commitTick().ok()) {
    return false;
  }
  const auto replay =
      BattleReplayer::replayJournal(recorder->encodeJournal().bytes);
  if (!replay.ok() || !replay.battle.has_value() ||
      replay.battle->resultProjection().state !=
          lol::battle::BattleResultState::Committed) {
    return false;
  }
  const auto expected = lol::battle_continuity::recoveryStateHash(
                            lol::battle_continuity::encodeRecoveryState(
                                battle.exportDeterministicState(), roomState())
                                .bytes)
                            .value();
  return replay.finalStateHash.has_value() &&
         *replay.finalStateHash == expected &&
         replay.battle->exportDeterministicState().committedResult ==
             battle.exportDeterministicState().committedResult &&
         replay.battle->lootProjection() == battle.lootProjection();
}

bool restoreStartsAtLastCheckpointAndReplaysTail() {
  auto battle = openedBattle();
  auto recorder = FlightRecorder::start(battle, identity(), 2U, roomState());
  if (!recorder.has_value() || !buildMovementJournal(*recorder, battle) ||
      !recorder->commitTick().ok() ||
      !recorder->appendCheckpoint(battle, roomState()).recorded ||
      !recorder->commitTick().ok()) {
    return false;
  }

  const auto before = battle.exportDeterministicState();
  if (battle.integrateMovement(MovementTickCommand{BattleInstanceId{1}, 4},
                               BattleTime::fromLogicalTick(4)) !=
          lol::battle::MovementResultCode::Ok ||
      !battle.captureStateSnapshot().has_value() ||
      !recorder
           ->appendCommand(CanonicalCommand::movementTick(4), before,
                           roomState(), battle, roomState(), 0, {})
           .recorded ||
      !recorder->commitTick().ok()) {
    return false;
  }

  const auto encoded = recorder->encodeJournal();
  if (!encoded.ok()) {
    return false;
  }
  const auto full = BattleReplayer::replayJournal(encoded.bytes);
  const auto restored = BattleReplayer::restoreJournal(encoded.bytes);
  return full.ok() && restored.ok() && full.finalStateHash.has_value() &&
         restored.finalStateHash.has_value() &&
         *full.finalStateHash == *restored.finalStateHash &&
         restored.battle.has_value() &&
         restored.battle->exportDeterministicState() ==
             battle.exportDeterministicState();
}

bool restoreValidatesHistoryBeforeLastCheckpoint() {
  auto battle = openedBattle();
  auto recorder = FlightRecorder::start(battle, identity(), 2U, roomState());
  if (!recorder.has_value() || !buildMovementJournal(*recorder, battle) ||
      !recorder->commitTick().ok() ||
      !recorder->appendCheckpoint(battle, roomState()).recorded ||
      !recorder->commitTick().ok()) {
    return false;
  }

  const auto before = battle.exportDeterministicState();
  if (battle.integrateMovement(MovementTickCommand{BattleInstanceId{1}, 4},
                               BattleTime::fromLogicalTick(4)) !=
          lol::battle::MovementResultCode::Ok ||
      !battle.captureStateSnapshot().has_value() ||
      !recorder
           ->appendCommand(CanonicalCommand::movementTick(4), before,
                           roomState(), battle, roomState(), 0, {})
           .recorded ||
      !recorder->commitTick().ok()) {
    return false;
  }

  auto records = recorder->records();
  std::uint64_t corruptedSequence = 0U;
  for (auto &record : records) {
    if (record.header.recordType != RecordType::CommandDecision) {
      continue;
    }
    auto &payload = std::get<lol::battle_continuity::CommandDecisionPayload>(
        record.payload);
    payload.postDecisionStateHash[0] ^= 0x01U;
    corruptedSequence = record.header.recordSequence;
    break;
  }
  if (corruptedSequence == 0U) {
    return false;
  }

  lol::battle_continuity::Bytes encoded;
  for (const auto &record : records) {
    const auto recordBytes = lol::battle_continuity::encodeRecord(record);
    if (!recordBytes.ok()) {
      return false;
    }
    encoded.insert(encoded.end(), recordBytes.bytes.begin(),
                   recordBytes.bytes.end());
  }
  const auto restored = BattleReplayer::restoreJournal(encoded);
  return !restored.ok() && restored.error.has_value() &&
         restored.error->code ==
             lol::battle_continuity::ReplayErrorCode::StateDivergence &&
         restored.error->recordSequence == corruptedSequence;
}

} // namespace

int main() {
  if (!replayUsesProductionBattleAndMatchesFinalHash() ||
      !oddMovementTickDoesNotInventSnapshotMutation() ||
      !tamperedCommandReportsFirstDivergence() ||
      !tamperedRoomStateReportsFirstDivergenceWithContext() ||
      !unsupportedRecordSchemaIsExplicitlyRejected() ||
      !resumeRejectsTamperedRoomState() ||
      !commandOrderReportsFirstDivergence() ||
      !unknownRulesetIsRefusedBeforeReplay() ||
      !terminalReceiptReplaysResultAndSettlementIdentity() ||
      !restoreStartsAtLastCheckpointAndReplaysTail() ||
      !restoreValidatesHistoryBeforeLastCheckpoint()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
