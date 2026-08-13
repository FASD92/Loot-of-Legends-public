#include "workflows/BattleTerminalWorkflow.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace lol::game_flow::workflows {
namespace {

std::optional<settlement::BattleOutcome>
toSettlementOutcome(battle::BattleOutcome outcome) noexcept {
  switch (outcome) {
  case battle::BattleOutcome::MonsterDefeated:
    return settlement::BattleOutcome::MonsterDefeated;
  case battle::BattleOutcome::CombatTimeout:
    return settlement::BattleOutcome::CombatTimeout;
  case battle::BattleOutcome::CancelledNoActiveParticipants:
    return settlement::BattleOutcome::CancelledNoActiveParticipants;
  }
  return std::nullopt;
}

std::optional<settlement::ParticipantExitStatus>
toSettlementExitStatus(battle::ParticipantExitStatus status) noexcept {
  switch (status) {
  case battle::ParticipantExitStatus::TerminalPresent:
    return settlement::ParticipantExitStatus::TerminalPresent;
  case battle::ParticipantExitStatus::TerminalExited:
    return settlement::ParticipantExitStatus::TerminalExited;
  case battle::ParticipantExitStatus::GameplayEligible:
  case battle::ParticipantExitStatus::VoluntaryLeft:
  case battle::ParticipantExitStatus::Disconnected:
    return std::nullopt;
  }
  return std::nullopt;
}

} // namespace

std::optional<settlement::SettlementIntentBatch>
holdTerminalForSettlementDurability(lobby_room::Room &room,
                                    const battle::BattleInstance &battle,
                                    settlement::ResultCommittedAt committedAt) {
  const auto resultProjection = battle.resultProjection();
  if (resultProjection.state != battle::BattleResultState::Committed ||
      !resultProjection.result.has_value()) {
    return std::nullopt;
  }

  const auto &result = *resultProjection.result;
  const auto load = battle.projection();
  const auto loot = battle.lootProjection();
  const auto outcome = toSettlementOutcome(result.outcome);
  if (!outcome.has_value() || result.roomId != load.roomId ||
      result.battleId != load.battleId || loot.battleId != result.battleId ||
      result.entries.size() != load.capturedParticipants.size()) {
    return std::nullopt;
  }

  std::vector<settlement::SettlementParticipantSource> participants;
  participants.reserve(result.entries.size());
  for (const auto &entry : result.entries) {
    const auto captured = std::find_if(
        load.capturedParticipants.begin(), load.capturedParticipants.end(),
        [&entry](const battle::CapturedParticipant &participant) {
          return participant.sessionId == entry.sessionId;
        });
    const auto exitStatus = toSettlementExitStatus(entry.exitStatus);
    if (captured == load.capturedParticipants.end() ||
        !exitStatus.has_value()) {
      return std::nullopt;
    }

    std::vector<settlement::ItemDelta> itemDeltas;
    for (const auto &holding : loot.holdings) {
      if (holding.sessionId == entry.sessionId) {
        itemDeltas.push_back(settlement::ItemDelta{
            .itemId = holding.itemId.value,
            .quantity = holding.quantity,
        });
      }
    }
    participants.push_back(settlement::SettlementParticipantSource{
        .accountId = captured->accountId,
        .exitStatus = *exitStatus,
        .itemDeltas = std::move(itemDeltas),
        .finalAssetValue = entry.finalAssetValue,
    });
  }

  auto batch = settlement::createSettlementIntentBatch(
      settlement::SettlementIntentBatchSource{
          .roomId = result.roomId,
          .battleId = result.battleId,
          .outcome = *outcome,
          .catalogVersion = battle::RelicCatalog::version,
          .committedAt = committedAt,
          .participants = std::move(participants),
      });
  if (!batch.has_value() || room.commitAwaitingSettlementDurability() !=
                                lobby_room::RoomResultCode::Ok) {
    return std::nullopt;
  }
  return batch;
}

} // namespace lol::game_flow::workflows
