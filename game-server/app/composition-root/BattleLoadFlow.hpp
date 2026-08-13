#pragma once

#include <lol/game_flow/RoomCommandGateway.hpp>
#include <lol/transport/tcp/BattleLoadProtocol.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace lol::app {

struct BattleLoadDispatchResult final {
  transport::tcp::BattleLoadCodecError codecError;
  std::optional<game_flow::RoomSubmitResult> submitResult;
};

struct EncodedBattleLoadIntent final {
  game_flow::LobbyRoomOutboundAudience audience;
  std::vector<std::byte> frame;
};

class BattleLoadFlow final {
public:
  explicit BattleLoadFlow(game_flow::RoomCommandGateway &gateway) noexcept;

  [[nodiscard]] BattleLoadDispatchResult
  submit(const game_flow::AuthenticatedRoomSession &session,
         std::span<const std::byte> frame);
  [[nodiscard]] static std::optional<EncodedBattleLoadIntent>
  encode(const game_flow::LobbyRoomOutboundIntent &intent);

private:
  game_flow::RoomCommandGateway &gateway_;
};

} // namespace lol::app
