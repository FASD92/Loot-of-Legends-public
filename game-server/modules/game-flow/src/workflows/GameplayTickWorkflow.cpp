#include "workflows/GameplayTickWorkflow.hpp"

namespace lol::game_flow::workflows {

GameplayTickWorkflowResult
advanceGameplayTick(std::optional<battle::BattleInstance> &battle,
                    const battle::MovementTickCommand &command) {
  if (!battle.has_value()) {
    return {.code = battle::MovementResultCode::StaleBattle,
            .snapshot = std::nullopt};
  }
  const auto code = battle->integrateMovement(command);
  if (code != battle::MovementResultCode::Ok ||
      (command.serverTick % 2U) != 0U) {
    return {.code = code, .snapshot = std::nullopt};
  }
  return {.code = code, .snapshot = battle->captureStateSnapshot()};
}

} // namespace lol::game_flow::workflows
