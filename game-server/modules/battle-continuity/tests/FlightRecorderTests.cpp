#include <lol/battle/BattleLoadApi.hpp>
#include <lol/battle_continuity/FlightRecorder.hpp>

#include <array>
#include <cstdint>
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
using lol::battle_continuity::RoomRecoveryPhase;
using lol::battle_continuity::RoomRecoveryState;
using lol::battle_continuity::RecorderErrorCode;
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

  const auto noMutation = recorder.appendCommand(
      CanonicalCommand::suspendInput(1), before, roomState(), battle,
      roomState(), 0, {});
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
  const auto rejected = recorder->appendCommand(
      CanonicalCommand::arenaLoadComplete(1), before, roomState(), battle,
      roomState(), 0, {});
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
      recording->records().size() != 5U) {
    return false;
  }
  const auto firstBatch = recording->takePendingBatch();
  if (!firstBatch.has_value() || firstBatch->firstRecordSequence != 4U ||
      firstBatch->lastRecordSequence != 5U || firstBatch->terminal ||
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
      recording->records().size() != 8U) {
    return false;
  }
  if (recording->records()[5].header.recordType !=
          lol::battle_continuity::RecordType::CommandDecision ||
      recording->records()[6].header.recordType !=
          lol::battle_continuity::RecordType::Checkpoint ||
      recording->records()[7].header.recordType !=
          lol::battle_continuity::RecordType::TickCommit) {
    return false;
  }
  const auto phaseBatch = recording->takePendingBatch();
  if (!phaseBatch.has_value() || phaseBatch->firstRecordSequence != 6U ||
      phaseBatch->lastRecordSequence != 8U || phaseBatch->terminal ||
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
      recording->records().size() != 11U) {
    return false;
  }
  const auto periodicBatch = recording->takePendingBatch();
  return periodicBatch.has_value() &&
         periodicBatch->firstRecordSequence == 9U &&
         periodicBatch->lastRecordSequence == 11U &&
         periodicBatch->logicalTick == 21U && !periodicBatch->terminal &&
         !periodicBatch->encodedRecords.empty() &&
         recording->records()[9].header.recordType ==
             lol::battle_continuity::RecordType::Checkpoint &&
         recording->records()[9].header.logicalTick == 21U &&
         recording->records()[10].header.recordType ==
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
      resumed->records().size() != 5U ||
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
         batch->firstRecordSequence == 6U && batch->lastRecordSequence == 8U &&
         batch->logicalTick == 2U &&
         resumed->records()[5].header.writerRecoveryEpoch == 3U;
}

} // namespace

int main() {
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
