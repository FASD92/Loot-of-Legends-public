#include "workflows/LoadBarrierWorkflow.hpp"

#include <exception>
#include <utility>

namespace lol::game_flow::workflows {
namespace {

std::optional<battle::BattleLoadProjection>
finishResolvedBarrier(lobby_room::Room &room,
                      std::optional<battle::BattleInstance> &battleInstance,
                      battle::BattleLoadState before) {
  auto projection = battleInstance->projection();
  if (projection.state == before) {
    return projection;
  }
  if (projection.state == battle::BattleLoadState::GameplayCommitted) {
    if (room.commitInProgress() != lobby_room::RoomResultCode::Ok) {
      std::terminate();
    }
  } else if (projection.state == battle::BattleLoadState::LoadCancelled) {
    if (room.reopenAfterLoadCancelled() != lobby_room::RoomResultCode::Ok) {
      std::terminate();
    }
    battleInstance.reset();
  }
  return projection;
}

bool becameGameplayCommitted(
    battle::BattleLoadState before,
    const std::optional<battle::BattleLoadProjection> &after) {
  return before != battle::BattleLoadState::GameplayCommitted &&
         after.has_value() &&
         after->state == battle::BattleLoadState::GameplayCommitted;
}

} // namespace

LoadBarrierWorkflowResult
completeLoad(lobby_room::Room &room,
             std::optional<battle::BattleInstance> &battleInstance,
             const battle::ArenaLoadCompleteCommand &command,
             const GameplayTransportReadinessPort *readiness) {
  if (!battleInstance.has_value()) {
    return {battle::BattleLoadResultCode::StaleBattle, std::nullopt, false};
  }
  const auto before = battleInstance->projection().state;
  const bool transportReady =
      readiness != nullptr &&
      readiness->isReady(command.sessionId, command.generation);
  const auto code = battleInstance->completeLoad(command, transportReady);
  if (code != battle::BattleLoadResultCode::Ok) {
    return {code, battleInstance->projection(), false};
  }
  auto projection = finishResolvedBarrier(room, battleInstance, before);
  return {code, projection, becameGameplayCommitted(before, projection)};
}

LoadDisconnectWorkflowResult
exitParticipant(lobby_room::Room &room,
                std::optional<battle::BattleInstance> &battleInstance,
                shared::SessionId sessionId,
                shared::SessionGeneration generation,
                battle::ParticipantExitStatus exitStatus,
                std::chrono::steady_clock::time_point completedAt) {
  auto battleCode = battle::BattleLoadResultCode::StaleBattle;
  auto before = battle::BattleLoadState::Created;
  if (battleInstance.has_value()) {
    const auto projection = battleInstance->projection();
    before = projection.state;
    const auto command = battle::CandidateDisconnectedCommand{
        .sessionId = sessionId,
        .generation = generation,
        .roomId = projection.roomId,
        .battleId = projection.battleId,
    };
    battleCode = exitStatus == battle::ParticipantExitStatus::VoluntaryLeft
                     ? battleInstance->leave(command, completedAt)
                     : battleInstance->disconnect(command, completedAt);
  }
  const auto roomCode = room.leave(lobby_room::LeaveRoomCommand{
      .sessionId = sessionId,
      .generation = generation,
  });
  auto projection = battleInstance.has_value()
                        ? std::optional{battleInstance->projection()}
                        : std::nullopt;
  if (battleInstance.has_value() &&
      battleCode == battle::BattleLoadResultCode::Ok) {
    projection = finishResolvedBarrier(room, battleInstance, before);
  }
  const bool gameplayStartCommitted =
      becameGameplayCommitted(before, projection);
  return {roomCode, battleCode, std::move(projection), gameplayStartCommitted};
}

LoadDisconnectWorkflowResult
disconnect(lobby_room::Room &room,
           std::optional<battle::BattleInstance> &battleInstance,
           shared::SessionId sessionId, shared::SessionGeneration generation,
           std::chrono::steady_clock::time_point completedAt) {
  return exitParticipant(room, battleInstance, sessionId, generation,
                         battle::ParticipantExitStatus::Disconnected,
                         completedAt);
}

LoadBarrierWorkflowResult
expireLoadBarrier(lobby_room::Room &room,
                  std::optional<battle::BattleInstance> &battleInstance,
                  const battle::LoadBarrierDeadlineCommand &command) {
  if (!battleInstance.has_value()) {
    return {battle::BattleLoadResultCode::StaleBattle, std::nullopt, false};
  }
  const auto before = battleInstance->projection().state;
  const auto code = battleInstance->expireLoadBarrier(command);
  if (code != battle::BattleLoadResultCode::Ok) {
    return {code, battleInstance->projection(), false};
  }
  auto projection = finishResolvedBarrier(room, battleInstance, before);
  return {code, projection, becameGameplayCommitted(before, projection)};
}

} // namespace lol::game_flow::workflows
