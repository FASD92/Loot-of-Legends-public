#pragma once

#include <lol/battle/BattleResult.hpp>
#include <lol/game_flow/RoomCommandGateway.hpp>
#include <lol/lobby_room/RoomApi.hpp>

#include <optional>
#include <vector>

namespace lol::game_flow::workflows {

struct ResultGenerationFailureRecovery final {
  BattleRecoveryNotice notice;
  std::vector<lobby_room::RoomMemberSnapshot> participants;
};

[[nodiscard]] std::optional<ResultGenerationFailureRecovery>
recoverResultGenerationFailure(
    lobby_room::Room &room, const battle::BattleLoadProjection &battle,
    const battle::BattleResultProjection &result,
    std::optional<shared::BattleInstanceId> &emittedBattle);

[[nodiscard]] std::vector<LobbyRoomOutboundIntent>
composeResultFailureVisibility(
    const ResultGenerationFailureRecovery &recovery,
    const std::vector<lobby_room::RoomSummary> &remainingRooms);

} // namespace lol::game_flow::workflows
