#include "workflows/CombatTickWorkflow.hpp"

#include <utility>

namespace lol::game_flow::workflows {

AttackWorkflowResult
applyAttack(std::optional<battle::BattleInstance> &battleInstance,
            const battle::AttackCommand &command,
            std::chrono::steady_clock::time_point receivedAt) {
  if (!battleInstance.has_value()) {
    return {
        .result =
            battle::AttackTerminalResult{
                .commandId = command.commandId,
                .battleId = command.battleId,
                .code = battle::AttackResultCode::StaleBattle,
                .monsterId = battle::CombatRuleset::monsterId,
                .remainingHitPoints = 0,
                .rulesetVersion = battle::CombatRuleset::version,
                .outcome = battle::CombatOutcome::None,
            },
        .combat = std::nullopt,
        .lootResolutionOpened = false,
    };
  }
  // Report the loot resolution transition caused by exactly this Attack: true
  // only for NotStarted -> Open. Replays, late attacks after the terminal, and
  // nonlethal attacks leave the resolution unchanged and stay false.
  const auto resolutionBefore = battleInstance->lootProjection().resolution;
  auto result = battleInstance->attack(command, receivedAt);
  const auto resolutionAfter = battleInstance->lootProjection().resolution;
  return {.result = std::move(result),
          .combat = battleInstance->combatProjection(),
          .lootResolutionOpened =
              resolutionBefore == battle::LootResolutionState::NotStarted &&
              resolutionAfter == battle::LootResolutionState::Open};
}

CombatDeadlineWorkflowResult
expireCombat(std::optional<battle::BattleInstance> &battleInstance,
             const battle::CombatDeadlineCommand &command,
             std::chrono::steady_clock::time_point completedAt) {
  if (!battleInstance.has_value()) {
    return {.code = battle::CombatDeadlineResultCode::StaleBattle,
            .combat = std::nullopt};
  }
  const auto code = battleInstance->expireCombat(command, completedAt);
  return {.code = code, .combat = battleInstance->combatProjection()};
}

LootDeadlineWorkflowResult
expireLoot(std::optional<battle::BattleInstance> &battleInstance,
           const battle::LootDeadlineCommand &command,
           std::chrono::steady_clock::time_point completedAt) {
  if (!battleInstance.has_value()) {
    return {.code = battle::LootDeadlineResultCode::StaleBattle};
  }
  return {.code = battleInstance->expireLoot(command, completedAt)};
}

} // namespace lol::game_flow::workflows
