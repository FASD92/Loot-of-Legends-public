#pragma once

#include <lol/battle/MovementApi.hpp>
#include <lol/game_flow/RoomCommandGateway.hpp>
#include <lol/transport/rudp/RudpBindingRegistry.hpp>
#include <lol/transport/rudp/RudpMovementCodec.hpp>

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace lol::app {

class RudpGameplayReadiness final
    : public game_flow::GameplayTransportReadinessPort {
public:
  explicit RudpGameplayReadiness(
      const transport::rudp::RudpBindingRegistry &bindings) noexcept;

  [[nodiscard]] bool
  isReady(shared::SessionId sessionId,
          shared::SessionGeneration generation) const noexcept override;

private:
  const transport::rudp::RudpBindingRegistry &bindings_;
};

enum class RudpMovementSubmitResult {
  Accepted,
  Malformed,
  UnexpectedMessage,
  PeerRejected,
  StaleTransport,
  RoomRejected,
};

struct EncodedRudpDatagram final {
  transport::rudp::RudpEndpoint endpoint;
  std::vector<std::byte> datagram;
};

class RudpMovementFlow final {
public:
  RudpMovementFlow(transport::rudp::RudpBindingRegistry &bindings,
                   game_flow::RoomCommandGateway &gateway) noexcept;

  [[nodiscard]] RudpMovementSubmitResult
  submitMove(std::span<const std::byte> datagram,
             const transport::rudp::RudpEndpoint &endpoint,
             std::chrono::steady_clock::time_point receivedAt);
  [[nodiscard]] std::optional<std::vector<EncodedRudpDatagram>>
  encodeSnapshot(const battle::StateSnapshotProjection &snapshot);

private:
  transport::rudp::RudpBindingRegistry &bindings_;
  game_flow::RoomCommandGateway &gateway_;
};

} // namespace lol::app
