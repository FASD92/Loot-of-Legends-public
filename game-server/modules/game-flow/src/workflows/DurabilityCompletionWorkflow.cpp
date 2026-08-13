#include "workflows/DurabilityCompletionWorkflow.hpp"

namespace lol::game_flow::workflows {

DurabilityCompletionResult completeSettlementDurability(
    lobby_room::Room &room, std::optional<battle::BattleInstance> &battle,
    std::optional<settlement::SettlementIntentBatch> &batch,
    const settlement::DurableAppendCompleted &completion) {
  if (completion.commitSequence == 0u || !battle.has_value() ||
      !batch.has_value() ||
      room.lifecycle() !=
          lobby_room::RoomLifecycle::AwaitingSettlementDurability ||
      completion.batchId != batch->id() ||
      completion.roomId != batch->roomId() ||
      completion.battleId != batch->battleId()) {
    return {.code = lobby_room::RoomResultCode::InvalidArgument,
            .finalResult = std::nullopt,
            .retiredBattle = std::nullopt,
            .retainedLootResults = std::nullopt,
            .applied = false};
  }
  const auto result = battle->resultProjection();
  const auto projection = battle->projection();
  if (result.state != battle::BattleResultState::Committed ||
      !result.result.has_value() ||
      result.result->roomId != completion.roomId ||
      result.result->battleId != completion.battleId ||
      projection.roomId != completion.roomId ||
      projection.battleId != completion.battleId ||
      room.reopenAfterSettlementDurability() !=
          lobby_room::RoomResultCode::Ok) {
    return {.code = lobby_room::RoomResultCode::InvalidArgument,
            .finalResult = std::nullopt,
            .retiredBattle = std::nullopt,
            .retainedLootResults = std::nullopt,
            .applied = false};
  }
  auto finalResult = result.result;
  auto retainedLootResults = battle->retainedLootResults();
  batch.reset();
  battle.reset();
  return {.code = lobby_room::RoomResultCode::Ok,
          .finalResult = std::move(finalResult),
          .retiredBattle = projection,
          .retainedLootResults = std::move(retainedLootResults),
          .applied = true};
}

} // namespace lol::game_flow::workflows
