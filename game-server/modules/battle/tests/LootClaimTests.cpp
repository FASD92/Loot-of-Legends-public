#include <lol/battle/BattleLoadApi.hpp>
#include <lol/battle/LootApi.hpp>
#include <lol/battle/LootResultStore.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using lol::battle::ArenaLoadCompleteCommand;
using lol::battle::AttackCommand;
using lol::battle::AttackResultCode;
using lol::battle::BattleAdmissionSnapshot;
using lol::battle::BattleInstance;
using lol::battle::BattleLoadProjection;
using lol::battle::BattleLoadResultCode;
using lol::battle::BattleLoadState;
using lol::battle::BattleStartCandidate;
using lol::battle::CandidateDisconnectedCommand;
using lol::battle::ClaimLootCommand;
using lol::battle::ClaimLootResultCode;
using lol::battle::ClaimLootTerminalResult;
using lol::battle::CombatRuleset;
using lol::battle::CommandId;
using lol::battle::DirectionIntent;
using lol::battle::DropId;
using lol::battle::ItemId;
using lol::battle::LootDeadlineCommand;
using lol::battle::LootDeadlineResultCode;
using lol::battle::LootDropProjection;
using lol::battle::LootDropState;
using lol::battle::LootProjection;
using lol::battle::LootResolutionState;
using lol::battle::LootResultStore;
using lol::battle::LootResultStoreDecision;
using lol::battle::MoveCommand;
using lol::battle::MovementResultCode;
using lol::battle::MovementTickCommand;
using lol::battle::ParticipantExitStatus;
using lol::battle::RelicCatalog;
using lol::shared::AccountId;
using lol::shared::BattleInstanceId;
using lol::shared::RoomId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;

// The command surface is exactly the five fixed fields: identity, session,
// generation, battle, drop. There is no client-supplied item, unit value,
// quantity, position, eligibility, or owner payload on the wire structure.
static_assert(
    sizeof(ClaimLootCommand) == sizeof(CommandId) + sizeof(SessionId) +
                                    sizeof(SessionGeneration) +
                                    sizeof(BattleInstanceId) + sizeof(DropId),
    "ClaimLootCommand must carry exactly the five fixed command fields");

constexpr std::int32_t kTickMillimeters = 250;

AccountId account(std::uint8_t suffix) {
  AccountId::Bytes bytes{};
  bytes.back() = suffix;
  return AccountId{bytes};
}

BattleAdmissionSnapshot admission(std::uint64_t battleId, std::size_t count) {
  std::vector<BattleStartCandidate> candidates;
  for (std::size_t index = 0; index < count; ++index) {
    const auto session = static_cast<std::uint64_t>(index + 1);
    candidates.push_back(BattleStartCandidate{
        .accountId = account(static_cast<std::uint8_t>(session)),
        .sessionId = SessionId{session},
        .generation = SessionGeneration{1},
        .nickname = "player-" + std::to_string(session),
    });
  }
  return BattleAdmissionSnapshot{
      .roomId = RoomId{7},
      .battleId = BattleInstanceId{battleId},
      .candidates = std::move(candidates),
  };
}

BattleInstance openCommittedBattle(std::uint64_t battleId, std::size_t count) {
  auto result = BattleInstance::create(admission(battleId, count));
  if (result.code != BattleLoadResultCode::Ok || !result.battle.has_value() ||
      result.battle->openLoadBarrier() != BattleLoadResultCode::Ok) {
    std::abort();
  }
  auto battle = std::move(*result.battle);
  for (std::size_t index = 0; index < count; ++index) {
    const auto session = static_cast<std::uint64_t>(index + 1);
    if (battle.completeLoad(
            ArenaLoadCompleteCommand{.sessionId = SessionId{session},
                                     .generation = SessionGeneration{1},
                                     .roomId = RoomId{7},
                                     .battleId = BattleInstanceId{battleId}},
            true) != BattleLoadResultCode::Ok) {
      std::abort();
    }
  }
  if (battle.projection().state != BattleLoadState::GameplayCommitted) {
    std::abort();
  }
  return battle;
}

// Player 1 (session 1, generation 1) defeats the monster from the spawn-safe
// origin position. 80 fixed 20-damage hits empty the fixed 1600 HP pool and
// produce the MonsterDefeated combat terminal.
void killMonster(BattleInstance &battle, std::uint64_t battleId,
                 std::uint64_t sessionId) {
  constexpr std::uint32_t kHitPoints = CombatRuleset::monsterHitPoints;
  constexpr std::uint32_t kDamage = CombatRuleset::attackDamage;
  const auto start =
      std::chrono::steady_clock::time_point{std::chrono::hours{1}};
  for (std::uint32_t index = 0; index < kHitPoints / kDamage; ++index) {
    const auto result = battle.attack(
        AttackCommand{
            .commandId = CommandId{.high = 0, .low = index + 1},
            .sessionId = SessionId{sessionId},
            .generation = SessionGeneration{1},
            .battleId = BattleInstanceId{battleId},
            .targetHint = CombatRuleset::monsterId,
        },
        start + index * 800ms);
    if (result.code != AttackResultCode::Ok) {
      std::abort();
    }
  }
}

// Move a participant along single axes from its actual authoritative position
// in fixed 250mm ticks until the exact grid target is reached. The starting
// position is read from `movementProjection()` because BattleInstance position
// persists between invocations; a test-local origin would diverge on every
// call after the first. `sequence` is shared across calls so action/server
// tick numbers stay strictly increasing for the whole battle.
void moveTo(BattleInstance &battle, std::uint64_t battleId,
            std::uint64_t sessionId, std::int32_t targetX, std::int32_t targetY,
            std::uint32_t &sequence) {
  const auto start =
      std::chrono::steady_clock::time_point{std::chrono::hours{1}};
  std::int32_t x = 0;
  std::int32_t y = 0;
  bool found = false;
  for (const auto &player : battle.movementProjection().players) {
    if (player.sessionId == SessionId{sessionId}) {
      x = player.posXMillimeter;
      y = player.posYMillimeter;
      found = true;
      break;
    }
  }
  if (!found) {
    std::abort();
  }
  const auto tick = [&](std::int16_t dirX, std::int16_t dirY) {
    ++sequence;
    if (battle.acceptMove(
            MoveCommand{
                .sessionId = SessionId{sessionId},
                .generation = SessionGeneration{1},
                .battleId = BattleInstanceId{battleId},
                .actionSequence = sequence,
                .direction = DirectionIntent{.desiredX = dirX,
                                             .desiredY = dirY,
                                             .inputFlags = 0},
            },
            start + sequence * 50ms) != MovementResultCode::Ok) {
      std::abort();
    }
    if (battle.integrateMovement(MovementTickCommand{
            .battleId = BattleInstanceId{battleId}, .serverTick = sequence}) !=
        MovementResultCode::Ok) {
      std::abort();
    }
    x += dirX * kTickMillimeters;
    y += dirY * kTickMillimeters;
  };
  while (x != targetX) {
    tick(static_cast<std::int16_t>(x < targetX ? 1 : -1),
         static_cast<std::int16_t>(0));
  }
  while (y != targetY) {
    tick(static_cast<std::int16_t>(0),
         static_cast<std::int16_t>(y < targetY ? 1 : -1));
  }
  // Zero-direction tick: neutralizes this participant's last movement delta
  // so it cannot leak into later integrates issued for another participant.
  tick(0, 0);
}

ClaimLootCommand claimCommand(std::uint64_t commandLow, std::uint64_t sessionId,
                              std::uint64_t generation, std::uint64_t battleId,
                              std::uint64_t dropId) {
  return ClaimLootCommand{
      .commandId = CommandId{.high = 0, .low = commandLow},
      .sessionId = SessionId{sessionId},
      .generation = SessionGeneration{generation},
      .battleId = BattleInstanceId{battleId},
      .dropId = DropId{dropId},
  };
}

ClaimLootTerminalResult claim(BattleInstance &battle, std::uint64_t battleId,
                              std::uint64_t sessionId, std::uint64_t commandLow,
                              std::uint64_t dropId,
                              std::uint64_t generation = 1) {
  return battle.claimLoot(
      claimCommand(commandLow, sessionId, generation, battleId, dropId),
      std::chrono::steady_clock::time_point{} +
          std::chrono::milliseconds{static_cast<std::int64_t>(commandLow) *
                                    125});
}

ClaimLootTerminalResult terminal(const ClaimLootCommand &command,
                                 ClaimLootResultCode code) {
  return ClaimLootTerminalResult{command.commandId, command.battleId,
                                 command.dropId, code};
}

const LootDropProjection *findDrop(const LootProjection &projection,
                                   std::uint64_t dropId) {
  for (const auto &drop : projection.drops) {
    if (drop.dropId == DropId{dropId}) {
      return &drop;
    }
  }
  return nullptr;
}

std::optional<std::uint64_t> holdingQuantity(const LootProjection &projection,
                                             std::uint64_t sessionId,
                                             std::uint64_t itemId) {
  for (const auto &holding : projection.holdings) {
    if (holding.sessionId == SessionId{sessionId} &&
        holding.itemId == ItemId{itemId}) {
      return holding.quantity;
    }
  }
  return std::nullopt;
}

ParticipantExitStatus exitStatusOf(const BattleLoadProjection &projection,
                                   std::uint64_t sessionId) {
  for (const auto &captured : projection.capturedParticipants) {
    if (captured.sessionId == SessionId{sessionId}) {
      return captured.exitStatus;
    }
  }
  std::abort();
}

CandidateDisconnectedCommand exitCommand(std::uint64_t sessionId,
                                         std::uint64_t battleId) {
  return CandidateDisconnectedCommand{
      .sessionId = SessionId{sessionId},
      .generation = SessionGeneration{1},
      .roomId = RoomId{7},
      .battleId = BattleInstanceId{battleId},
  };
}

// Battle 1 (room 7) two participants. Frozen generator golden for this battle:
// drop 1 Rare at (-3360, 8722), drop 2 Common at (-414, 819).
// From the origin, drop 2 is 842157mm^2 away (in range) and drop 1 is far.

bool validClaimSetsOneOwnerAndHoldingOnce() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);

  const auto before = battle.lootProjection();
  if (before.drops.size() != 2 || !before.holdings.empty()) {
    return false;
  }
  if (findDrop(before, 1) == nullptr || findDrop(before, 2) == nullptr) {
    return false;
  }

  const auto result = claim(battle, 1, 1, 1, 2);
  if (result.code != ClaimLootResultCode::Ok) {
    return false;
  }

  const auto after = battle.lootProjection();
  const auto *drop = findDrop(after, 2);
  const auto quantity = holdingQuantity(after, 1, 1);
  return drop != nullptr && drop->owner == SessionId{1} &&
         findDrop(after, 1)->owner == std::nullopt && quantity.has_value() &&
         *quantity == 1;
}

// Battle 728 (room 7) two participants. Frozen generator golden for this
// battle: drop 1 Rare at (-1882, -2553), drop 2 Common at (-1596, 5528).
// From grid (-3000, 5000) drop 2 is exactly 1500mm away (1404^2 + 528^2 ==
// 1500^2). From grid (-500, -3500) drop 1 is 2806733mm^2 (just past the
// boundary); from grid (-750, -3500) drop 1 is 2178233mm^2 (inside).

bool rangeRuleIsInclusiveAndOverflowSafe() {
  auto battle = openCommittedBattle(728, 2);
  killMonster(battle, 728, 1);
  std::uint32_t sequence = 100;

  // Extreme signed-coordinate guard: far corner deltas are converted to int64
  // and rejected on the axis check before any multiplication.
  moveTo(battle, 728, 1, -10000, -10000, sequence);
  if (claim(battle, 728, 1, 1, 2).code != ClaimLootResultCode::OutOfRange) {
    return false;
  }
  moveTo(battle, 728, 1, 0, 0, sequence);

  // Actual far drop: |dy| == 5528 exceeds the axis bound.
  if (claim(battle, 728, 1, 2, 2).code != ClaimLootResultCode::OutOfRange) {
    return false;
  }

  // Exact inclusive integer boundary: distance == 1500 exactly is accepted.
  moveTo(battle, 728, 1, -3000, 5000, sequence);
  if (claim(battle, 728, 1, 3, 2).code != ClaimLootResultCode::Ok) {
    return false;
  }

  // 1501-class circle rejection: both axes within bounds but the squared
  // distance (2806733) exceeds 1500^2. Drop stays available, holdings stay
  // unchanged.
  if (claim(battle, 728, 2, 4, 1).code != ClaimLootResultCode::OutOfRange) {
    return false;
  }
  moveTo(battle, 728, 2, -500, -3500, sequence);
  if (claim(battle, 728, 2, 5, 1).code != ClaimLootResultCode::OutOfRange) {
    return false;
  }
  moveTo(battle, 728, 2, -750, -3500, sequence);
  if (claim(battle, 728, 2, 6, 1).code != ClaimLootResultCode::Ok) {
    return false;
  }

  const auto after = battle.lootProjection();
  return findDrop(after, 1)->owner == SessionId{2} &&
         findDrop(after, 2)->owner == SessionId{1} &&
         holdingQuantity(after, 1, 1) == std::optional<std::uint64_t>{1} &&
         holdingQuantity(after, 2, 2) == std::optional<std::uint64_t>{1};
}

bool eligibilityRejectionsDoNotMutate() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);

  if (claim(battle, 1, 99, 1, 2).code != ClaimLootResultCode::NotEligible ||
      claim(battle, 1, 1, 2, 2, 2).code != ClaimLootResultCode::StaleSession ||
      claim(battle, 99, 1, 3, 2).code != ClaimLootResultCode::StaleBattle) {
    return false;
  }

  if (battle.disconnect(CandidateDisconnectedCommand{
          .sessionId = SessionId{2},
          .generation = SessionGeneration{1},
          .roomId = RoomId{7},
          .battleId = BattleInstanceId{1}}) != BattleLoadResultCode::Ok) {
    return false;
  }
  if (claim(battle, 1, 2, 4, 2).code != ClaimLootResultCode::NotEligible) {
    return false;
  }

  const auto after = battle.lootProjection();
  if (findDrop(after, 2)->owner.has_value() || !after.holdings.empty()) {
    return false;
  }
  // The non-GameplayEligible rejection is a retained current-scope terminal
  // result and replays unchanged.
  return claim(battle, 1, 2, 4, 2).code == ClaimLootResultCode::NotEligible;
}

bool dropIdentityInvalidUnknownAndFirstOwnerSticky() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);

  if (claim(battle, 1, 1, 1, 0).code != ClaimLootResultCode::InvalidDrop ||
      claim(battle, 1, 1, 2, 99).code != ClaimLootResultCode::UnknownDrop) {
    return false;
  }

  if (claim(battle, 1, 1, 3, 2).code != ClaimLootResultCode::Ok ||
      claim(battle, 1, 2, 4, 2).code != ClaimLootResultCode::AlreadyClaimed) {
    return false;
  }

  const auto after = battle.lootProjection();
  const auto quantity = holdingQuantity(after, 1, 1);
  return findDrop(after, 2)->owner == SessionId{1} &&
         findDrop(after, 1)->owner == std::nullopt && quantity.has_value() &&
         *quantity == 1;
}

bool exactlyOneOwnerForSharedDrop() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);

  if (claim(battle, 1, 1, 1, 2).code != ClaimLootResultCode::Ok ||
      claim(battle, 1, 2, 2, 2).code != ClaimLootResultCode::AlreadyClaimed) {
    return false;
  }

  const auto after = battle.lootProjection();
  const auto quantity = holdingQuantity(after, 1, 1);
  return findDrop(after, 2)->owner == SessionId{1} &&
         holdingQuantity(after, 2, 1) == std::nullopt && quantity.has_value() &&
         *quantity == 1;
}

bool sameCommandReplaysIdenticalTerminalResult() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);

  const auto first = claim(battle, 1, 1, 1, 2);
  const auto replay = claim(battle, 1, 1, 1, 2);
  if (first.code != ClaimLootResultCode::Ok || replay != first) {
    return false;
  }

  const auto after = battle.lootProjection();
  const auto quantity = holdingQuantity(after, 1, 1);
  return findDrop(after, 2)->owner == SessionId{1} && quantity.has_value() &&
         *quantity == 1;
}

bool commandConflictPreservesEverything() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);

  if (claim(battle, 1, 1, 1, 2).code != ClaimLootResultCode::Ok) {
    return false;
  }
  if (claim(battle, 1, 1, 1, 1).code != ClaimLootResultCode::CommandConflict) {
    return false;
  }

  const auto after = battle.lootProjection();
  const auto quantity = holdingQuantity(after, 1, 1);
  if (findDrop(after, 1)->owner.has_value() ||
      findDrop(after, 2)->owner != SessionId{1} || !quantity.has_value() ||
      *quantity != 1) {
    return false;
  }
  // The retained terminal result still replays for the original payload.
  return claim(battle, 1, 1, 1, 2) ==
         terminal(claimCommand(1, 1, 1, 1, 2), ClaimLootResultCode::Ok);
}

bool overloadedCommandReplaysAfterRateRefill() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);

  constexpr auto claimAt = std::chrono::steady_clock::time_point{} + 31s;
  for (std::uint64_t id = 1; id <= 4; ++id) {
    if (battle.claimLoot(claimCommand(id, 1, 1, 1, 100 + id), claimAt).code !=
        ClaimLootResultCode::UnknownDrop) {
      return false;
    }
  }

  const auto overloadedCommand = claimCommand(5, 1, 1, 1, 105);
  const auto overloaded = battle.claimLoot(overloadedCommand, claimAt);
  const auto beforeRetry = battle.lootProjection();
  if (overloaded.code != ClaimLootResultCode::Overloaded ||
      battle.claimLoot(overloadedCommand, claimAt + 125ms) != overloaded ||
      battle.claimLoot(claimCommand(5, 1, 1, 1, 999), claimAt + 125ms).code !=
          ClaimLootResultCode::CommandConflict ||
      battle.claimLoot(claimCommand(6, 1, 1, 1, 106), claimAt + 125ms).code !=
          ClaimLootResultCode::UnknownDrop) {
    return false;
  }
  return battle.lootProjection() == beforeRetry;
}

bool capacityRejectsBeforeAnyMutation() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);

  if (claim(battle, 1, 1, 1, 2).code != ClaimLootResultCode::Ok) {
    return false;
  }
  for (std::uint64_t id = 2; id <= LootResultStore::maximumResults; ++id) {
    if (claim(battle, 1, 1, id, 2).code !=
        ClaimLootResultCode::AlreadyClaimed) {
      return false;
    }
  }

  // A new command when the store is full is rejected before eligibility,
  // distance, drop, owner, or holding checks: the target drop stays unowned.
  if (claim(battle, 1, 1, LootResultStore::maximumResults + 1, 1).code !=
      ClaimLootResultCode::Overloaded) {
    return false;
  }

  const auto after = battle.lootProjection();
  const auto quantity = holdingQuantity(after, 1, 1);
  if (findDrop(after, 1)->owner.has_value() ||
      findDrop(after, 2)->owner != SessionId{1} || !quantity.has_value() ||
      *quantity != 1) {
    return false;
  }
  // Replay and conflict semantics survive the full store.
  return claim(battle, 1, 1, 1, 2).code == ClaimLootResultCode::Ok &&
         claim(battle, 1, 1, 1, 1).code == ClaimLootResultCode::CommandConflict;
}

bool scopeIsolationAcrossSessionGenerationBattle() {
  auto results =
      LootResultStore{SessionId{11}, SessionGeneration{2}, BattleInstanceId{7}};
  const auto original = claimCommand(1, 11, 2, 7, 3);
  if (results.inspect(original).decision !=
          LootResultStoreDecision::Available ||
      !results.retain(original, terminal(original, ClaimLootResultCode::Ok))) {
    return false;
  }
  const auto replay = results.inspect(original);
  const auto conflict = results.inspect(claimCommand(1, 11, 2, 7, 4));
  const auto otherSession = results.inspect(claimCommand(1, 12, 2, 7, 3));
  const auto otherGeneration = results.inspect(claimCommand(1, 11, 3, 7, 3));
  const auto otherBattle = results.inspect(claimCommand(1, 11, 2, 8, 3));
  if (replay.decision != LootResultStoreDecision::Replay ||
      replay.result != terminal(original, ClaimLootResultCode::Ok) ||
      conflict.decision != LootResultStoreDecision::Conflict ||
      otherSession.decision != LootResultStoreDecision::ScopeMismatch ||
      otherGeneration.decision != LootResultStoreDecision::ScopeMismatch ||
      otherBattle.decision != LootResultStoreDecision::ScopeMismatch) {
    return false;
  }
  const auto wrongScope = claimCommand(2, 12, 2, 7, 3);
  if (results.retain(wrongScope,
                     terminal(wrongScope, ClaimLootResultCode::Ok)) ||
      results.size() != 1) {
    return false;
  }

  // Battle-level: the same CommandId under a different session scope is a
  // fresh command, never a replay of another scope's result.
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);
  if (claim(battle, 1, 1, 1, 2).code != ClaimLootResultCode::Ok ||
      claim(battle, 1, 2, 1, 2).code != ClaimLootResultCode::AlreadyClaimed) {
    return false;
  }
  const auto after = battle.lootProjection();
  return findDrop(after, 2)->owner == SessionId{1};
}

bool catalogIntegrityResolvesEveryClaimedItem() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);

  // Drop 1 (Rare) at (-3360, 8722): grid (-3250, 8750) is 12884mm^2 away.
  std::uint32_t sequence = 0;
  moveTo(battle, 1, 1, -3250, 8750, sequence);
  if (claim(battle, 1, 1, 1, 1).code != ClaimLootResultCode::Ok) {
    return false;
  }
  // Drop 2 (Common) is in range from the origin.
  if (claim(battle, 1, 2, 2, 2).code != ClaimLootResultCode::Ok) {
    return false;
  }

  const auto catalog = RelicCatalog::v1Snapshot();
  const auto after = battle.lootProjection();
  const auto rare = holdingQuantity(after, 1, 2);
  const auto common = holdingQuantity(after, 2, 1);
  return findDrop(after, 1)->owner == SessionId{1} &&
         findDrop(after, 2)->owner == SessionId{2} && rare.has_value() &&
         *rare == 1 && common.has_value() && *common == 1 &&
         catalog.unitValueOf(ItemId{2}) == std::optional<std::uint64_t>{300} &&
         catalog.unitValueOf(ItemId{1}) == std::optional<std::uint64_t>{100};
}

bool resultStoreReplayConflictScopeAndCapacity() {
  auto results =
      LootResultStore{SessionId{11}, SessionGeneration{2}, BattleInstanceId{7}};
  for (std::uint64_t id = 1; id <= LootResultStore::maximumResults; ++id) {
    const auto command = claimCommand(id, 11, 2, 7, 3);
    if (results.inspect(command).decision !=
            LootResultStoreDecision::Available ||
        !results.retain(command, terminal(command, ClaimLootResultCode::Ok))) {
      return false;
    }
  }
  const auto overflow =
      claimCommand(LootResultStore::maximumResults + 1, 11, 2, 7, 4);
  return results.size() == LootResultStore::maximumResults &&
         results.inspect(overflow).decision ==
             LootResultStoreDecision::Overloaded &&
         !results.retain(overflow,
                         terminal(overflow, ClaimLootResultCode::Ok)) &&
         results.inspect(claimCommand(1, 11, 2, 7, 3)).decision ==
             LootResultStoreDecision::Replay &&
         results.inspect(claimCommand(1, 11, 2, 7, 4)).decision ==
             LootResultStoreDecision::Conflict;
}

bool resultStoreRetentionShapeMatchesAttackStore() {
  auto results =
      LootResultStore{SessionId{11}, SessionGeneration{2}, BattleInstanceId{7}};
  const auto first = claimCommand(1, 11, 2, 7, 3);
  const auto second = claimCommand(2, 11, 2, 7, 4);
  if (!results.retain(first, terminal(first, ClaimLootResultCode::Ok)) ||
      !results.retain(second,
                      terminal(second, ClaimLootResultCode::AlreadyClaimed)) ||
      results.evictExpired(LootResultStore::Clock::time_point{} + 1h) != 0) {
    return false;
  }
  const auto completedAt = LootResultStore::Clock::time_point{} + 2h;
  results.markBattleCompleted(completedAt);
  return results.evictExpired(completedAt + 29999ms) == 0 &&
         results.size() == 2 &&
         results.evictExpired(completedAt + 30000ms) == 2 &&
         results.size() == 0;
}

// Battle 1 (room 7) two participants: drop 1 Rare at (-3360, 8722), drop 2
// Common at (-414, 819). Drop 2 is in claim range from the origin; drop 1 is
// in range from grid (-3250, 8750).

bool claimThenDeadlineClosesRemainingDrop() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);

  if (claim(battle, 1, 1, 1, 2).code != ClaimLootResultCode::Ok) {
    return false;
  }
  const auto afterClaim = battle.lootProjection();
  if (afterClaim.resolution != LootResolutionState::Open ||
      findDrop(afterClaim, 2)->state != LootDropState::Claimed ||
      findDrop(afterClaim, 2)->owner != SessionId{1} ||
      findDrop(afterClaim, 1)->state != LootDropState::Available) {
    return false;
  }

  if (battle.expireLoot(LootDeadlineCommand{BattleInstanceId{1}}) !=
      LootDeadlineResultCode::Ok) {
    return false;
  }
  const auto after = battle.lootProjection();
  const auto quantity = holdingQuantity(after, 1, 1);
  return after.resolution == LootResolutionState::Resolved &&
         findDrop(after, 2)->state == LootDropState::Claimed &&
         findDrop(after, 2)->owner == SessionId{1} &&
         findDrop(after, 1)->state == LootDropState::Unclaimed &&
         findDrop(after, 1)->owner == std::nullopt && quantity.has_value() &&
         *quantity == 1;
}

bool deadlineThenFreshClaimIsResolutionClosed() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);

  if (battle.expireLoot(LootDeadlineCommand{BattleInstanceId{1}}) !=
      LootDeadlineResultCode::Ok) {
    return false;
  }
  const auto afterDeadline = battle.lootProjection();
  if (afterDeadline.resolution != LootResolutionState::Resolved ||
      findDrop(afterDeadline, 1)->state != LootDropState::Unclaimed ||
      findDrop(afterDeadline, 2)->state != LootDropState::Unclaimed) {
    return false;
  }

  const auto fresh = claim(battle, 1, 1, 1, 2);
  if (fresh.code != ClaimLootResultCode::ResolutionClosed) {
    return false;
  }
  const auto after = battle.lootProjection();
  return after.resolution == LootResolutionState::Resolved &&
         findDrop(after, 2)->state == LootDropState::Unclaimed &&
         findDrop(after, 2)->owner == std::nullopt && after.holdings.empty();
}

bool earlyResolvedThenDeadlineIsNoMutation() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);
  std::uint32_t sequence = 0;

  // Claim every Drop: drop 2 from the origin, drop 1 after moving into range.
  if (claim(battle, 1, 1, 1, 2).code != ClaimLootResultCode::Ok) {
    return false;
  }
  moveTo(battle, 1, 1, -3250, 8750, sequence);
  if (claim(battle, 1, 1, 2, 1).code != ClaimLootResultCode::Ok) {
    return false;
  }
  const auto closed = battle.lootProjection();
  if (closed.resolution != LootResolutionState::Resolved ||
      findDrop(closed, 1)->state != LootDropState::Claimed ||
      findDrop(closed, 2)->state != LootDropState::Claimed) {
    return false;
  }

  const auto before = battle.lootProjection();
  return battle.expireLoot(LootDeadlineCommand{BattleInstanceId{1}}) ==
             LootDeadlineResultCode::ResolutionClosed &&
         battle.lootProjection() == before;
}

bool staleBattleDeadlineIsNoMutation() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);

  const auto before = battle.lootProjection();
  return battle.expireLoot(LootDeadlineCommand{BattleInstanceId{2}}) ==
             LootDeadlineResultCode::StaleBattle &&
         battle.lootProjection() == before &&
         before.resolution == LootResolutionState::Open &&
         findDrop(before, 1)->state == LootDropState::Available &&
         findDrop(before, 2)->state == LootDropState::Available;
}

bool claimThenVoluntaryLeftKeepsHoldingAndExitStatus() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);

  // The claimant itself voluntarily leaves; the other participant stays
  // captured. The acquired owner/holding must survive the claimant's exit.
  if (claim(battle, 1, 1, 1, 2).code != ClaimLootResultCode::Ok ||
      battle.leave(exitCommand(1, 1)) != BattleLoadResultCode::Ok) {
    return false;
  }

  const auto projection = battle.projection();
  const auto after = battle.lootProjection();
  if (exitStatusOf(projection, 1) != ParticipantExitStatus::VoluntaryLeft ||
      exitStatusOf(projection, 2) != ParticipantExitStatus::GameplayEligible ||
      findDrop(after, 2)->owner != SessionId{1} ||
      holdingQuantity(after, 1, 1) != std::optional<std::uint64_t>{1}) {
    return false;
  }

  // A fresh claim from the exited claimant is rejected without mutation.
  if (claim(battle, 1, 1, 2, 2).code != ClaimLootResultCode::NotEligible) {
    return false;
  }
  // The original accepted ClaimLoot still replays identically.
  return claim(battle, 1, 1, 1, 2) ==
         terminal(claimCommand(1, 1, 1, 1, 2), ClaimLootResultCode::Ok);
}

bool claimThenDisconnectedKeepsHoldingAndExitStatus() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);

  // The claimant itself disconnects; the other participant stays captured.
  // The acquired owner/holding must survive the claimant's exit.
  if (claim(battle, 1, 1, 1, 2).code != ClaimLootResultCode::Ok ||
      battle.disconnect(exitCommand(1, 1)) != BattleLoadResultCode::Ok) {
    return false;
  }

  const auto projection = battle.projection();
  const auto after = battle.lootProjection();
  if (exitStatusOf(projection, 1) != ParticipantExitStatus::Disconnected ||
      exitStatusOf(projection, 2) != ParticipantExitStatus::GameplayEligible ||
      findDrop(after, 2)->owner != SessionId{1} ||
      holdingQuantity(after, 1, 1) != std::optional<std::uint64_t>{1}) {
    return false;
  }

  if (claim(battle, 1, 1, 2, 2).code != ClaimLootResultCode::NotEligible) {
    return false;
  }
  return claim(battle, 1, 1, 1, 2) ==
         terminal(claimCommand(1, 1, 1, 1, 2), ClaimLootResultCode::Ok);
}

bool voluntaryLeftThenClaimRejectsWithoutMutation() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);

  if (battle.leave(exitCommand(2, 1)) != BattleLoadResultCode::Ok ||
      claim(battle, 1, 2, 1, 2).code != ClaimLootResultCode::NotEligible) {
    return false;
  }
  const auto after = battle.lootProjection();
  return exitStatusOf(battle.projection(), 2) ==
             ParticipantExitStatus::VoluntaryLeft &&
         after.resolution == LootResolutionState::Open &&
         findDrop(after, 1)->state == LootDropState::Available &&
         findDrop(after, 2)->state == LootDropState::Available &&
         findDrop(after, 2)->owner == std::nullopt && after.holdings.empty();
}

bool disconnectedThenClaimRejectsWithoutMutation() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);

  if (battle.disconnect(exitCommand(2, 1)) != BattleLoadResultCode::Ok ||
      claim(battle, 1, 2, 1, 2).code != ClaimLootResultCode::NotEligible) {
    return false;
  }
  const auto after = battle.lootProjection();
  return exitStatusOf(battle.projection(), 2) ==
             ParticipantExitStatus::Disconnected &&
         after.resolution == LootResolutionState::Open &&
         findDrop(after, 1)->state == LootDropState::Available &&
         findDrop(after, 2)->state == LootDropState::Available &&
         findDrop(after, 2)->owner == std::nullopt && after.holdings.empty();
}

bool firstExitReasonIsNotOverwritten() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);

  // Disconnect first: a later voluntary leave is a no-mutation notification.
  if (battle.disconnect(exitCommand(2, 1)) != BattleLoadResultCode::Ok ||
      battle.leave(exitCommand(2, 1)) != BattleLoadResultCode::Ok ||
      exitStatusOf(battle.projection(), 2) !=
          ParticipantExitStatus::Disconnected) {
    return false;
  }

  auto other = openCommittedBattle(1, 2);
  killMonster(other, 1, 1);
  // Leave first: a later disconnect cannot overwrite the exit reason.
  return other.leave(exitCommand(2, 1)) == BattleLoadResultCode::Ok &&
         other.disconnect(exitCommand(2, 1)) == BattleLoadResultCode::Ok &&
         exitStatusOf(other.projection(), 2) ==
             ParticipantExitStatus::VoluntaryLeft;
}

} // namespace

int main() {
  if (!validClaimSetsOneOwnerAndHoldingOnce() ||
      !rangeRuleIsInclusiveAndOverflowSafe() ||
      !eligibilityRejectionsDoNotMutate() ||
      !dropIdentityInvalidUnknownAndFirstOwnerSticky() ||
      !exactlyOneOwnerForSharedDrop() ||
      !sameCommandReplaysIdenticalTerminalResult() ||
      !commandConflictPreservesEverything() ||
      !overloadedCommandReplaysAfterRateRefill() ||
      !capacityRejectsBeforeAnyMutation() ||
      !scopeIsolationAcrossSessionGenerationBattle() ||
      !catalogIntegrityResolvesEveryClaimedItem() ||
      !resultStoreReplayConflictScopeAndCapacity() ||
      !resultStoreRetentionShapeMatchesAttackStore() ||
      !claimThenDeadlineClosesRemainingDrop() ||
      !deadlineThenFreshClaimIsResolutionClosed() ||
      !earlyResolvedThenDeadlineIsNoMutation() ||
      !staleBattleDeadlineIsNoMutation() ||
      !claimThenVoluntaryLeftKeepsHoldingAndExitStatus() ||
      !claimThenDisconnectedKeepsHoldingAndExitStatus() ||
      !voluntaryLeftThenClaimRejectsWithoutMutation() ||
      !disconnectedThenClaimRejectsWithoutMutation() ||
      !firstExitReasonIsNotOverwritten()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
