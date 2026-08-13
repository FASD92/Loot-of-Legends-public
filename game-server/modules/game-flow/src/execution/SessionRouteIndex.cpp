#include "execution/SessionRouteIndex.hpp"

namespace lol::game_flow::execution {

RouteBindResult SessionRouteIndex::bind(SessionRoomRoute route) {
  std::lock_guard lock{mutex_};
  const auto current = routes_.find(route.sessionId);
  if (current != routes_.end()) {
    return current->second == route ? RouteBindResult::AlreadyBound
                                    : RouteBindResult::Conflict;
  }
  routes_.emplace(route.sessionId, route);
  return RouteBindResult::Inserted;
}

std::optional<shared::RoomId>
SessionRouteIndex::lookup(shared::SessionId sessionId,
                          shared::SessionGeneration generation) const {
  std::lock_guard lock{mutex_};
  const auto route = routes_.find(sessionId);
  if (route == routes_.end() || route->second.generation != generation) {
    return std::nullopt;
  }
  return route->second.roomId;
}

bool SessionRouteIndex::clear(shared::SessionId sessionId,
                              shared::SessionGeneration generation,
                              shared::RoomId roomId) {
  std::lock_guard lock{mutex_};
  const auto route = routes_.find(sessionId);
  if (route == routes_.end() || route->second.generation != generation ||
      route->second.roomId != roomId) {
    return false;
  }
  routes_.erase(route);
  return true;
}

std::size_t SessionRouteIndex::size() const {
  std::lock_guard lock{mutex_};
  return routes_.size();
}

} // namespace lol::game_flow::execution
