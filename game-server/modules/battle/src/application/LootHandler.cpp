#include <lol/battle/BattleLoadApi.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

namespace lol::battle {
namespace {

constexpr std::int64_t kClaimRangeMillimeters = 1500;

// Inclusive integer claim range. Both coordinates convert to int64_t before
// subtraction; an axis delta outside [-1500, 1500] rejects first, and only
// then is dx*dx + dy*dy compared against 1500*1500. This avoids overflow for
// arbitrary signed 32-bit inputs. Floating point and sqrt are not used.
bool inClaimRange(DropPosition drop, std::int32_t participantXMillimeter,
                  std::int32_t participantYMillimeter) noexcept {
  const auto deltaX = static_cast<std::int64_t>(drop.xMillimeter) -
                      static_cast<std::int64_t>(participantXMillimeter);
  const auto deltaY = static_cast<std::int64_t>(drop.yMillimeter) -
                      static_cast<std::int64_t>(participantYMillimeter);
  if (deltaX < -kClaimRangeMillimeters || deltaX > kClaimRangeMillimeters ||
      deltaY < -kClaimRangeMillimeters || deltaY > kClaimRangeMillimeters) {
    return false;
  }
  return deltaX * deltaX + deltaY * deltaY <=
         kClaimRangeMillimeters * kClaimRangeMillimeters;
}

ClaimLootTerminalResult reject(const ClaimLootCommand &command,
                               ClaimLootResultCode code) noexcept {
  return ClaimLootTerminalResult{
      .commandId = command.commandId,
      .battleId = command.battleId,
      .dropId = command.dropId,
      .code = code,
  };
}

} // namespace

ClaimLootTerminalResult
BattleInstance::claimLoot(const ClaimLootCommand &command,
                          std::chrono::steady_clock::time_point receivedAt) {
  if (command.battleId != battleId_) {
    return reject(command, ClaimLootResultCode::StaleBattle);
  }
  if (state_ != BattleLoadState::GameplayCommitted) {
    return reject(command, ClaimLootResultCode::NotEligible);
  }
  auto *participant = participantRecord(command.sessionId);
  if (participant == nullptr) {
    return reject(command, ClaimLootResultCode::NotEligible);
  }
  if (participant->generation != command.generation) {
    return reject(command, ClaimLootResultCode::StaleSession);
  }

  const auto inspection = participant->lootResults.inspect(command);
  if (inspection.decision == LootResultStoreDecision::Replay) {
    return *inspection.result;
  }
  if (inspection.decision == LootResultStoreDecision::Conflict) {
    return reject(command, ClaimLootResultCode::CommandConflict);
  }
  if (inspection.decision == LootResultStoreDecision::Overloaded) {
    return reject(command, ClaimLootResultCode::Overloaded);
  }
  if (inspection.decision != LootResultStoreDecision::Available) {
    std::terminate();
  }
  const auto retain = [participant, &command](ClaimLootResultCode code) {
    auto result = reject(command, code);
    if (!participant->lootResults.retain(command, result)) {
      std::terminate();
    }
    return result;
  };
  if (!participant->lastClaimRateUpdate.has_value()) {
    participant->lastClaimRateUpdate = receivedAt;
  } else if (receivedAt > *participant->lastClaimRateUpdate) {
    const auto elapsed = std::min(
        receivedAt - *participant->lastClaimRateUpdate,
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            fullClaimRateRefill));
    const auto elapsedNanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    participant->claimRateCreditUnits =
        std::min(claimRateCapacityUnits,
                 participant->claimRateCreditUnits +
                     static_cast<std::uint64_t>(elapsedNanoseconds) *
                         claimRateUnitsPerNanosecond);
    participant->lastClaimRateUpdate = receivedAt;
  }
  if (participant->claimRateCreditUnits < claimRateTokenUnits) {
    return retain(ClaimLootResultCode::Overloaded);
  }
  participant->claimRateCreditUnits -= claimRateTokenUnits;

  if (!participant->gameplayEligible) {
    return retain(ClaimLootResultCode::NotEligible);
  }
  if (!combatTerminal_.has_value() ||
      combatTerminal_->outcome != CombatOutcome::MonsterDefeated) {
    return retain(ClaimLootResultCode::NotEligible);
  }
  // A fresh claim after the resolution window closed (or before drop
  // generation) is retained as ResolutionClosed; only a replayed accepted
  // command above ever returns Ok again.
  if (lootResolution_ != LootResolutionState::Open) {
    return retain(ClaimLootResultCode::ResolutionClosed);
  }
  if (command.dropId.value == 0) {
    return retain(ClaimLootResultCode::InvalidDrop);
  }
  LootDropRecord *dropRecord = nullptr;
  for (auto &candidate : drops_) {
    if (candidate.drop.dropId == command.dropId) {
      dropRecord = &candidate;
      break;
    }
  }
  if (dropRecord == nullptr) {
    return retain(ClaimLootResultCode::UnknownDrop);
  }
  if (dropRecord->state == LootDropState::Claimed) {
    return retain(ClaimLootResultCode::AlreadyClaimed);
  }
  if (dropRecord->state == LootDropState::Unclaimed) {
    return retain(ClaimLootResultCode::ResolutionClosed);
  }
  if (!inClaimRange(dropRecord->drop.position, participant->posXMillimeter,
                    participant->posYMillimeter)) {
    return retain(ClaimLootResultCode::OutOfRange);
  }
  const auto catalog = RelicCatalog::v1Snapshot();
  if (!catalog.unitValueOf(dropRecord->drop.itemId).has_value() ||
      dropRecord->drop.quantity == 0) {
    return retain(ClaimLootResultCode::CatalogRejected);
  }

  dropRecord->state = LootDropState::Claimed;
  dropRecord->owner = participant->sessionId;
  const auto existing = std::find_if(
      holdings_.begin(), holdings_.end(), [&](const HoldingRecord &holding) {
        return holding.sessionId == participant->sessionId &&
               holding.itemId == dropRecord->drop.itemId;
      });
  if (existing != holdings_.end()) {
    existing->quantity += dropRecord->drop.quantity;
  } else {
    holdings_.push_back(HoldingRecord{
        .sessionId = participant->sessionId,
        .itemId = dropRecord->drop.itemId,
        .quantity = dropRecord->drop.quantity,
    });
  }
  const bool anyAvailable = std::any_of(
      drops_.begin(), drops_.end(), [](const LootDropRecord &record) {
        return record.state == LootDropState::Available;
      });
  if (!anyAvailable) {
    lootResolution_ = LootResolutionState::Resolved;
    // The final Available Drop became Claimed: commit exactly once. Replayed
    // or later commands cannot rebuild or mutate the committed result.
    commitResultIfReady(receivedAt);
  }
  return retain(ClaimLootResultCode::Ok);
}

LootProjection BattleInstance::lootProjection() const {
  std::vector<LootDropProjection> drops;
  drops.reserve(drops_.size());
  for (const auto &record : drops_) {
    drops.push_back(LootDropProjection{
        .dropId = record.drop.dropId,
        .itemId = record.drop.itemId,
        .quantity = record.drop.quantity,
        .position = record.drop.position,
        .state = record.state,
        .owner = record.owner,
    });
  }
  std::vector<LootHoldingProjection> holdings;
  holdings.reserve(holdings_.size());
  for (const auto &holding : holdings_) {
    holdings.push_back(LootHoldingProjection{
        .sessionId = holding.sessionId,
        .itemId = holding.itemId,
        .quantity = holding.quantity,
    });
  }
  return LootProjection{
      .battleId = battleId_,
      .resolution = lootResolution_,
      .drops = std::move(drops),
      .holdings = std::move(holdings),
  };
}

} // namespace lol::battle
