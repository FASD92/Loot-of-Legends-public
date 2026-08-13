#pragma once

#include <lol/game_flow/RoomCommandGateway.hpp>
#include <lol/transport/tcp/BattleRecoveryProtocol.hpp>
#include <lol/transport/tcp/FinalResultProtocol.hpp>
#include <lol/transport/tcp/LobbyRoomProtocol.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace lol::app {

struct LobbyRoomDispatchResult final {
  transport::tcp::LobbyRoomCodecError codecError;
  std::optional<game_flow::RoomSubmitResult> submitResult;
};

struct EncodedLobbyRoomIntent final {
  game_flow::LobbyRoomOutboundAudience audience;
  std::vector<std::byte> frame;
};

class LobbyRoomFlow final {
public:
  explicit LobbyRoomFlow(game_flow::RoomCommandGateway &gateway) noexcept;

  [[nodiscard]] LobbyRoomDispatchResult
  submit(const game_flow::AuthenticatedRoomSession &session,
         std::span<const std::byte> frame);
  [[nodiscard]] static std::optional<EncodedLobbyRoomIntent>
  encode(const game_flow::LobbyRoomOutboundIntent &intent);

private:
  game_flow::RoomCommandGateway &gateway_;
};

} // namespace lol::app
