#pragma once

#include <lol/shared/Identifiers.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>

namespace lol::game_flow::execution {

struct SessionRoomRoute final {
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
  shared::RoomId roomId;

  bool operator==(const SessionRoomRoute &) const = default;
};

enum class RouteBindResult : std::uint8_t {
  Inserted,
  AlreadyBound,
  Conflict,
};

class SessionRouteIndex final {
public:
  [[nodiscard]] RouteBindResult bind(SessionRoomRoute route);
  [[nodiscard]] std::optional<shared::RoomId>
  lookup(shared::SessionId sessionId,
         shared::SessionGeneration generation) const;
  [[nodiscard]] bool clear(shared::SessionId sessionId,
                           shared::SessionGeneration generation,
                           shared::RoomId roomId);
  [[nodiscard]] std::size_t size() const;

private:
  mutable std::mutex mutex_;
  std::map<shared::SessionId, SessionRoomRoute> routes_;
};

} // namespace lol::game_flow::execution
