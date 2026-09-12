#include <lol/battle/BattleLoadApi.hpp>
#include <lol/battle/CombatApi.hpp>
#include <lol/battle/MovementApi.hpp>
#include <lol/battle_continuity/BattleReplay.hpp>
#include <lol/battle_continuity/FlightRecorder.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace {

using lol::battle::ArenaLoadCompleteCommand;
using lol::battle::BattleAdmissionSnapshot;
using lol::battle::BattleInstance;
using lol::battle::BattleLoadResultCode;
using lol::battle::BattleStartCandidate;
using lol::battle::BattleTime;
using lol::battle_continuity::BattleIdentity;
using lol::battle_continuity::BattleRecording;
using lol::battle_continuity::CanonicalCommand;
using lol::battle_continuity::FlightRecorder;
using lol::battle_continuity::RecorderErrorCode;
using lol::battle_continuity::RoomRecoveryPhase;
using lol::battle_continuity::RoomRecoveryState;
using lol::shared::AccountId;
using lol::shared::BattleInstanceId;
using lol::shared::RoomId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;

AccountId account(std::uint8_t suffix) {
  AccountId::Bytes bytes{};
  bytes.back() = suffix;
  return AccountId{bytes};
}

BattleInstance createdBattle() {
  constexpr std::uint64_t roomValue = (9ULL << 32U) | 1ULL;
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
      .rulesetVersion = lol::battle::battleRulesetVersion,
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

BattleIdentity identity() {
  return BattleIdentity{
      .originRecoveryEpoch = 9,
      .roomId = RoomId{(9ULL << 32U) | 1ULL},
      .battleInstanceId = BattleInstanceId{1},
  };
}

RoomRecoveryState roomState() {
  return RoomRecoveryState{.roomId = identity().roomId,
                           .capacity = 2U,
                           .hostParticipantSlot = 1U,
                           .memberSlots = {1U, 2U},
                           .phase = RoomRecoveryPhase::Loading,
                           .nextBattleOrdinal = 2U};
}

bool recorderEmitsOnlyAStateMutation() {
  auto battle = createdBattle();
  auto started = FlightRecorder::start(battle, identity(), 2, roomState());
  if (!started.has_value()) {
    return false;
  }
  auto recorder = std::move(*started);
  const auto before = battle.exportDeterministicState();

  const auto noMutation =
      recorder.appendCommand(CanonicalCommand::suspendInput(1), before,
                             roomState(), battle, roomState(), 0, {});
  if (noMutation.recorded || !noMutation.ok()) {
    return false;
  }
  if (!recorder.commitTick().ok()) {
    return false;
  }

  const auto mutationBefore = battle.exportDeterministicState();
  if (battle.completeLoad(
          ArenaLoadCompleteCommand{.sessionId = SessionId{11},
                                   .generation = SessionGeneration{3},
                                   .roomId = identity().roomId,
                                   .battleId = identity().battleInstanceId},
          true, BattleTime::fromLogicalTick(1)) != BattleLoadResultCode::Ok) {
    return false;
  }
  const auto mutation = recorder.appendCommand(
      CanonicalCommand::arenaLoadComplete(1), mutationBefore, roomState(),
      battle, roomState(), 0, {});
  if (!mutation.ok() || !mutation.recorded || recorder.records().size() != 4U ||
      !mutation.record.has_value()) {
    return false;
  }
  const auto &payload =
      std::get<lol::battle_continuity::CommandDecisionPayload>(
          mutation.record->payload);
  return payload.participantSlot == 0U && payload.commandId.high == 0U &&
         payload.commandId.low == mutation.record->header.recordSequence &&
         payload.commandPayload == lol::battle_continuity::Bytes{0U, 1U} &&
         payload.outcomePayload.empty() &&
         payload.roomRecoveryStateBytes ==
             lol::battle_continuity::encodeRoomRecoveryState(roomState()).bytes;
}

bool journalRoundTripsWithInitialCheckpoint() {
  auto battle = createdBattle();
  auto started = FlightRecorder::start(battle, identity(), 2, roomState());
  if (!started.has_value()) {
    return false;
  }
  auto recorder = std::move(*started);
  if (!recorder.commitTick().ok()) {
    return false;
  }
  const auto bytes = recorder.encodeJournal();
  return bytes.ok() && bytes.bytes.size() > 0;
}

bool duplicateAndStaleAdmissionDoNotBecomeMutationRecords() {
  auto battle = createdBattle();
  auto recorder = FlightRecorder::start(battle, identity(), 2, roomState());
  if (!recorder.has_value()) {
    return false;
  }
  if (!recorder->commitTick().ok()) {
    return false;
  }
  const auto beforeFirst = battle.exportDeterministicState();
  const auto load =
      ArenaLoadCompleteCommand{.sessionId = SessionId{11},
                               .generation = SessionGeneration{3},
                               .roomId = identity().roomId,
                               .battleId = identity().battleInstanceId};
  if (battle.completeLoad(load, true, BattleTime::fromLogicalTick(1)) !=
          BattleLoadResultCode::Ok ||
      !recorder
           ->appendCommand(CanonicalCommand::arenaLoadComplete(1), beforeFirst,
                           roomState(), battle, roomState(), 0, {})
           .recorded) {
    return false;
  }
  const auto beforeDuplicate = battle.exportDeterministicState();
  if (battle.completeLoad(load, true, BattleTime::fromLogicalTick(20)) !=
      BattleLoadResultCode::Ok) {
    return false;
  }
  const auto duplicate = recorder->appendCommand(
      CanonicalCommand::arenaLoadComplete(1), beforeDuplicate, roomState(),
      battle, roomState(), 0, {});
  if (!duplicate.ok() || duplicate.recorded) {
    return false;
  }
  const auto beforeStale = battle.exportDeterministicState();
  const auto stale =
      ArenaLoadCompleteCommand{.sessionId = SessionId{12},
                               .generation = SessionGeneration{999},
                               .roomId = identity().roomId,
                               .battleId = identity().battleInstanceId};
  if (battle.completeLoad(stale, true, BattleTime::fromLogicalTick(20)) !=
      lol::battle::BattleLoadResultCode::StaleSession) {
    return false;
  }
  const auto staleRecord = recorder->appendCommand(
      CanonicalCommand::arenaLoadComplete(2), beforeStale, roomState(), battle,
      roomState(), 9, {});
  return staleRecord.ok() && !staleRecord.recorded &&
         recorder->records().size() == 4U;
}

bool differentTickRequiresPreviousBatchCommit() {
  auto battle = createdBattle();
  auto recorder = FlightRecorder::start(battle, identity(), 2, roomState());
  if (!recorder.has_value()) {
    return false;
  }
  const auto before = battle.exportDeterministicState();
  if (battle.completeLoad(
          ArenaLoadCompleteCommand{.sessionId = SessionId{11},
                                   .generation = SessionGeneration{3},
                                   .roomId = identity().roomId,
                                   .battleId = identity().battleInstanceId},
          true, BattleTime::fromLogicalTick(1)) != BattleLoadResultCode::Ok) {
    return false;
  }
  const auto rejected =
      recorder->appendCommand(CanonicalCommand::arenaLoadComplete(1), before,
                              roomState(), battle, roomState(), 0, {});
  return !rejected.ok() && !rejected.recorded && rejected.error.has_value() &&
         rejected.error->code == RecorderErrorCode::BatchNotCommitted;
}

bool battleRecordingRejectsFalseRoomPredecessor() {
  auto battle = createdBattle();
  auto recording = BattleRecording::start(battle, identity(), 2U, roomState());
  if (!recording.has_value() || !recording->takePendingBatch().has_value()) {
    return false;
  }
  const auto before = battle.exportDeterministicState();
  auto falseBeforeRoom = roomState();
  falseBeforeRoom.memberSlots = {1U};
  const auto recordCount = recording->records().size();
  const bool recorded = recording->recordDecision(
      CanonicalCommand::suspendInput(1U), before, falseBeforeRoom, battle,
      roomState(), 0U, std::nullopt);
  return !recorded && recording->records().size() == recordCount &&
         !recording->takePendingBatch().has_value();
}

bool battleRecordingCommitsInitialBatchAndPhaseCheckpoint() {
  auto battle = createdBattle();
  auto recording = BattleRecording::start(battle, identity(), 2U, roomState());
  if (!recording.has_value() || recording->records().size() != 3U ||
      recording->records().back().header.recordType !=
          lol::battle_continuity::RecordType::TickCommit) {
    return false;
  }

  const auto beforeNoop = battle.exportDeterministicState();
  if (recording->recordDecision(CanonicalCommand::arenaLoadComplete(1U),
                                beforeNoop, roomState(), battle, roomState(),
                                0U, std::nullopt)) {
    return false;
  }
  const auto initialBatch = recording->takePendingBatch();
  if (!initialBatch.has_value() || initialBatch->firstRecordSequence != 1U ||
      initialBatch->lastRecordSequence != 3U ||
      initialBatch->logicalTick != 0U || initialBatch->terminal ||
      initialBatch->encodedRecords.empty() ||
      !recording->recordDecision(CanonicalCommand::arenaLoadComplete(1U),
                                 beforeNoop, roomState(), battle, roomState(),
                                 0U, std::nullopt) ||
      recording->records().size() != 3U) {
    return false;
  }

  const auto firstBefore = battle.exportDeterministicState();
  if (battle.completeLoad(
          ArenaLoadCompleteCommand{.sessionId = SessionId{11},
                                   .generation = SessionGeneration{3},
                                   .roomId = identity().roomId,
                                   .battleId = identity().battleInstanceId},
          true, BattleTime::fromLogicalTick(1U)) != BattleLoadResultCode::Ok ||
      !recording->recordDecision(CanonicalCommand::arenaLoadComplete(1U),
                                 firstBefore, roomState(), battle, roomState(),
                                 0U, std::nullopt) ||
      recording->records().size() != 6U) {
    return false;
  }
  const auto firstBatch = recording->takePendingBatch();
  if (!firstBatch.has_value() || firstBatch->firstRecordSequence != 4U ||
      firstBatch->lastRecordSequence != 6U || firstBatch->terminal ||
      firstBatch->encodedRecords.empty()) {
    return false;
  }

  const auto secondBefore = battle.exportDeterministicState();
  if (battle.completeLoad(
          ArenaLoadCompleteCommand{.sessionId = SessionId{12},
                                   .generation = SessionGeneration{4},
                                   .roomId = identity().roomId,
                                   .battleId = identity().battleInstanceId},
          true, BattleTime::fromLogicalTick(1U)) != BattleLoadResultCode::Ok ||
      !recording->recordDecision(CanonicalCommand::arenaLoadComplete(2U),
                                 secondBefore, roomState(), battle, roomState(),
                                 0U, std::nullopt) ||
      recording->records().size() != 9U) {
    return false;
  }
  if (recording->records()[6].header.recordType !=
          lol::battle_continuity::RecordType::CommandDecision ||
      recording->records()[7].header.recordType !=
          lol::battle_continuity::RecordType::Checkpoint ||
      recording->records()[8].header.recordType !=
          lol::battle_continuity::RecordType::TickCommit) {
    return false;
  }
  const auto phaseBatch = recording->takePendingBatch();
  if (!phaseBatch.has_value() || phaseBatch->firstRecordSequence != 7U ||
      phaseBatch->lastRecordSequence != 9U || phaseBatch->terminal ||
      phaseBatch->encodedRecords.empty()) {
    return false;
  }

  const auto periodicBefore = battle.exportDeterministicState();
  if (battle.suspendInput(SessionId{11}, SessionGeneration{3},
                          BattleTime::fromLogicalTick(21U)) !=
          lol::battle::BattleInputResultCode::Ok ||
      !recording->recordDecision(CanonicalCommand::suspendInput(1U),
                                 periodicBefore, roomState(), battle,
                                 roomState(), 0U, std::nullopt) ||
      recording->records().size() != 12U) {
    return false;
  }
  const auto periodicBatch = recording->takePendingBatch();
  return periodicBatch.has_value() &&
         periodicBatch->firstRecordSequence == 10U &&
         periodicBatch->lastRecordSequence == 12U &&
         periodicBatch->logicalTick == 21U && !periodicBatch->terminal &&
         !periodicBatch->encodedRecords.empty() &&
         recording->records()[10].header.recordType ==
             lol::battle_continuity::RecordType::Checkpoint &&
         recording->records()[10].header.logicalTick == 21U &&
         recording->records()[11].header.recordType ==
             lol::battle_continuity::RecordType::TickCommit;
}

bool battleRecordingResumesCommittedSequenceWithNewWriterEpoch() {
  auto battle = createdBattle();
  auto recording = BattleRecording::start(battle, identity(), 2U, roomState());
  if (!recording.has_value() || !recording->takePendingBatch().has_value()) {
    return false;
  }
  const auto firstBefore = battle.exportDeterministicState();
  if (battle.completeLoad(
          ArenaLoadCompleteCommand{.sessionId = SessionId{11},
                                   .generation = SessionGeneration{3},
                                   .roomId = identity().roomId,
                                   .battleId = identity().battleInstanceId},
          true, BattleTime::fromLogicalTick(1U)) != BattleLoadResultCode::Ok ||
      !recording->recordDecision(CanonicalCommand::arenaLoadComplete(1U),
                                 firstBefore, roomState(), battle, roomState(),
                                 0U, std::nullopt) ||
      !recording->takePendingBatch().has_value()) {
    return false;
  }
  const std::vector<lol::battle_continuity::Record> committedRecords{
      recording->records().begin(), recording->records().end()};
  if (BattleRecording::resume(battle, committedRecords, 2U).has_value()) {
    return false;
  }
  auto resumed = BattleRecording::resume(battle, committedRecords, 3U);
  if (!resumed.has_value() || resumed->takePendingBatch().has_value() ||
      resumed->records().size() != 6U ||
      !resumed->roomRecoveryState().has_value() ||
      *resumed->roomRecoveryState() != roomState()) {
    return false;
  }

  const auto secondBefore = battle.exportDeterministicState();
  if (battle.completeLoad(
          ArenaLoadCompleteCommand{.sessionId = SessionId{12},
                                   .generation = SessionGeneration{4},
                                   .roomId = identity().roomId,
                                   .battleId = identity().battleInstanceId},
          true, BattleTime::fromLogicalTick(2U)) != BattleLoadResultCode::Ok ||
      !resumed->recordDecision(CanonicalCommand::arenaLoadComplete(2U),
                               secondBefore, roomState(), battle, roomState(),
                               0U, std::nullopt)) {
    return false;
  }
  const auto batch = resumed->takePendingBatch();
  return batch.has_value() && batch->writerRecoveryEpoch == 3U &&
         batch->firstRecordSequence == 7U && batch->lastRecordSequence == 9U &&
         batch->logicalTick == 2U &&
         resumed->records()[6].header.writerRecoveryEpoch == 3U;
}

lol::battle::MovementResultCode advanceTick(
    BattleInstance &battle, const lol::battle::MovementTickCommand &command,
    BattleTime at) {
  const auto code = battle.integrateMovement(command, at);
  if (code == lol::battle::MovementResultCode::Ok && command.serverTick % 2U == 0U) {
    static_cast<void>(battle.captureStateSnapshot());
  }
  return code;
}

bool movementDefersDiskUntilCriticalEventAndRestoresDurablePrefix() {
  auto battle = createdBattle();
  auto recording = BattleRecording::start(battle, identity(), 2U, roomState());
  if (!recording.has_value()) {
    return false;
  }
  lol::battle_continuity::Bytes durable;
  const auto persistPending = [&] {
    auto batch = recording->takePendingBatch();
    if (!batch.has_value()) {
      return false;
    }
    durable.insert(durable.end(), batch->encodedRecords.begin(),
                   batch->encodedRecords.end());
    return true;
  };
  if (!persistPending()) {
    return false;
  }
  for (std::uint16_t slot = 1U; slot <= 2U; ++slot) {
    const auto before = battle.exportDeterministicState();
    if (battle.completeLoad(
            ArenaLoadCompleteCommand{
                SessionId{10U + slot}, SessionGeneration{2U + slot},
                identity().roomId, identity().battleInstanceId},
            true,
            BattleTime::fromLogicalTick(1U)) != BattleLoadResultCode::Ok ||
        !recording->recordDecision(CanonicalCommand::arenaLoadComplete(slot),
                                   before, roomState(), battle, roomState(), 0U,
                                   std::nullopt) ||
        !persistPending()) {
      return false;
    }
  }
  const auto durableHash =
      lol::battle_continuity::BattleReplayer::restoreJournal(durable)
          .finalStateHash;
  const auto beforeMove = battle.exportDeterministicState();
  const lol::battle::DirectionIntent direction{1, 0, 0};
  if (battle.acceptMove({SessionId{11}, SessionGeneration{3},
                         identity().battleInstanceId, 1U, direction},
                        BattleTime::fromLogicalTick(2U)) !=
          lol::battle::MovementResultCode::Ok ||
      !recording->recordDecision(
          CanonicalCommand::move(1U, {0U, 1U}, direction), beforeMove,
          roomState(), battle, roomState(), 0U, std::nullopt) ||
      recording->takePendingBatch().has_value()) {
    return false;
  }
  const auto beforeTick = battle.exportDeterministicState();
  if (advanceTick(battle, {identity().battleInstanceId, 2U},
                                 BattleTime::fromLogicalTick(2U)) != lol::battle::MovementResultCode::Ok ||
      !recording->recordDecision(CanonicalCommand::movementTick(2U), beforeTick,
                                 roomState(), battle, roomState(), 0U,
                                 std::nullopt) ||
      recording->takePendingBatch().has_value()) {
    return false;
  }
  // No disk submission occurred: a crash restores the pre-movement state.
  // Cross the former periodic checkpoint boundary without a disk write.
  for (std::uint32_t tick = 3U; tick <= 240U; ++tick) {
    const auto before = battle.exportDeterministicState();
    if (advanceTick(battle, {identity().battleInstanceId, tick},
                                   BattleTime::fromLogicalTick(tick)) != lol::battle::MovementResultCode::Ok ||
        !recording->recordDecision(CanonicalCommand::movementTick(tick), before,
                                   roomState(), battle, roomState(), 0U,
                                   std::nullopt) ||
        recording->takePendingBatch().has_value()) {
      std::fprintf(stderr, "volatile tick failed at %u\n", tick);
      return false;
    }
  }
  const auto rolledBack =
      lol::battle_continuity::BattleReplayer::restoreJournal(durable);
  if (!rolledBack.ok() || !durableHash.has_value() ||
      rolledBack.finalStateHash != durableHash) {
    return false;
  }
  const auto beforeAttack = battle.exportDeterministicState();
  const auto hit =
      battle.attackWithApplied({{0U, 1U},
                                SessionId{11},
                                SessionGeneration{3},
                                identity().battleInstanceId,
                                lol::battle::CombatRuleset::monsterId},
                               BattleTime::fromLogicalTick(240U));
  if (hit.result.code != lol::battle::AttackResultCode::Ok ||
      hit.result.remainingHitPoints == 0U ||
      !recording->recordDecision(
          CanonicalCommand::attack(1U, {0U, 1U},
                                   lol::battle::CombatRuleset::monsterId),
          beforeAttack, roomState(), battle, roomState(), 0U, std::nullopt) ||
      recording->takePendingBatch().has_value())
    return false;
  const auto beforeSuspend = battle.exportDeterministicState();
  if (battle.suspendInput(SessionId{11}, SessionGeneration{3},
                          BattleTime::fromLogicalTick(241U)) !=
          lol::battle::BattleInputResultCode::Ok ||
      !recording->recordDecision(CanonicalCommand::suspendInput(1U),
                                 beforeSuspend, roomState(), battle,
                                 roomState(), 0U, std::nullopt) ||
      !persistPending()) {
    return false;
  }
  const auto restored =
      lol::battle_continuity::BattleReplayer::restoreJournal(durable);
  const auto expected = lol::battle_continuity::encodeRecoveryState(
      battle.exportDeterministicState(), roomState());
  if (!(restored.ok() && expected.ok() &&
        restored.finalStateHash ==
            lol::battle_continuity::recoveryStateHash(expected.bytes)))
    return false;
  const auto beforeResume = battle.exportDeterministicState();
  if (battle.resumeInput(SessionId{11}, SessionGeneration{3},
                         BattleTime::fromLogicalTick(242U)) !=
          lol::battle::BattleInputResultCode::Ok ||
      !recording->recordDecision(CanonicalCommand::resumeInput(1U),
                                 beforeResume, roomState(), battle, roomState(),
                                 0U, std::nullopt) ||
      !persistPending())
    return false;
  for (std::uint64_t attackId = 2U; attackId <= 16U; ++attackId) {
    const auto before = battle.exportDeterministicState();
    const auto result = battle.attackWithApplied(
        {{0U, attackId},
         SessionId{11},
         SessionGeneration{3},
         identity().battleInstanceId,
         lol::battle::CombatRuleset::monsterId},
        BattleTime::fromLogicalTick(241U + attackId * 16U));
    if (result.result.code != lol::battle::AttackResultCode::Ok ||
        !recording->recordDecision(
            CanonicalCommand::attack(1U, {0U, attackId},
                                     lol::battle::CombatRuleset::monsterId),
            before, roomState(), battle, roomState(), 0U, std::nullopt))
      return false;
    if (attackId < 16U) {
      if (recording->takePendingBatch().has_value())
        return false;
    } else if (result.result.remainingHitPoints != 0U || !persistPending())
      return false;
  }
  const auto afterKill =
      lol::battle_continuity::BattleReplayer::restoreJournal(durable);
  const auto killState = lol::battle_continuity::encodeRecoveryState(
      battle.exportDeterministicState(), roomState());
  return afterKill.ok() && killState.ok() &&
         afterKill.finalStateHash ==
             lol::battle_continuity::recoveryStateHash(killState.bytes);
}

} // namespace

int main() {
  if (!movementDefersDiskUntilCriticalEventAndRestoresDurablePrefix()) {
    std::fputs("selective persistence regression failed\n", stderr);
    return EXIT_FAILURE;
  }
  if (!recorderEmitsOnlyAStateMutation() ||
      !journalRoundTripsWithInitialCheckpoint() ||
      !duplicateAndStaleAdmissionDoNotBecomeMutationRecords() ||
      !differentTickRequiresPreviousBatchCommit() ||
      !battleRecordingRejectsFalseRoomPredecessor() ||
      !battleRecordingCommitsInitialBatchAndPhaseCheckpoint() ||
      !battleRecordingResumesCommittedSequenceWithNewWriterEpoch()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
