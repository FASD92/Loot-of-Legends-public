#include "RudpMovementFlow.hpp"

#include <lol/shared/Identifiers.hpp>

#include <utility>

namespace lol::app {

RudpGameplayReadiness::RudpGameplayReadiness(
    const transport::rudp::RudpBindingRegistry &bindings) noexcept
    : bindings_(bindings) {}

bool RudpGameplayReadiness::isReady(
    shared::SessionId sessionId,
    shared::SessionGeneration generation) const noexcept {
  return bindings_.isBound(sessionId.value(), generation.value());
}

RudpMovementFlow::RudpMovementFlow(
    transport::rudp::RudpBindingRegistry &bindings,
    game_flow::RoomCommandGateway &gateway) noexcept
    : bindings_(bindings), gateway_(gateway) {}

RudpMovementSubmitResult
RudpMovementFlow::submitMove(std::span<const std::byte> datagram,
                             const transport::rudp::RudpEndpoint &endpoint,
                             std::chrono::steady_clock::time_point receivedAt) {
  const auto decoded = transport::rudp::RudpMovementCodec::decode(datagram);
  if (decoded.error != transport::rudp::RudpMovementCodecError::None ||
      !decoded.header.has_value() || !decoded.message.has_value()) {
    return RudpMovementSubmitResult::Malformed;
  }
  const auto *move =
      std::get_if<transport::rudp::RudpMoveIntent>(&*decoded.message);
  if (move == nullptr) {
    return RudpMovementSubmitResult::UnexpectedMessage;
  }
  const auto received =
      bindings_.receive(*decoded.header, endpoint, receivedAt);
  if (received.status != transport::rudp::RudpPacketStatus::Current ||
      !received.disposition.has_value()) {
    return RudpMovementSubmitResult::PeerRejected;
  }
  if (*received.disposition != transport::rudp::ReceiveDisposition::Newest) {
    return RudpMovementSubmitResult::StaleTransport;
  }
  const auto submitted = gateway_.submitMovement(
      battle::MoveCommand{
          .sessionId = shared::SessionId{decoded.header->sessionId},
          .generation =
              shared::SessionGeneration{decoded.header->sessionGeneration},
          .battleId = shared::BattleInstanceId{move->battleInstanceId},
          .actionSequence = move->actionSequence,
          .direction = battle::DirectionIntent{.desiredX = move->desiredX,
                                               .desiredY = move->desiredY,
                                               .inputFlags = move->inputFlags},
      },
      receivedAt);
  return submitted == game_flow::RoomSubmitResult::Accepted
             ? RudpMovementSubmitResult::Accepted
             : RudpMovementSubmitResult::RoomRejected;
}

std::optional<std::vector<EncodedRudpDatagram>>
RudpMovementFlow::encodeSnapshot(
    const battle::StateSnapshotProjection &snapshot) {
  transport::rudp::RudpStateSnapshot message{
      .battleInstanceId = snapshot.battleId.value(),
      .snapshotSequence = snapshot.snapshotSequence,
      .serverTick = snapshot.serverTick,
      .players = {},
  };
  message.players.reserve(snapshot.players.size());
  for (const auto &player : snapshot.players) {
    message.players.push_back(transport::rudp::RudpSnapshotPlayer{
        .sessionId = player.sessionId.value(),
        .posXMillimeter = player.posXMillimeter,
        .posYMillimeter = player.posYMillimeter,
    });
  }

  std::vector<EncodedRudpDatagram> datagrams;
  datagrams.reserve(snapshot.players.size());
  for (const auto &player : snapshot.players) {
    const auto route = bindings_.nextOutbound(player.sessionId.value());
    if (!route.has_value()) {
      continue;
    }
    auto encoded = transport::rudp::RudpMovementCodec::encode(
        transport::rudp::RudpHeader{
            .flag = transport::rudp::RudpFlag::Unreliable,
            .sessionId = route->sessionId,
            .sessionGeneration = route->sessionGeneration,
            .transportEpoch = route->transportEpoch,
            .sequence = route->sequence,
            .ack = route->ack.ack,
            .ackBits = route->ack.ackBits,
            .messageId = 26,
        },
        transport::rudp::RudpMovementMessage{message});
    if (!encoded.has_value()) {
      return std::nullopt;
    }
    datagrams.push_back(EncodedRudpDatagram{
        .endpoint = route->endpoint,
        .datagram = std::move(*encoded),
    });
  }
  return datagrams;
}

} // namespace lol::app
