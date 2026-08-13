#include "workflows/BattleRecoveryWorkflow.hpp"

#include <algorithm>
#include <utility>

namespace lol::game_flow::workflows {

std::optional<ResultGenerationFailureRecovery> recoverResultGenerationFailure(
    lobby_room::Room &room, const battle::BattleLoadProjection &battle,
    const battle::BattleResultProjection &result,
    std::optional<shared::BattleInstanceId> &emittedBattle) {
  const auto detail = room.detail();
  if (emittedBattle.has_value() ||
      result.state != battle::BattleResultState::ResultGenerationFailed ||
      result.result.has_value() || battle.roomId.value() == 0u ||
      battle.battleId.value() == 0u ||
      battle.state != battle::BattleLoadState::GameplayCommitted ||
      room.lifecycle() != lobby_room::RoomLifecycle::InProgress) {
    return std::nullopt;
  }
  if (!detail.has_value()) {
    if (!room.closed()) {
      return std::nullopt;
    }
    emittedBattle = battle.battleId;
    return ResultGenerationFailureRecovery{
        .notice =
            BattleRecoveryNotice{
                .roomId = battle.roomId,
                .battleId = battle.battleId,
                .reason = BattleRecoveryReason::ResultGenerationFailed,
            },
        .participants = {},
    };
  }
  if (battle.roomId != detail->roomId) {
    return std::nullopt;
  }
  const bool allCurrentMembersWereCaptured =
      std::ranges::all_of(detail->members, [&battle](const auto &member) {
        return std::ranges::any_of(
            battle.capturedParticipants, [&member](const auto &participant) {
              return participant.sessionId == member.sessionId &&
                     participant.generation == member.sessionGeneration;
            });
      });
  if (!allCurrentMembersWereCaptured) {
    return std::nullopt;
  }

  auto participants = detail->members;
  for (const auto &participant : participants) {
    if (room.leave(lobby_room::LeaveRoomCommand{
            .sessionId = participant.sessionId,
            .generation = participant.sessionGeneration,
        }) != lobby_room::RoomResultCode::Ok) {
      return std::nullopt;
    }
  }
  if (!room.closed()) {
    return std::nullopt;
  }
  emittedBattle = battle.battleId;
  return ResultGenerationFailureRecovery{
      .notice =
          BattleRecoveryNotice{
              .roomId = battle.roomId,
              .battleId = battle.battleId,
              .reason = BattleRecoveryReason::ResultGenerationFailed,
          },
      .participants = std::move(participants),
  };
}

std::vector<LobbyRoomOutboundIntent> composeResultFailureVisibility(
    const ResultGenerationFailureRecovery &recovery,
    const std::vector<lobby_room::RoomSummary> &remainingRooms) {
  std::vector<LobbyRoomOutboundIntent> intents;
  intents.reserve(recovery.participants.size() + 2u);
  intents.push_back(LobbyRoomOutboundIntent{
      .audience = RoomAudience{recovery.notice.roomId},
      .message = recovery.notice,
  });
  for (const auto &participant : recovery.participants) {
    intents.push_back(LobbyRoomOutboundIntent{
        .audience = SessionAudience{participant.sessionId,
                                    participant.sessionGeneration},
        .message =
            LobbyEntrySnapshot{
                .session =
                    AuthenticatedRoomSession{
                        .accountId = participant.accountId,
                        .sessionId = participant.sessionId,
                        .generation = participant.sessionGeneration,
                        .nickname = participant.nickname,
                    },
                .rooms = remainingRooms,
            },
    });
  }
  intents.push_back(LobbyRoomOutboundIntent{
      .audience = LobbyAudience{},
      .message = LobbyRoomListUpdate{.rooms = remainingRooms},
  });
  return intents;
}

} // namespace lol::game_flow::workflows
