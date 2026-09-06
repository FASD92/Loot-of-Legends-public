#pragma once

#include <lol/battle/BattleLoadApi.hpp>

#include <optional>

namespace lol::game_flow::workflows {

struct AttackWorkflowResult final {
  battle::AttackTerminalResult result;
  std::optional<battle::AttackAppliedRecord> applied;
  std::optional<battle::CombatProjection> combat;
  // True exactly on the single NotStarted -> Open loot resolution transition
  // caused by this Attack. The Cell schedules exactly one 15 000 ms loot
  // deadline from this report.
  bool lootResolutionOpened;
};

struct CombatDeadlineWorkflowResult final {
  battle::CombatDeadlineResultCode code;
  std::optional<battle::CombatProjection> combat;
};

struct LootDeadlineWorkflowResult final {
  battle::LootDeadlineResultCode code;
};

[[nodiscard]] AttackWorkflowResult
applyAttack(std::optional<battle::BattleInstance> &battle,
            const battle::AttackCommand &command,
            battle::BattleTime receivedAt);

[[nodiscard]] CombatDeadlineWorkflowResult
expireCombat(std::optional<battle::BattleInstance> &battle,
             const battle::CombatDeadlineCommand &command);

[[nodiscard]] LootDeadlineWorkflowResult
expireLoot(std::optional<battle::BattleInstance> &battle,
           const battle::LootDeadlineCommand &command);

} // namespace lol::game_flow::workflows
