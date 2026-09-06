#include <lol/battle/BattleLoadApi.hpp>
#include <lol/battle/CombatApi.hpp>
#include <lol/battle/LootApi.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {

using lol::battle::ArenaLoadCompleteCommand;
using lol::battle::AttackCommand;
using lol::battle::AttackResultCode;
using lol::battle::BattleAdmissionSnapshot;
using lol::battle::BattleInstance;
using lol::battle::BattleLoadResultCode;
using lol::battle::BattleLoadState;
using lol::battle::BattleStartCandidate;
using lol::battle::CandidateDisconnectedCommand;
using lol::battle::ClaimLootCommand;
using lol::battle::CommandId;
using lol::battle::DirectionIntent;
using lol::battle::LoadBarrierDeadlineCommand;
using lol::battle::LoadCandidateState;
using lol::battle::MovementResultCode;
using lol::battle::ParticipantExitStatus;
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

BattleStartCandidate candidate(std::uint64_t sessionId,
                               std::uint64_t generation = 1) {
  return BattleStartCandidate{
      .accountId = account(static_cast<std::uint8_t>(sessionId)),
      .sessionId = SessionId{sessionId},
      .generation = SessionGeneration{generation},
      .nickname = "player-" + std::to_string(sessionId),
  };
}

BattleAdmissionSnapshot admission(std::size_t count) {
  std::vector<BattleStartCandidate> candidates;
  for (std::size_t index = 0; index < count; ++index) {
    candidates.push_back(candidate(index + 1));
  }
  return BattleAdmissionSnapshot{
      .roomId = RoomId{7},
      .battleId = BattleInstanceId{1},
      .candidates = std::move(candidates),
  };
}

BattleInstance openBattle(std::size_t count) {
  auto result = BattleInstance::create(admission(count));
  if (result.code != BattleLoadResultCode::Ok || !result.battle.has_value() ||
      result.battle->projection().state != BattleLoadState::Created ||
      result.battle->openLoadBarrier() != BattleLoadResultCode::Ok) {
    std::abort();
  }
  return std::move(*result.battle);
}

ArenaLoadCompleteCommand complete(std::uint64_t sessionId,
                                  std::uint64_t generation = 1,
                                  std::uint64_t roomId = 7,
                                  std::uint64_t battleId = 1) {
  return ArenaLoadCompleteCommand{
      .sessionId = SessionId{sessionId},
      .generation = SessionGeneration{generation},
      .roomId = RoomId{roomId},
      .battleId = BattleInstanceId{battleId},
  };
}

CandidateDisconnectedCommand disconnect(std::uint64_t sessionId,
                                        std::uint64_t generation = 1) {
  return CandidateDisconnectedCommand{
      .sessionId = SessionId{sessionId},
      .generation = SessionGeneration{generation},
      .roomId = RoomId{7},
      .battleId = BattleInstanceId{1},
  };
}

bool inputSuspendStopsAllPlayerCommandsAndResumeRestoresOnlyCurrentGeneration() {
  auto battle = openBattle(2);
  if (battle.completeLoad(complete(1), true) != BattleLoadResultCode::Ok ||
      battle.completeLoad(complete(2), true) != BattleLoadResultCode::Ok) {
    return false;
  }

  const auto receivedAt = std::chrono::steady_clock::now();
  const auto move = lol::battle::MoveCommand{
      .sessionId = SessionId{1},
      .generation = SessionGeneration{1},
      .battleId = BattleInstanceId{1},
      .actionSequence = 1,
      .direction =
          DirectionIntent{.desiredX = 1, .desiredY = 0, .inputFlags = 0},
  };
  if (battle.acceptMove(move, receivedAt) != MovementResultCode::Ok ||
      battle.integrateMovement(lol::battle::MovementTickCommand{
          .battleId = BattleInstanceId{1}, .serverTick = 1}) !=
          MovementResultCode::Ok) {
    return false;
  }
  const auto beforeSuspend = battle.movementProjection();
  const auto beforeLoad = battle.projection();
  const auto beforeLoot = battle.lootProjection();
  const auto beforeResult = battle.resultProjection();
  if (battle.suspendInput(SessionId{1}, SessionGeneration{1}) !=
      lol::battle::BattleInputResultCode::Ok) {
    return false;
  }

  auto suspendedMove = move;
  suspendedMove.actionSequence = 2;
  const auto attack = AttackCommand{
      .commandId = CommandId{.high = 1, .low = 1},
      .sessionId = SessionId{1},
      .generation = SessionGeneration{1},
      .battleId = BattleInstanceId{1},
      .targetHint = lol::battle::CombatRuleset::monsterId,
  };
  const auto claim = ClaimLootCommand{
      .commandId = CommandId{.high = 1, .low = 2},
      .sessionId = SessionId{1},
      .generation = SessionGeneration{1},
      .battleId = BattleInstanceId{1},
      .dropId = lol::battle::DropId{1},
  };
  if (battle.acceptMove(suspendedMove, receivedAt) !=
          MovementResultCode::NotEligible ||
      battle.attack(attack, receivedAt).code != AttackResultCode::NotEligible ||
      battle.claimLoot(claim, receivedAt).code !=
          lol::battle::ClaimLootResultCode::NotEligible ||
      battle.integrateMovement(lol::battle::MovementTickCommand{
          .battleId = BattleInstanceId{1}, .serverTick = 2}) !=
          MovementResultCode::Ok) {
    return false;
  }
  const auto whileSuspended = battle.movementProjection();
  const auto suspended =
      battle.resumeProjection(SessionId{1}, SessionGeneration{1});
  if (whileSuspended.players != beforeSuspend.players ||
      battle.projection().state != beforeLoad.state ||
      battle.projection().capturedParticipants !=
          beforeLoad.capturedParticipants ||
      battle.lootProjection() != beforeLoot ||
      battle.resultProjection() != beforeResult || !suspended.has_value() ||
      suspended->inputEnabled ||
      battle.suspendInput(SessionId{1}, SessionGeneration{2}) !=
          lol::battle::BattleInputResultCode::StaleSession ||
      battle.resumeInput(SessionId{1}, SessionGeneration{2}) !=
          lol::battle::BattleInputResultCode::StaleSession) {
    return false;
  }

  if (battle.resumeInput(SessionId{1}, SessionGeneration{1}) !=
      lol::battle::BattleInputResultCode::Ok) {
    return false;
  }
  const auto resumed =
      battle.resumeProjection(SessionId{1}, SessionGeneration{1});
  return resumed.has_value() && resumed->roomId == RoomId{7} &&
         resumed->battleId == BattleInstanceId{1} &&
         resumed->sessionId == SessionId{1} &&
         resumed->generation == SessionGeneration{1} && resumed->inputEnabled &&
         resumed->battleState == BattleLoadState::GameplayCommitted &&
         resumed->combat.has_value() &&
         resumed->loot.battleId == BattleInstanceId{1} &&
         resumed->result.state == lol::battle::BattleResultState::NotReady;
}

LoadCandidateState candidateState(const BattleInstance &battle,
                                  std::uint64_t sessionId) {
  for (const auto &candidateProjection : battle.projection().candidates) {
    if (candidateProjection.sessionId == SessionId{sessionId}) {
      return candidateProjection.state;
    }
  }
  std::abort();
}

bool opensLoadBarrierAndRejectsInvalidAdmission() {
  auto invalid = BattleInstance::create(admission(1));
  if (invalid.code != BattleLoadResultCode::InvalidArgument ||
      invalid.battle.has_value()) {
    return false;
  }

  auto battle = openBattle(2);
  const auto projection = battle.projection();
  return projection.state == BattleLoadState::LoadBarrierOpen &&
         projection.roomId == RoomId{7} &&
         projection.battleId == BattleInstanceId{1} &&
         projection.candidates.size() == 2;
}

bool completionIsCorrelatedAndIdempotent() {
  auto battle = openBattle(3);
  if (battle.completeLoad(complete(1), true) != BattleLoadResultCode::Ok ||
      battle.completeLoad(complete(1), true) != BattleLoadResultCode::Ok ||
      candidateState(battle, 1) != LoadCandidateState::Ready ||
      battle.completeLoad(complete(1, 1, 8), true) !=
          BattleLoadResultCode::StaleBattle ||
      battle.completeLoad(complete(1, 1, 7, 2), true) !=
          BattleLoadResultCode::StaleBattle ||
      battle.completeLoad(complete(9), true) !=
          BattleLoadResultCode::NotEligible ||
      battle.completeLoad(complete(2, 2), true) !=
          BattleLoadResultCode::StaleSession) {
    return false;
  }
  const auto projection = battle.projection();
  return projection.state == BattleLoadState::LoadBarrierOpen &&
         candidateState(battle, 2) == LoadCandidateState::PendingLoad &&
         projection.capturedParticipants.empty();
}

bool twoReadyCandidatesCommitCapturedSet() {
  auto battle = openBattle(2);
  if (battle.completeLoad(complete(1), true) != BattleLoadResultCode::Ok ||
      battle.completeLoad(complete(2), true) != BattleLoadResultCode::Ok) {
    return false;
  }
  const auto projection = battle.projection();
  return projection.state == BattleLoadState::GameplayCommitted &&
         projection.capturedParticipants.size() == 2 &&
         projection.capturedParticipants[0].accountId == account(1) &&
         projection.capturedParticipants[0].sessionId == SessionId{1} &&
         projection.capturedParticipants[0].generation ==
             SessionGeneration{1} &&
         projection.capturedParticipants[0].nickname == "player-1" &&
         projection.capturedParticipants[0].exitStatus ==
             ParticipantExitStatus::GameplayEligible;
}

bool disconnectBelowMinimumCancels() {
  auto battle = openBattle(2);
  if (battle.disconnect(disconnect(1)) != BattleLoadResultCode::Ok ||
      battle.completeLoad(complete(1), true) !=
          BattleLoadResultCode::NotEligible ||
      battle.completeLoad(complete(2), true) != BattleLoadResultCode::Ok) {
    return false;
  }
  const auto projection = battle.projection();
  return candidateState(battle, 1) == LoadCandidateState::Disconnected &&
         projection.state == BattleLoadState::LoadCancelled &&
         projection.capturedParticipants.empty();
}

bool deadlineTimesOutPendingCandidatesAndResolves() {
  auto battle = openBattle(3);
  if (battle.completeLoad(complete(1), true) != BattleLoadResultCode::Ok ||
      battle.completeLoad(complete(2), true) != BattleLoadResultCode::Ok ||
      battle.expireLoadBarrier(LoadBarrierDeadlineCommand{
          .roomId = RoomId{7}, .battleId = BattleInstanceId{1}}) !=
          BattleLoadResultCode::Ok) {
    return false;
  }
  const auto projection = battle.projection();
  return candidateState(battle, 3) == LoadCandidateState::TimedOut &&
         projection.state == BattleLoadState::GameplayCommitted &&
         projection.capturedParticipants.size() == 2;
}

} // namespace

int main() {
  if (!opensLoadBarrierAndRejectsInvalidAdmission() ||
      !completionIsCorrelatedAndIdempotent() ||
      !twoReadyCandidatesCommitCapturedSet() ||
      !disconnectBelowMinimumCancels() ||
      !deadlineTimesOutPendingCandidatesAndResolves() ||
      !inputSuspendStopsAllPlayerCommandsAndResumeRestoresOnlyCurrentGeneration()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
