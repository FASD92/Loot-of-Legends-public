#include "BattleLoadFlow.hpp"

#include <cstdint>
#include <type_traits>
#include <utility>

namespace lol::app {
namespace {

game_flow::LobbyRoomClientMessage
normalize(transport::tcp::BattleLoadClientMessage message) {
  return std::visit(
      [](const auto &request) -> game_flow::LobbyRoomClientMessage {
        using Request = std::remove_cvref_t<decltype(request)>;
        if constexpr (std::is_same_v<Request,
                                     transport::tcp::HostStartRequest>) {
          return game_flow::HostStartRequest{
              .requestId = shared::RequestId{request.requestId},
          };
        } else {
          return game_flow::ArenaLoadCompleteRequest{
              .requestId = shared::RequestId{request.requestId},
              .roomId = shared::RoomId{request.roomId},
              .battleId = shared::BattleInstanceId{request.battleInstanceId},
          };
        }
      },
      std::move(message));
}

std::optional<transport::tcp::BattleLoadServerMessage>
toWire(const game_flow::LobbyRoomServerMessage &message) {
  return std::visit(
      [](const auto &value)
          -> std::optional<transport::tcp::BattleLoadServerMessage> {
        using Message = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Message,
                                     game_flow::BattleCommandResponse>) {
          return transport::tcp::BattleCommandResponse{
              .requestId = value.requestId.value(),
              .resultCode = static_cast<std::uint16_t>(value.result),
          };
        } else if constexpr (std::is_same_v<Message,
                                            game_flow::ArenaLoadEntry>) {
          return transport::tcp::ArenaLoadEntry{
              .roomId = value.roomId.value(),
              .battleInstanceId = value.battleId.value(),
          };
        } else if constexpr (std::is_same_v<Message,
                                            game_flow::ArenaGameplayStart>) {
          transport::tcp::ArenaGameplayStart result{
              .roomId = value.roomId.value(),
              .battleInstanceId = value.battleId.value(),
              .participants = {},
          };
          result.participants.reserve(value.participants.size());
          for (const auto &participant : value.participants) {
            result.participants.push_back(transport::tcp::BattleParticipant{
                .sessionId = participant.sessionId.value(),
                .sessionGeneration = participant.generation.value(),
                .nickname = participant.nickname,
            });
          }
          return result;
        } else if constexpr (std::is_same_v<Message,
                                            game_flow::ArenaLoadCancelled>) {
          return transport::tcp::ArenaLoadCancelled{
              .roomId = value.roomId.value(),
              .battleInstanceId = value.battleId.value(),
              .reasonCode = static_cast<std::uint16_t>(value.reason),
          };
        } else {
          return std::nullopt;
        }
      },
      message);
}

} // namespace

BattleLoadFlow::BattleLoadFlow(game_flow::RoomCommandGateway &gateway) noexcept
    : gateway_(gateway) {}

BattleLoadDispatchResult
BattleLoadFlow::submit(const game_flow::AuthenticatedRoomSession &session,
                       std::span<const std::byte> frame) {
  auto decoded =
      transport::tcp::BattleLoadProtocolCodec::decodeClientFrame(frame);
  if (decoded.error != transport::tcp::BattleLoadCodecError::None ||
      !decoded.message.has_value()) {
    return {.codecError = decoded.error, .submitResult = std::nullopt};
  }
  return {
      .codecError = transport::tcp::BattleLoadCodecError::None,
      .submitResult = gateway_.submit(game_flow::RoomCommandEnvelope{
          .session = session,
          .command = normalize(std::move(*decoded.message)),
      }),
  };
}

std::optional<EncodedBattleLoadIntent>
BattleLoadFlow::encode(const game_flow::LobbyRoomOutboundIntent &intent) {
  const auto message = toWire(intent.message);
  if (!message.has_value()) {
    return std::nullopt;
  }
  auto frame =
      transport::tcp::BattleLoadProtocolCodec::encodeServerFrame(*message);
  if (!frame.has_value()) {
    return std::nullopt;
  }
  return EncodedBattleLoadIntent{
      .audience = intent.audience,
      .frame = std::move(*frame),
  };
}

} // namespace lol::app
