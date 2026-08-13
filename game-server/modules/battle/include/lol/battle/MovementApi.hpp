#pragma once

#include <lol/shared/Identifiers.hpp>

#include <cstdint>
#include <vector>

namespace lol::battle {

struct DirectionIntent final {
  std::int16_t desiredX;
  std::int16_t desiredY;
  std::uint16_t inputFlags;
};

struct MoveCommand final {
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
  shared::BattleInstanceId battleId;
  std::uint32_t actionSequence;
  DirectionIntent direction;
};

struct MovementTickCommand final {
  shared::BattleInstanceId battleId;
  std::uint32_t serverTick;
};

enum class MovementResultCode : std::uint8_t {
  Ok,
  InvalidArgument,
  StaleSession,
  StaleBattle,
  NotEligible,
  StaleAction,
  StaleTick,
  RateLimited,
};

struct PlayerPositionProjection final {
  shared::SessionId sessionId;
  std::int32_t posXMillimeter;
  std::int32_t posYMillimeter;

  bool operator==(const PlayerPositionProjection &) const = default;
};

struct MovementProjection final {
  shared::BattleInstanceId battleId;
  std::uint32_t serverTick;
  std::vector<PlayerPositionProjection> players;
};

struct StateSnapshotProjection final {
  shared::BattleInstanceId battleId;
  std::uint32_t snapshotSequence;
  std::uint32_t serverTick;
  std::vector<PlayerPositionProjection> players;

  bool operator==(const StateSnapshotProjection &) const = default;
};

} // namespace lol::battle
