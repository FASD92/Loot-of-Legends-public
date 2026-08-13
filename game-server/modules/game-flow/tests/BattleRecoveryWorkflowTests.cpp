#include "workflows/BattleRecoveryWorkflow.hpp"

#include <lol/battle/BattleResult.hpp>
#include <lol/game_flow/RoomCommandGateway.hpp>
#include <lol/lobby_room/RoomApi.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

namespace {

using lol::battle::BattleLoadProjection;
using lol::battle::BattleLoadState;
using lol::battle::BattleResultProjection;
using lol::battle::BattleResultState;
using lol::battle::CapturedParticipant;
using lol::battle::ParticipantExitStatus;
using lol::game_flow::BattleRecoveryNotice;
using lol::game_flow::BattleRecoveryReason;
using lol::game_flow::LobbyEntrySnapshot;
using lol::game_flow::LobbyRoomListUpdate;
using lol::game_flow::workflows::composeResultFailureVisibility;
using lol::game_flow::workflows::recoverResultGenerationFailure;
using lol::lobby_room::CreateRoomCommand;
using lol::lobby_room::JoinRoomCommand;
using lol::lobby_room::Room;
using lol::lobby_room::RoomMemberIdentity;
using lol::lobby_room::RoomResultCode;
using lol::lobby_room::SetReadyCommand;
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

std::optional<Room> inProgressRoom() {
  auto created = Room::create(CreateRoomCommand{
      .roomId = RoomId{7},
      .title = "recovery",
      .capacity = 2,
      .creator = RoomMemberIdentity{account(1), SessionId{1},
                                    SessionGeneration{1}, "neo"},
  });
  if (!created.room.has_value() ||
      created.room->join(JoinRoomCommand{RoomMemberIdentity{
          account(2), SessionId{2}, SessionGeneration{1}, "trinity"}}) !=
          RoomResultCode::Ok ||
      created.room->setReady(SetReadyCommand{SessionId{1}, SessionGeneration{1},
                                             true}) != RoomResultCode::Ok ||
      created.room->setReady(SetReadyCommand{SessionId{2}, SessionGeneration{1},
                                             true}) != RoomResultCode::Ok) {
    return std::nullopt;
  }
  const auto admission =
      created.room->prepareHostStart({.actorSessionId = SessionId{1},
                                      .actorGeneration = SessionGeneration{1}});
  if (!admission.admission.has_value() ||
      created.room->commitLoading(*admission.admission) != RoomResultCode::Ok ||
      created.room->commitInProgress() != RoomResultCode::Ok) {
    return std::nullopt;
  }
  return std::move(created.room);
}

BattleLoadProjection failedBattle() {
  return BattleLoadProjection{
      .roomId = RoomId{7},
      .battleId = BattleInstanceId{9},
      .state = BattleLoadState::GameplayCommitted,
      .candidates = {},
      .capturedParticipants =
          {
              CapturedParticipant{account(1), SessionId{1},
                                  SessionGeneration{1}, "neo",
                                  ParticipantExitStatus::GameplayEligible},
              CapturedParticipant{account(2), SessionId{2},
                                  SessionGeneration{1}, "trinity",
                                  ParticipantExitStatus::GameplayEligible},
          },
  };
}

bool terminalFailureClosesOnceAndComposesNoticeBeforeLobby() {
  auto room = inProgressRoom();
  if (!room.has_value()) {
    return false;
  }
  std::optional<BattleInstanceId> emittedBattle;
  auto recovery = recoverResultGenerationFailure(
      *room, failedBattle(),
      BattleResultProjection{BattleResultState::ResultGenerationFailed,
                             std::nullopt},
      emittedBattle);
  if (!recovery.has_value() || !room->closed() || room->detail().has_value() ||
      recovery->notice !=
          BattleRecoveryNotice{
              .roomId = RoomId{7},
              .battleId = BattleInstanceId{9},
              .reason = BattleRecoveryReason::ResultGenerationFailed,
          } ||
      recovery->participants.size() != 2u) {
    return false;
  }

  const auto intents = composeResultFailureVisibility(*recovery, {});
  if (intents.size() != 4u ||
      !std::holds_alternative<BattleRecoveryNotice>(intents[0].message) ||
      !std::holds_alternative<LobbyEntrySnapshot>(intents[1].message) ||
      !std::holds_alternative<LobbyEntrySnapshot>(intents[2].message) ||
      !std::holds_alternative<LobbyRoomListUpdate>(intents[3].message) ||
      std::ranges::any_of(intents, [](const auto &intent) {
        return std::holds_alternative<lol::battle::BattleFinalResult>(
            intent.message);
      })) {
    return false;
  }

  return !recoverResultGenerationFailure(
              *room, failedBattle(),
              BattleResultProjection{BattleResultState::ResultGenerationFailed,
                                     std::nullopt},
              emittedBattle)
              .has_value();
}

bool nonFailureAndWrongIdentityDoNotMutate() {
  auto room = inProgressRoom();
  if (!room.has_value()) {
    return false;
  }
  auto wrong = failedBattle();
  wrong.roomId = RoomId{8};
  std::optional<BattleInstanceId> emittedBattle;
  const auto result = recoverResultGenerationFailure(
      *room, wrong,
      BattleResultProjection{BattleResultState::Committed, std::nullopt},
      emittedBattle);
  return !result.has_value() && room->detail().has_value() &&
         !emittedBattle.has_value();
}

bool emptyAffectedRoomStillTerminatesWithoutLobbyRecipient() {
  auto room = inProgressRoom();
  if (!room.has_value() ||
      room->leave({.sessionId = SessionId{1},
                   .generation = SessionGeneration{1}}) != RoomResultCode::Ok ||
      room->leave({.sessionId = SessionId{2},
                   .generation = SessionGeneration{1}}) != RoomResultCode::Ok ||
      !room->closed()) {
    return false;
  }
  std::optional<BattleInstanceId> emittedBattle;
  const auto recovery = recoverResultGenerationFailure(
      *room, failedBattle(),
      BattleResultProjection{BattleResultState::ResultGenerationFailed,
                             std::nullopt},
      emittedBattle);
  if (!recovery.has_value() || !recovery->participants.empty()) {
    return false;
  }
  const auto intents = composeResultFailureVisibility(*recovery, {});
  return intents.size() == 2u &&
         std::holds_alternative<BattleRecoveryNotice>(intents[0].message) &&
         std::holds_alternative<LobbyRoomListUpdate>(intents[1].message);
}

} // namespace

int main() {
  return terminalFailureClosesOnceAndComposesNoticeBeforeLobby() &&
                 nonFailureAndWrongIdentityDoNotMutate() &&
                 emptyAffectedRoomStillTerminatesWithoutLobbyRecipient()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
