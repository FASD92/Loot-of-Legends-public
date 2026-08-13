#pragma once

#include <lol/battle/BattleLoadApi.hpp>
#include <lol/lobby_room/RoomApi.hpp>
#include <lol/settlement/SettlementIntent.hpp>
#include <lol/settlement/SettlementPublication.hpp>

#include <optional>

namespace lol::game_flow::workflows {

struct DurabilityCompletionResult final {
  lobby_room::RoomResultCode code;
  std::optional<battle::BattleFinalResult> finalResult;
  std::optional<battle::BattleLoadProjection> retiredBattle;
  std::optional<battle::RetainedLootResults> retainedLootResults;
  bool applied;
};

[[nodiscard]] DurabilityCompletionResult completeSettlementDurability(
    lobby_room::Room &room, std::optional<battle::BattleInstance> &battle,
    std::optional<settlement::SettlementIntentBatch> &batch,
    const settlement::DurableAppendCompleted &completion);

} // namespace lol::game_flow::workflows
