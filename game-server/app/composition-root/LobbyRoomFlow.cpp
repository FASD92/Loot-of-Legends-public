#include "LobbyRoomFlow.hpp"

#include <lol/lobby_room/RoomProjections.hpp>

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace lol::app {
namespace {

game_flow::LobbyRoomClientMessage
normalize(transport::tcp::LobbyRoomClientMessage message) {
  return std::visit(
      [](auto request) -> game_flow::LobbyRoomClientMessage {
        using Request = std::remove_cvref_t<decltype(request)>;
        if constexpr (std::is_same_v<Request, transport::tcp::CreateRoom>) {
          return game_flow::CreateRoomRequest{
              .requestId = shared::RequestId{request.requestId},
              .title = std::move(request.title),
              .capacity = request.capacity,
          };
        } else if constexpr (std::is_same_v<Request,
                                            transport::tcp::JoinRoom>) {
          return game_flow::JoinRoomRequest{
              .requestId = shared::RequestId{request.requestId},
              .roomId = shared::RoomId{request.roomId},
          };
        } else if constexpr (std::is_same_v<Request,
                                            transport::tcp::LeaveRoom>) {
          return game_flow::LeaveRoomRequest{
              .requestId = shared::RequestId{request.requestId},
          };
        } else if constexpr (std::is_same_v<Request,
                                            transport::tcp::SetReady>) {
          return game_flow::SetReadyRequest{
              .requestId = shared::RequestId{request.requestId},
              .ready = request.ready,
          };
        } else {
          return game_flow::KickRoomMemberRequest{
              .requestId = shared::RequestId{request.requestId},
              .targetSessionId = shared::SessionId{request.targetSessionId},
              .targetGeneration =
                  shared::SessionGeneration{request.targetSessionGeneration},
          };
        }
      },
      std::move(message));
}

transport::tcp::RoomSummary toWire(const lobby_room::RoomSummary &summary) {
  return transport::tcp::RoomSummary{
      .roomId = summary.roomId.value(),
      .title = summary.title,
      .memberCount = summary.memberCount,
      .capacity = summary.capacity,
  };
}

std::vector<transport::tcp::RoomSummary>
toWire(const std::vector<lobby_room::RoomSummary> &summaries) {
  std::vector<transport::tcp::RoomSummary> result;
  result.reserve(summaries.size());
  for (const auto &summary : summaries) {
    result.push_back(toWire(summary));
  }
  return result;
}

transport::tcp::RoomDetailProjection
toWire(const lobby_room::RoomDetailProjection &detail) {
  transport::tcp::RoomDetailProjection result{
      .roomId = detail.roomId.value(),
      .title = detail.title,
      .capacity = detail.capacity,
      .hostSessionId = detail.hostSessionId.value(),
      .hostSessionGeneration = detail.hostSessionGeneration.value(),
      .members = {},
  };
  result.members.reserve(detail.members.size());
  for (const auto &member : detail.members) {
    result.members.push_back(transport::tcp::RoomMember{
        .sessionId = member.sessionId.value(),
        .sessionGeneration = member.sessionGeneration.value(),
        .nickname = member.nickname,
        .ready = member.ready,
    });
  }
  return result;
}

std::optional<transport::tcp::FinalResult>
toWire(const battle::BattleFinalResult &result) {
  transport::tcp::FinalResultOutcome outcome;
  switch (result.outcome) {
  case battle::BattleOutcome::MonsterDefeated:
    outcome = transport::tcp::FinalResultOutcome::MonsterDefeated;
    break;
  case battle::BattleOutcome::CombatTimeout:
    outcome = transport::tcp::FinalResultOutcome::CombatTimeout;
    break;
  case battle::BattleOutcome::CancelledNoActiveParticipants:
    outcome = transport::tcp::FinalResultOutcome::CancelledNoActiveParticipants;
    break;
  default:
    return std::nullopt;
  }
  transport::tcp::FinalResult wire{
      .roomId = result.roomId.value(),
      .battleInstanceId = result.battleId.value(),
      .outcome = outcome,
      .entries = {},
  };
  wire.entries.reserve(result.entries.size());
  for (const auto &entry : result.entries) {
    transport::tcp::FinalResultExitStatus exitStatus;
    switch (entry.exitStatus) {
    case battle::ParticipantExitStatus::TerminalPresent:
      exitStatus = transport::tcp::FinalResultExitStatus::TerminalPresent;
      break;
    case battle::ParticipantExitStatus::TerminalExited:
      exitStatus = transport::tcp::FinalResultExitStatus::TerminalExited;
      break;
    case battle::ParticipantExitStatus::GameplayEligible:
    case battle::ParticipantExitStatus::VoluntaryLeft:
    case battle::ParticipantExitStatus::Disconnected:
      return std::nullopt;
    default:
      return std::nullopt;
    }
    wire.entries.push_back(transport::tcp::FinalResultEntry{
        .sessionId = entry.sessionId.value(),
        .nickname = entry.nickname,
        .exitStatus = exitStatus,
        .finalAssetValue = entry.finalAssetValue,
        .rank = entry.rank.value_or(0u),
        .isTop = entry.isTop,
    });
  }
  return wire;
}

std::optional<transport::tcp::BattleRecoveryNotice>
toWire(const game_flow::BattleRecoveryNotice &notice) {
  transport::tcp::BattleRecoveryReason reason;
  switch (notice.reason) {
  case game_flow::BattleRecoveryReason::ResultGenerationFailed:
    reason = transport::tcp::BattleRecoveryReason::ResultGenerationFailed;
    break;
  case game_flow::BattleRecoveryReason::SettlementRecoveryPending:
    reason = transport::tcp::BattleRecoveryReason::SettlementRecoveryPending;
    break;
  default:
    return std::nullopt;
  }
  return transport::tcp::BattleRecoveryNotice{
      .roomId = notice.roomId.value(),
      .battleInstanceId = notice.battleId.value(),
      .reason = reason,
  };
}

std::optional<transport::tcp::LobbyRoomServerMessage>
toWire(const game_flow::LobbyRoomServerMessage &message) {
  return std::visit(
      [](const auto &value)
          -> std::optional<transport::tcp::LobbyRoomServerMessage> {
        using Message = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Message, game_flow::LobbyEntrySnapshot>) {
          return transport::tcp::LobbyEntrySnapshot{
              .sessionId = value.session.sessionId.value(),
              .sessionGeneration = value.session.generation.value(),
              .nickname = value.session.nickname,
              .rooms = toWire(value.rooms),
          };
        } else if constexpr (std::is_same_v<Message,
                                            game_flow::LobbyRoomListUpdate>) {
          return transport::tcp::LobbyRoomListUpdate{
              .rooms = toWire(value.rooms),
          };
        } else if constexpr (std::is_same_v<Message,
                                            game_flow::RoomCommandResponse>) {
          return transport::tcp::RoomCommandResponse{
              .requestId = value.requestId.value(),
              .resultCode = static_cast<std::uint16_t>(value.result),
          };
        } else if constexpr (std::is_same_v<Message,
                                            lobby_room::RoomDetailProjection>) {
          return toWire(value);
        } else {
          return std::nullopt;
        }
      },
      message);
}

} // namespace

LobbyRoomFlow::LobbyRoomFlow(game_flow::RoomCommandGateway &gateway) noexcept
    : gateway_(gateway) {}

LobbyRoomDispatchResult
LobbyRoomFlow::submit(const game_flow::AuthenticatedRoomSession &session,
                      std::span<const std::byte> frame) {
  auto decoded =
      transport::tcp::LobbyRoomProtocolCodec::decodeClientFrame(frame);
  if (decoded.error != transport::tcp::LobbyRoomCodecError::None ||
      !decoded.message.has_value()) {
    return {.codecError = decoded.error, .submitResult = std::nullopt};
  }
  return {
      .codecError = transport::tcp::LobbyRoomCodecError::None,
      .submitResult = gateway_.submit(game_flow::RoomCommandEnvelope{
          .session = session,
          .command = normalize(std::move(*decoded.message)),
      }),
  };
}

std::optional<EncodedLobbyRoomIntent>
LobbyRoomFlow::encode(const game_flow::LobbyRoomOutboundIntent &intent) {
  if (const auto *recovery =
          std::get_if<game_flow::BattleRecoveryNotice>(&intent.message)) {
    const auto wire = toWire(*recovery);
    if (!wire.has_value()) {
      return std::nullopt;
    }
    auto frame =
        transport::tcp::BattleRecoveryProtocolCodec::encodeServerFrame(*wire);
    if (!frame.has_value()) {
      return std::nullopt;
    }
    return EncodedLobbyRoomIntent{
        .audience = intent.audience,
        .frame = std::move(*frame),
    };
  }
  if (const auto *result =
          std::get_if<battle::BattleFinalResult>(&intent.message)) {
    const auto wire = toWire(*result);
    if (!wire.has_value()) {
      return std::nullopt;
    }
    auto frame =
        transport::tcp::FinalResultProtocolCodec::encodeServerFrame(*wire);
    if (!frame.has_value()) {
      return std::nullopt;
    }
    return EncodedLobbyRoomIntent{
        .audience = intent.audience,
        .frame = std::move(*frame),
    };
  }
  const auto message = toWire(intent.message);
  if (!message.has_value()) {
    return std::nullopt;
  }
  auto frame =
      transport::tcp::LobbyRoomProtocolCodec::encodeServerFrame(*message);
  if (!frame.has_value()) {
    return std::nullopt;
  }
  return EncodedLobbyRoomIntent{
      .audience = intent.audience,
      .frame = std::move(*frame),
  };
}

} // namespace lol::app
