#pragma once

#include <lol/battle/BattleLoadApi.hpp>
#include <lol/game_flow/GameplayTransportReadinessPort.hpp>
#include <lol/lobby_room/RoomApi.hpp>

#include <chrono>
#include <optional>

namespace lol::game_flow::workflows {

struct LoadBarrierWorkflowResult final {
  battle::BattleLoadResultCode code;
  std::optional<battle::BattleLoadProjection> battle;
  bool gameplayStartCommitted;
};

struct LoadDisconnectWorkflowResult final {
  lobby_room::RoomResultCode roomCode;
  battle::BattleLoadResultCode battleCode;
  std::optional<battle::BattleLoadProjection> battle;
  bool gameplayStartCommitted;
};

[[nodiscard]] LoadBarrierWorkflowResult
completeLoad(lobby_room::Room &room,
             std::optional<battle::BattleInstance> &battle,
             const battle::ArenaLoadCompleteCommand &command,
             const GameplayTransportReadinessPort *readiness);

// Single composition seam for both voluntary leave and confirmed disconnect:
// battle participant exit/freeze first, then lobby-room membership removal in
// the same Cell turn. The distinct exit reasons are preserved.
[[nodiscard]] LoadDisconnectWorkflowResult exitParticipant(
    lobby_room::Room &room, std::optional<battle::BattleInstance> &battle,
    shared::SessionId sessionId, shared::SessionGeneration generation,
    battle::ParticipantExitStatus exitStatus,
    std::chrono::steady_clock::time_point completedAt);

[[nodiscard]] LoadDisconnectWorkflowResult
disconnect(lobby_room::Room &room,
           std::optional<battle::BattleInstance> &battle,
           shared::SessionId sessionId, shared::SessionGeneration generation,
           std::chrono::steady_clock::time_point completedAt);

[[nodiscard]] LoadBarrierWorkflowResult
expireLoadBarrier(lobby_room::Room &room,
                  std::optional<battle::BattleInstance> &battle,
                  const battle::LoadBarrierDeadlineCommand &command);

} // namespace lol::game_flow::workflows
