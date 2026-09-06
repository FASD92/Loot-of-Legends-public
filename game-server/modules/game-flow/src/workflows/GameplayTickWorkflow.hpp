#pragma once

#include <lol/battle/BattleLoadApi.hpp>

#include <optional>

namespace lol::game_flow::workflows {

struct GameplayTickWorkflowResult final {
  battle::MovementResultCode code;
  std::optional<battle::StateSnapshotProjection> snapshot;
};

[[nodiscard]] GameplayTickWorkflowResult
advanceGameplayTick(std::optional<battle::BattleInstance> &battle,
                    const battle::MovementTickCommand &command,
                    battle::BattleTime at);

} // namespace lol::game_flow::workflows
