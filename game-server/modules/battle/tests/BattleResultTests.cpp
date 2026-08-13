#include <lol/battle/BattleLoadApi.hpp>
#include <lol/battle/BattleResult.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using lol::battle::ArenaLoadCompleteCommand;
using lol::battle::AttackCommand;
using lol::battle::AttackResultCode;
using lol::battle::BattleAdmissionSnapshot;
using lol::battle::BattleFinalResult;
using lol::battle::BattleInstance;
using lol::battle::BattleLoadProjection;
using lol::battle::BattleLoadResultCode;
using lol::battle::BattleLoadState;
using lol::battle::BattleOutcome;
using lol::battle::BattleResultProjection;
using lol::battle::BattleResultState;
using lol::battle::BattleStartCandidate;
using lol::battle::buildFinalResult;
using lol::battle::CandidateDisconnectedCommand;
using lol::battle::CapturedParticipant;
using lol::battle::ClaimLootCommand;
using lol::battle::ClaimLootResultCode;
using lol::battle::ClaimLootTerminalResult;
using lol::battle::CombatDeadlineCommand;
using lol::battle::CombatDeadlineResultCode;
using lol::battle::CombatOutcome;
using lol::battle::CombatRuleset;
using lol::battle::CommandId;
using lol::battle::DirectionIntent;
using lol::battle::DropId;
using lol::battle::DropPosition;
using lol::battle::ItemId;
using lol::battle::LootDeadlineCommand;
using lol::battle::LootDeadlineResultCode;
using lol::battle::LootDropProjection;
using lol::battle::LootDropState;
using lol::battle::LootHoldingProjection;
using lol::battle::LootResolutionState;
using lol::battle::MoveCommand;
using lol::battle::MovementResultCode;
using lol::battle::MovementTickCommand;
using lol::battle::ParticipantExitStatus;
using lol::battle::RelicCatalog;
using lol::battle::ResultBuildSource;
using lol::battle::ResultBuildStatus;
using lol::shared::AccountId;
using lol::shared::BattleInstanceId;
using lol::shared::RoomId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;

// Required case 1: exact enum model. CombatOutcome stays 0/1/2 (None,
// MonsterDefeated, CombatTimeout) and BattleOutcome is exactly 1/2/3
// (MonsterDefeated, CombatTimeout, CancelledNoActiveParticipants). The Battle
// final result and its build source carry the Battle outcome, never the
// combat-only outcome.
static_assert(static_cast<int>(CombatOutcome::None) == 0);
static_assert(static_cast<int>(BattleOutcome::MonsterDefeated) == 1);
static_assert(static_cast<int>(BattleOutcome::CombatTimeout) == 2);
static_assert(static_cast<int>(CombatOutcome::MonsterDefeated) == 1);
static_assert(static_cast<int>(CombatOutcome::CombatTimeout) == 2);
static_assert(static_cast<int>(BattleOutcome::CancelledNoActiveParticipants) ==
              3);
static_assert(std::is_same_v<decltype(ResultBuildSource{
                                 RoomId{1},
                                 BattleInstanceId{1},
                                 BattleOutcome::MonsterDefeated,
                                 {},
                                 {},
                                 {}}.outcome),
                             BattleOutcome>);
static_assert(std::is_same_v<decltype(BattleFinalResult{
                                 RoomId{1},
                                 BattleInstanceId{1},
                                 BattleOutcome::MonsterDefeated,
                                 {}}.outcome),
                             BattleOutcome>);

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

CandidateDisconnectedCommand exitCommand(std::uint64_t sessionId,
                                         std::uint64_t battleId) {
  return CandidateDisconnectedCommand{
      .sessionId = SessionId{sessionId},
      .generation = SessionGeneration{1},
      .roomId = RoomId{7},
      .battleId = BattleInstanceId{battleId},
  };
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

CapturedParticipant participant(std::uint64_t session, std::string nickname,
                                ParticipantExitStatus status) {
  return CapturedParticipant{
      .accountId = account(static_cast<std::uint8_t>(session)),
      .sessionId = SessionId{session},
      .generation = SessionGeneration{1},
      .nickname = std::move(nickname),
      .exitStatus = status,
  };
}

ResultBuildSource sourceWith(BattleOutcome outcome,
                             std::vector<CapturedParticipant> captured) {
  return ResultBuildSource{
      .roomId = RoomId{7},
      .battleId = BattleInstanceId{1},
      .outcome = outcome,
      .captured = std::move(captured),
      .drops = {},
      .holdings = {},
  };
}

LootDropProjection dropProjection(std::uint64_t dropId, std::uint64_t itemId,
                                  std::uint64_t quantity) {
  return LootDropProjection{
      .dropId = DropId{dropId},
      .itemId = ItemId{itemId},
      .quantity = quantity,
      .position = DropPosition{0, 0},
      .state = LootDropState::Unclaimed,
      .owner = std::nullopt,
  };
}

// Flexible Drop projection for explicit economic state: quantity, state, and
// owner (nullopt means no owner).
LootDropProjection drop(std::uint64_t dropId, std::uint64_t itemId,
                        std::uint64_t quantity, LootDropState state,
                        std::optional<std::uint64_t> owner) {
  return LootDropProjection{
      .dropId = DropId{dropId},
      .itemId = ItemId{itemId},
      .quantity = quantity,
      .position = DropPosition{0, 0},
      .state = state,
      .owner = owner.has_value() ? std::optional<SessionId>{SessionId{*owner}}
                                 : std::nullopt,
  };
}

LootHoldingProjection holdingProjection(std::uint64_t sessionId,
                                        std::uint64_t itemId,
                                        std::uint64_t quantity) {
  return LootHoldingProjection{
      .sessionId = SessionId{sessionId},
      .itemId = ItemId{itemId},
      .quantity = quantity,
  };
}

// Required case 1: N=2 MonsterDefeated source with a rare holder (300) and a
// common holder (100). Ranks 1 and 2, exactly one top. The Rare Drop is Claimed
// by the 300 holder and the Common Drop by the 100 holder, so holdings equal
// the claimed Drop aggregation.
bool monsterDefeatedRanksThreeHundredAndOneHundred() {
  auto source = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  source.drops = {drop(1, 2, 1, LootDropState::Claimed, 1),
                  drop(2, 1, 1, LootDropState::Claimed, 2)};
  source.holdings = {holdingProjection(1, 2, 1), holdingProjection(2, 1, 1)};
  const auto built = buildFinalResult(source, RelicCatalog::v1Snapshot());
  if (built.status != ResultBuildStatus::Built || !built.result.has_value()) {
    return false;
  }
  const auto &result = *built.result;
  const auto &entries = result.entries;
  std::size_t topCount = 0;
  for (const auto &entry : entries) {
    if (entry.isTop) {
      ++topCount;
    }
  }
  return result.outcome == BattleOutcome::MonsterDefeated &&
         result.roomId == RoomId{7} && result.battleId == BattleInstanceId{1} &&
         entries.size() == 2 && topCount == 1 &&
         entries[0].sessionId == SessionId{1} &&
         entries[0].finalAssetValue == 300 &&
         entries[0].rank == std::optional<std::uint32_t>{1} &&
         entries[0].isTop && entries[1].sessionId == SessionId{2} &&
         entries[1].finalAssetValue == 100 &&
         entries[1].rank == std::optional<std::uint32_t>{2} &&
         !entries[1].isTop;
}

// Required case 2: valid N=10 source proving competition rank 1,1,3. Exactly
// one Rare Drop is Claimed by Session 2, exactly three Common Drops are
// Claimed by Session 9, and the remaining six Common Drops are distributed one
// per Session (1,3,4,5,6,7) so every next lower participant has value below
// 300. Holdings equal the claimed Drop aggregation. Session 2 then Session 9
// tie at rank 1; the next participant gets rank 3.
bool tenParticipantTieYieldsOneOneThree() {
  std::vector<CapturedParticipant> captured;
  for (std::uint64_t session = 1; session <= 10; ++session) {
    captured.push_back(participant(session, "p" + std::to_string(session),
                                   ParticipantExitStatus::GameplayEligible));
  }
  auto source = sourceWith(BattleOutcome::MonsterDefeated, std::move(captured));
  source.drops.push_back(drop(1, 2, 1, LootDropState::Claimed, 2));
  for (std::uint64_t dropId = 2; dropId <= 4; ++dropId) {
    source.drops.push_back(drop(dropId, 1, 1, LootDropState::Claimed, 9));
  }
  const std::uint64_t commonSessions[] = {1, 3, 4, 5, 6, 7};
  std::uint64_t dropId = 5;
  for (const auto commonSession : commonSessions) {
    source.drops.push_back(
        drop(dropId++, 1, 1, LootDropState::Claimed, commonSession));
  }
  source.holdings.push_back(holdingProjection(2, 2, 1));
  source.holdings.push_back(holdingProjection(9, 1, 3));
  for (const auto commonSession : commonSessions) {
    source.holdings.push_back(holdingProjection(commonSession, 1, 1));
  }
  const auto built = buildFinalResult(source, RelicCatalog::v1Snapshot());
  if (built.status != ResultBuildStatus::Built || !built.result.has_value()) {
    return false;
  }
  const auto &entries = built.result->entries;
  if (entries.size() != 10) {
    return false;
  }
  if (entries[0].sessionId != SessionId{2} ||
      entries[0].finalAssetValue != 300 ||
      entries[0].rank != std::optional<std::uint32_t>{1} || !entries[0].isTop ||
      entries[1].sessionId != SessionId{9} ||
      entries[1].finalAssetValue != 300 ||
      entries[1].rank != std::optional<std::uint32_t>{1} || !entries[1].isTop) {
    return false;
  }
  // The next lower participant has value below 300 and gets rank 3.
  if (entries[2].sessionId != SessionId{1} ||
      entries[2].finalAssetValue != 100 ||
      entries[2].rank != std::optional<std::uint32_t>{3} || entries[2].isTop) {
    return false;
  }
  for (std::size_t index = 3; index + 1 < entries.size(); ++index) {
    if (!(entries[index].sessionId < entries[index + 1].sessionId)) {
      return false;
    }
  }
  for (std::size_t index = 3; index <= 7; ++index) {
    if (entries[index].finalAssetValue != 100 ||
        entries[index].rank != std::optional<std::uint32_t>{3} ||
        entries[index].isTop) {
      return false;
    }
  }
  for (std::size_t index = 8; index < entries.size(); ++index) {
    if (entries[index].finalAssetValue != 0 ||
        entries[index].rank != std::optional<std::uint32_t>{9} ||
        entries[index].isTop) {
      return false;
    }
  }
  return true;
}

// Required case 3: equal-value inputs presented in different captured/drop/
// holding permutations produce an exactly equal ordered result. N=4: one
// participant owns the Rare Drop and another owns all three Common Drops;
// holdings equal the claimed Drop aggregation.
bool equalValuePermutationsProduceEqualOrderedResult() {
  auto first = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible),
       participant(3, "p3", ParticipantExitStatus::GameplayEligible),
       participant(4, "p4", ParticipantExitStatus::GameplayEligible)});
  first.drops = {drop(1, 2, 1, LootDropState::Claimed, 1),
                 drop(2, 1, 1, LootDropState::Claimed, 2),
                 drop(3, 1, 1, LootDropState::Claimed, 2),
                 drop(4, 1, 1, LootDropState::Claimed, 2)};
  first.holdings = {holdingProjection(1, 2, 1), holdingProjection(2, 1, 3)};

  auto second = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(4, "p4", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible),
       participant(3, "p3", ParticipantExitStatus::GameplayEligible),
       participant(1, "p1", ParticipantExitStatus::GameplayEligible)});
  second.drops = {drop(4, 1, 1, LootDropState::Claimed, 2),
                  drop(2, 1, 1, LootDropState::Claimed, 2),
                  drop(1, 2, 1, LootDropState::Claimed, 1),
                  drop(3, 1, 1, LootDropState::Claimed, 2)};
  second.holdings = {holdingProjection(2, 1, 3), holdingProjection(1, 2, 1)};

  const auto firstBuilt = buildFinalResult(first, RelicCatalog::v1Snapshot());
  const auto secondBuilt = buildFinalResult(second, RelicCatalog::v1Snapshot());
  if (firstBuilt.status != ResultBuildStatus::Built ||
      secondBuilt.status != ResultBuildStatus::Built ||
      firstBuilt.result != secondBuilt.result ||
      !firstBuilt.result.has_value()) {
    return false;
  }
  const auto &entries = firstBuilt.result->entries;
  return entries.size() == 4 && entries[0].sessionId == SessionId{1} &&
         entries[0].finalAssetValue == 300 &&
         entries[0].rank == std::optional<std::uint32_t>{1} &&
         entries[0].isTop && entries[1].sessionId == SessionId{2} &&
         entries[1].finalAssetValue == 300 &&
         entries[1].rank == std::optional<std::uint32_t>{1} &&
         entries[1].isTop && entries[2].sessionId == SessionId{3} &&
         entries[2].finalAssetValue == 0 &&
         entries[2].rank == std::optional<std::uint32_t>{3} &&
         !entries[2].isTop && entries[3].sessionId == SessionId{4} &&
         entries[3].finalAssetValue == 0 &&
         entries[3].rank == std::optional<std::uint32_t>{3} &&
         !entries[3].isTop;
}

// Required case 4: an exited participant remains a ranking entry, frozen
// holdings count, and the entry carries the terminal exit status. Drop state
// and owner are consistent with the holding: the Rare Drop is Claimed by the
// exited 300 holder, the Common Drop stays Unclaimed with no holding.
bool exitedParticipantKeepsEntryWithFrozenHoldings() {
  auto source = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::VoluntaryLeft),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  source.drops = {drop(1, 2, 1, LootDropState::Claimed, 1),
                  drop(2, 1, 1, LootDropState::Unclaimed, std::nullopt)};
  source.holdings = {holdingProjection(1, 2, 1)};
  const auto built = buildFinalResult(source, RelicCatalog::v1Snapshot());
  if (built.status != ResultBuildStatus::Built || !built.result.has_value()) {
    return false;
  }
  const auto &entries = built.result->entries;
  return entries.size() == 2 && entries[0].sessionId == SessionId{1} &&
         entries[0].finalAssetValue == 300 &&
         entries[0].exitStatus == ParticipantExitStatus::TerminalExited &&
         entries[0].rank == std::optional<std::uint32_t>{1} &&
         entries[0].isTop && entries[1].sessionId == SessionId{2} &&
         entries[1].finalAssetValue == 0 &&
         entries[1].exitStatus == ParticipantExitStatus::TerminalPresent &&
         entries[1].rank == std::optional<std::uint32_t>{2} &&
         !entries[1].isTop;
}

// Required case 5: CombatTimeout builds every captured entry with value 0,
// null rank and top false, ordered by SessionId ascending, with no winner.
bool combatTimeoutBuildsAllEntriesNoRankNoWinner() {
  auto source = sourceWith(
      BattleOutcome::CombatTimeout,
      {participant(3, "p3", ParticipantExitStatus::GameplayEligible),
       participant(1, "p1", ParticipantExitStatus::Disconnected),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  const auto built = buildFinalResult(source, RelicCatalog::v1Snapshot());
  if (built.status != ResultBuildStatus::Built || !built.result.has_value()) {
    return false;
  }
  const auto &result = *built.result;
  if (result.outcome != BattleOutcome::CombatTimeout ||
      result.roomId != RoomId{7} || result.battleId != BattleInstanceId{1} ||
      result.entries.size() != 3) {
    return false;
  }
  for (std::size_t index = 0; index < result.entries.size(); ++index) {
    const auto &entry = result.entries[index];
    if (entry.sessionId != SessionId{index + 1} || entry.finalAssetValue != 0 ||
        entry.rank.has_value() || entry.isTop) {
      return false;
    }
  }
  // Session 1 was Disconnected (TerminalExited); sessions 2 and 3 were
  // GameplayEligible (TerminalPresent).
  return result.entries[0].exitStatus ==
             ParticipantExitStatus::TerminalExited &&
         result.entries[1].exitStatus ==
             ParticipantExitStatus::TerminalPresent &&
         result.entries[2].exitStatus == ParticipantExitStatus::TerminalPresent;
}

// CombatTimeout requires no drops and no holdings; otherwise generation fails
// without any result.
bool combatTimeoutRejectsDropsOrHoldings() {
  auto withDrops = sourceWith(
      BattleOutcome::CombatTimeout,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  withDrops.drops = {dropProjection(1, 1, 1)};
  auto withHoldings = sourceWith(
      BattleOutcome::CombatTimeout,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  withHoldings.holdings = {holdingProjection(1, 1, 1)};
  const auto dropsBuilt =
      buildFinalResult(withDrops, RelicCatalog::v1Snapshot());
  const auto holdingsBuilt =
      buildFinalResult(withHoldings, RelicCatalog::v1Snapshot());
  return dropsBuilt.status != ResultBuildStatus::Built &&
         !dropsBuilt.result.has_value() &&
         holdingsBuilt.status != ResultBuildStatus::Built &&
         !holdingsBuilt.result.has_value();
}

// A Battle outcome that is none of the three valid values is an internal
// invariant failure with no result.
bool unknownOutcomeIsInvariantFailure() {
  auto source = sourceWith(
      // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
      static_cast<BattleOutcome>(0),
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  const auto built = buildFinalResult(source, RelicCatalog::v1Snapshot());
  return built.status == ResultBuildStatus::InvariantBroken &&
         !built.result.has_value();
}

// Required case 6: a missing generated Drop item and a missing holding item
// each produce a generation failure with no optional result.
bool missingDropAndHoldingItemsFailGeneration() {
  auto missingDrop = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  missingDrop.drops = {dropProjection(1, 99, 1), dropProjection(2, 1, 1)};
  missingDrop.holdings = {holdingProjection(1, 1, 1)};
  const auto dropBuilt =
      buildFinalResult(missingDrop, RelicCatalog::v1Snapshot());
  if (dropBuilt.status != ResultBuildStatus::CatalogIncomplete ||
      dropBuilt.result.has_value()) {
    return false;
  }

  auto missingHolding = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  missingHolding.drops = {dropProjection(1, 1, 1), dropProjection(2, 1, 1)};
  missingHolding.holdings = {holdingProjection(1, 99, 1)};
  const auto holdingBuilt =
      buildFinalResult(missingHolding, RelicCatalog::v1Snapshot());
  return holdingBuilt.status == ResultBuildStatus::CatalogIncomplete &&
         !holdingBuilt.result.has_value();
}

// Required case 7: multiplication overflow and per-participant addition
// overflow each fail generation with no partial result or rank. Both vectors
// are economically valid in every other respect; only the arithmetic guard
// fails, before an economic-conservation mismatch could mask it.
bool arithmeticOverflowFailsWithoutPartialResult() {
  auto multiplication = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  multiplication.drops = {drop(1, 2, 1, LootDropState::Claimed, 1),
                          drop(2, 1, 1, LootDropState::Claimed, 2)};
  multiplication.holdings = {holdingProjection(1, 2, UINT64_C(1) << 62)};
  const auto multiplied =
      buildFinalResult(multiplication, RelicCatalog::v1Snapshot());
  if (multiplied.status != ResultBuildStatus::ArithmeticOverflow ||
      multiplied.result.has_value()) {
    return false;
  }

  auto addition = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  addition.drops = {drop(1, 1, 1, LootDropState::Claimed, 1),
                    drop(2, 1, 1, LootDropState::Claimed, 2)};
  constexpr std::uint64_t kHugeQuantity = UINT64_C(93000000000000000);
  addition.holdings = {holdingProjection(1, 1, kHugeQuantity),
                       holdingProjection(1, 1, kHugeQuantity)};
  const auto summed = buildFinalResult(addition, RelicCatalog::v1Snapshot());
  return summed.status == ResultBuildStatus::ArithmeticOverflow &&
         !summed.result.has_value();
}

// A holding that belongs to no captured participant and a zero-quantity
// holding are inconsistent sources: invariant failure with no result. The Drop
// sources are economically valid so the holding defect is what fails.
bool foreignHoldingAndZeroQuantityAreInvariantFailures() {
  auto foreign = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  foreign.drops = {drop(1, 1, 1, LootDropState::Claimed, 1),
                   drop(2, 1, 1, LootDropState::Claimed, 2)};
  foreign.holdings = {holdingProjection(99, 1, 1)};
  const auto foreignBuilt =
      buildFinalResult(foreign, RelicCatalog::v1Snapshot());

  auto zero = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  zero.drops = {drop(1, 1, 1, LootDropState::Claimed, 1),
                drop(2, 1, 1, LootDropState::Claimed, 2)};
  zero.holdings = {holdingProjection(1, 1, 0)};
  const auto zeroBuilt = buildFinalResult(zero, RelicCatalog::v1Snapshot());
  return foreignBuilt.status == ResultBuildStatus::InvariantBroken &&
         !foreignBuilt.result.has_value() &&
         zeroBuilt.status == ResultBuildStatus::InvariantBroken &&
         !zeroBuilt.result.has_value();
}

// Required case 8: MonsterDefeated while loot is still Open remains NotReady
// with no result value.
bool monsterDefeatedWhileLootOpenRemainsNotReady() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);
  if (battle.lootProjection().resolution != LootResolutionState::Open) {
    return false;
  }
  const auto projection = battle.resultProjection();
  return projection.state == BattleResultState::NotReady &&
         !projection.result.has_value();
}

// Required case 9: after claim-all and after the loot deadline, BattleInstance
// commits the expected result exactly once. A stale repeated deadline, a
// replayed claim, and a fresh closed-window claim cannot rebuild or mutate.
bool claimAllAndDeadlineCommitExactlyOnce() {
  auto claimAll = openCommittedBattle(1, 2);
  killMonster(claimAll, 1, 1);
  if (claim(claimAll, 1, 1, 1, 2).code != ClaimLootResultCode::Ok) {
    return false;
  }
  std::uint32_t sequence = 0;
  moveTo(claimAll, 1, 2, -3250, 8750, sequence);
  if (claim(claimAll, 1, 2, 2, 1).code != ClaimLootResultCode::Ok) {
    return false;
  }
  const auto committed = claimAll.resultProjection();
  if (committed.state != BattleResultState::Committed ||
      !committed.result.has_value() ||
      committed.result->outcome != BattleOutcome::MonsterDefeated) {
    return false;
  }
  const auto &entries = committed.result->entries;
  if (entries.size() != 2 || entries[0].sessionId != SessionId{2} ||
      entries[0].finalAssetValue != 300 ||
      entries[0].rank != std::optional<std::uint32_t>{1} || !entries[0].isTop ||
      entries[1].sessionId != SessionId{1} ||
      entries[1].finalAssetValue != 100 ||
      entries[1].rank != std::optional<std::uint32_t>{2} || entries[1].isTop ||
      entries[0].exitStatus != ParticipantExitStatus::TerminalPresent ||
      entries[1].exitStatus != ParticipantExitStatus::TerminalPresent) {
    return false;
  }
  if (claimAll.expireLoot(LootDeadlineCommand{BattleInstanceId{1}}) !=
          LootDeadlineResultCode::ResolutionClosed ||
      claimAll.resultProjection() != committed) {
    return false;
  }

  auto deadline = openCommittedBattle(1, 2);
  killMonster(deadline, 1, 1);
  if (deadline.expireLoot(LootDeadlineCommand{BattleInstanceId{1}}) !=
      LootDeadlineResultCode::Ok) {
    return false;
  }
  const auto byDeadline = deadline.resultProjection();
  if (byDeadline.state != BattleResultState::Committed ||
      !byDeadline.result.has_value() ||
      byDeadline.result->outcome != BattleOutcome::MonsterDefeated) {
    return false;
  }
  const auto &byDeadlineEntries = byDeadline.result->entries;
  if (byDeadlineEntries.size() != 2 ||
      byDeadlineEntries[0].sessionId != SessionId{1} ||
      byDeadlineEntries[0].finalAssetValue != 0 ||
      byDeadlineEntries[0].rank != std::optional<std::uint32_t>{1} ||
      !byDeadlineEntries[0].isTop ||
      byDeadlineEntries[1].sessionId != SessionId{2} ||
      byDeadlineEntries[1].finalAssetValue != 0 ||
      byDeadlineEntries[1].rank != std::optional<std::uint32_t>{1} ||
      !byDeadlineEntries[1].isTop) {
    return false;
  }
  return deadline.expireLoot(LootDeadlineCommand{BattleInstanceId{1}}) ==
             LootDeadlineResultCode::ResolutionClosed &&
         claim(deadline, 1, 1, 9, 2).code ==
             ClaimLootResultCode::ResolutionClosed &&
         deadline.resultProjection() == byDeadline;
}

// Required case 10: a returned result copy can be modified by the test without
// changing the next BattleInstance projection.
bool mutatedResultCopyDoesNotAffectBattleProjection() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);
  if (claim(battle, 1, 1, 1, 2).code != ClaimLootResultCode::Ok) {
    return false;
  }
  std::uint32_t sequence = 0;
  moveTo(battle, 1, 2, -3250, 8750, sequence);
  if (claim(battle, 1, 2, 2, 1).code != ClaimLootResultCode::Ok) {
    return false;
  }
  const auto original = battle.resultProjection();
  if (original.state != BattleResultState::Committed ||
      !original.result.has_value() || original.result->entries.empty()) {
    return false;
  }
  auto mutated = battle.resultProjection();
  mutated.result->entries[0].finalAssetValue = 0;
  mutated.result->entries[0].rank = std::nullopt;
  mutated.result->entries[0].isTop = false;
  return battle.resultProjection() == original &&
         mutated.result != original.result;
}

// CombatTimeout commits immediately without Drop generation: no drops or
// holdings, all captured entries with value 0, null rank and top false. A late
// attack and a repeated combat deadline cannot rebuild or mutate.
bool combatTimeoutCommitsImmediatelyWithoutDrops() {
  auto battle = openCommittedBattle(1, 2);
  const auto completedAt =
      std::chrono::steady_clock::time_point{std::chrono::hours{2}};
  if (battle.expireCombat(CombatDeadlineCommand{BattleInstanceId{1}},
                          completedAt) != CombatDeadlineResultCode::Ok) {
    return false;
  }
  const auto projection = battle.resultProjection();
  if (projection.state != BattleResultState::Committed ||
      !projection.result.has_value()) {
    return false;
  }
  const auto &result = *projection.result;
  const auto loot = battle.lootProjection();
  if (result.outcome != BattleOutcome::CombatTimeout ||
      result.roomId != RoomId{7} || result.battleId != BattleInstanceId{1} ||
      !loot.drops.empty() || !loot.holdings.empty() ||
      result.entries.size() != 2) {
    return false;
  }
  for (std::size_t index = 0; index < result.entries.size(); ++index) {
    const auto &entry = result.entries[index];
    if (entry.sessionId != SessionId{index + 1} || entry.finalAssetValue != 0 ||
        entry.rank.has_value() || entry.isTop) {
      return false;
    }
  }
  const auto late = battle.attack(
      AttackCommand{.commandId = CommandId{.high = 0, .low = 100},
                    .sessionId = SessionId{1},
                    .generation = SessionGeneration{1},
                    .battleId = BattleInstanceId{1},
                    .targetHint = CombatRuleset::monsterId},
      std::chrono::steady_clock::time_point{std::chrono::hours{3}});
  return late.code == AttackResultCode::TerminalAlreadyDecided &&
         battle.expireCombat(CombatDeadlineCommand{BattleInstanceId{1}},
                             completedAt) ==
             CombatDeadlineResultCode::TerminalAlreadyDecided &&
         battle.resultProjection() == projection;
}

// An exited participant stays captured with frozen holdings through commit,
// and the Battle-owned captured projection is mutated to the terminal statuses
// only after the result built successfully.
bool exitedParticipantCommitsWithTerminalStatuses() {
  auto battle = openCommittedBattle(1, 2);
  if (battle.disconnect(exitCommand(2, 1)) != BattleLoadResultCode::Ok) {
    return false;
  }
  killMonster(battle, 1, 1);
  if (claim(battle, 1, 1, 1, 2).code != ClaimLootResultCode::Ok ||
      battle.expireLoot(LootDeadlineCommand{BattleInstanceId{1}}) !=
          LootDeadlineResultCode::Ok) {
    return false;
  }
  const auto projection = battle.resultProjection();
  if (projection.state != BattleResultState::Committed ||
      !projection.result.has_value() ||
      projection.result->outcome != BattleOutcome::MonsterDefeated) {
    return false;
  }
  const auto &entries = projection.result->entries;
  if (entries.size() != 2 || entries[0].sessionId != SessionId{1} ||
      entries[0].finalAssetValue != 100 ||
      entries[0].exitStatus != ParticipantExitStatus::TerminalPresent ||
      entries[0].rank != std::optional<std::uint32_t>{1} || !entries[0].isTop ||
      entries[1].sessionId != SessionId{2} || entries[1].finalAssetValue != 0 ||
      entries[1].exitStatus != ParticipantExitStatus::TerminalExited ||
      entries[1].rank != std::optional<std::uint32_t>{2} || entries[1].isTop) {
    return false;
  }
  const auto load = battle.projection();
  return exitStatusOf(load, 1) == ParticipantExitStatus::TerminalPresent &&
         exitStatusOf(load, 2) == ParticipantExitStatus::TerminalExited;
}

// An empty MonsterDefeated Drop source is fabricated input, not a valid
// resolved ledger: invariant failure with no result.
bool emptyMonsterDefeatedDropSourceFails() {
  auto source = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  source.drops = {};
  source.holdings = {};
  const auto built = buildFinalResult(source, RelicCatalog::v1Snapshot());
  return built.status == ResultBuildStatus::InvariantBroken &&
         !built.result.has_value();
}

// A Drop still Available at result build time is an unresolved ledger, not a
// valid source: invariant failure with no result.
bool availableDropAtBuildFails() {
  auto source = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  source.drops = {drop(1, 1, 1, LootDropState::Available, std::nullopt),
                  drop(2, 1, 1, LootDropState::Unclaimed, std::nullopt)};
  source.holdings = {holdingProjection(2, 1, 1)};
  const auto built = buildFinalResult(source, RelicCatalog::v1Snapshot());
  return built.status == ResultBuildStatus::InvariantBroken &&
         !built.result.has_value();
}

// Claimed must have exactly one owner, Unclaimed must have none, and every
// Claimed owner must be a captured SessionId. Each violation fails with no
// result.
bool claimOwnerStateViolationsFail() {
  auto claimedWithoutOwner = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  claimedWithoutOwner.drops = {
      drop(1, 1, 1, LootDropState::Claimed, std::nullopt),
      drop(2, 1, 1, LootDropState::Unclaimed, std::nullopt)};
  const auto noOwner =
      buildFinalResult(claimedWithoutOwner, RelicCatalog::v1Snapshot());

  auto unclaimedWithOwner = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  unclaimedWithOwner.drops = {
      drop(1, 1, 1, LootDropState::Unclaimed, 1),
      drop(2, 1, 1, LootDropState::Unclaimed, std::nullopt)};
  const auto withOwner =
      buildFinalResult(unclaimedWithOwner, RelicCatalog::v1Snapshot());

  auto unknownOwner = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  unknownOwner.drops = {drop(1, 1, 1, LootDropState::Claimed, 99),
                        drop(2, 1, 1, LootDropState::Unclaimed, std::nullopt)};
  const auto foreign =
      buildFinalResult(unknownOwner, RelicCatalog::v1Snapshot());

  return noOwner.status == ResultBuildStatus::InvariantBroken &&
         !noOwner.result.has_value() &&
         withOwner.status == ResultBuildStatus::InvariantBroken &&
         !withOwner.result.has_value() &&
         foreign.status == ResultBuildStatus::InvariantBroken &&
         !foreign.result.has_value();
}

// Every DropId must be nonzero and unique, and every Drop quantity must be
// exactly 1. Each violation fails with no result.
bool dropIdAndQuantityViolationsFail() {
  auto zeroDropId = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  zeroDropId.drops = {drop(0, 1, 1, LootDropState::Unclaimed, std::nullopt),
                      drop(2, 1, 1, LootDropState::Unclaimed, std::nullopt)};
  const auto zeroIdBuilt =
      buildFinalResult(zeroDropId, RelicCatalog::v1Snapshot());

  auto duplicateDropId = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  duplicateDropId.drops = {
      drop(1, 1, 1, LootDropState::Unclaimed, std::nullopt),
      drop(1, 1, 1, LootDropState::Unclaimed, std::nullopt)};
  const auto duplicateBuilt =
      buildFinalResult(duplicateDropId, RelicCatalog::v1Snapshot());

  auto wrongQuantity = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  wrongQuantity.drops = {drop(1, 1, 2, LootDropState::Unclaimed, std::nullopt),
                         drop(2, 1, 1, LootDropState::Unclaimed, std::nullopt)};
  const auto quantityBuilt =
      buildFinalResult(wrongQuantity, RelicCatalog::v1Snapshot());

  return zeroIdBuilt.status == ResultBuildStatus::InvariantBroken &&
         !zeroIdBuilt.result.has_value() &&
         duplicateBuilt.status == ResultBuildStatus::InvariantBroken &&
         !duplicateBuilt.result.has_value() &&
         quantityBuilt.status == ResultBuildStatus::InvariantBroken &&
         !quantityBuilt.result.has_value();
}

// Aggregate claimed Drop quantities must exactly equal holdings for every
// (SessionId, ItemId), in both directions. Each mismatch fails with no result.
bool holdingClaimAggregationMismatchFails() {
  // A claimed Drop with no matching holding.
  auto missingHolding = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  missingHolding.drops = {drop(1, 2, 1, LootDropState::Claimed, 1),
                          drop(2, 1, 1, LootDropState::Claimed, 2)};
  missingHolding.holdings = {holdingProjection(1, 2, 1)};
  const auto unbacked =
      buildFinalResult(missingHolding, RelicCatalog::v1Snapshot());

  // A holding quantity exceeding the claimed aggregation.
  auto excessHolding = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  excessHolding.drops = {drop(1, 2, 1, LootDropState::Claimed, 1),
                         drop(2, 1, 1, LootDropState::Claimed, 2)};
  excessHolding.holdings = {holdingProjection(1, 2, 2)};
  const auto excess =
      buildFinalResult(excessHolding, RelicCatalog::v1Snapshot());

  // An extra holding item that no claimed Drop backs.
  auto extraHolding = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  extraHolding.drops = {drop(1, 2, 1, LootDropState::Claimed, 1),
                        drop(2, 1, 1, LootDropState::Claimed, 2)};
  extraHolding.holdings = {holdingProjection(1, 2, 1),
                           holdingProjection(2, 1, 1),
                           holdingProjection(1, 1, 1)};
  const auto extra = buildFinalResult(extraHolding, RelicCatalog::v1Snapshot());

  return unbacked.status == ResultBuildStatus::InvariantBroken &&
         !unbacked.result.has_value() &&
         excess.status == ResultBuildStatus::InvariantBroken &&
         !excess.result.has_value() &&
         extra.status == ResultBuildStatus::InvariantBroken &&
         !extra.result.has_value();
}

// Captured SessionIds must be nonzero and unique; the source RoomId and
// BattleInstanceId must be nonzero. Each violation fails with no result.
bool duplicateOrZeroCapturedSessionFails() {
  auto duplicate = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(1, "p1", ParticipantExitStatus::GameplayEligible)});
  const auto duplicateBuilt =
      buildFinalResult(duplicate, RelicCatalog::v1Snapshot());

  auto zeroSession = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(0, "p0", ParticipantExitStatus::GameplayEligible),
       participant(1, "p1", ParticipantExitStatus::GameplayEligible)});
  const auto zeroSessionBuilt =
      buildFinalResult(zeroSession, RelicCatalog::v1Snapshot());

  auto zeroRoom = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  zeroRoom.roomId = RoomId{0};
  const auto zeroRoomBuilt =
      buildFinalResult(zeroRoom, RelicCatalog::v1Snapshot());

  auto zeroBattle = sourceWith(
      BattleOutcome::MonsterDefeated,
      {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
       participant(2, "p2", ParticipantExitStatus::GameplayEligible)});
  zeroBattle.battleId = BattleInstanceId{0};
  const auto zeroBattleBuilt =
      buildFinalResult(zeroBattle, RelicCatalog::v1Snapshot());

  return duplicateBuilt.status == ResultBuildStatus::InvariantBroken &&
         !duplicateBuilt.result.has_value() &&
         zeroSessionBuilt.status == ResultBuildStatus::InvariantBroken &&
         !zeroSessionBuilt.result.has_value() &&
         zeroRoomBuilt.status == ResultBuildStatus::InvariantBroken &&
         !zeroRoomBuilt.result.has_value() &&
         zeroBattleBuilt.status == ResultBuildStatus::InvariantBroken &&
         !zeroBattleBuilt.result.has_value();
}

// Required case 2: pure cancellation before combat/loot generation. The
// captured input is intentionally out of order; every participant appears in
// numeric SessionId order with value 0, TerminalExited, null rank, top false,
// and no winner.
bool cancellationBeforeLootBuildsAllEntriesInSessionOrder() {
  auto source =
      sourceWith(BattleOutcome::CancelledNoActiveParticipants,
                 {participant(3, "p3", ParticipantExitStatus::VoluntaryLeft),
                  participant(1, "p1", ParticipantExitStatus::Disconnected),
                  participant(2, "p2", ParticipantExitStatus::VoluntaryLeft)});
  const auto built = buildFinalResult(source, RelicCatalog::v1Snapshot());
  if (built.status != ResultBuildStatus::Built || !built.result.has_value()) {
    return false;
  }
  const auto &result = *built.result;
  if (result.outcome != BattleOutcome::CancelledNoActiveParticipants ||
      result.roomId != RoomId{7} || result.battleId != BattleInstanceId{1} ||
      result.entries.size() != 3) {
    return false;
  }
  for (std::size_t index = 0; index < result.entries.size(); ++index) {
    const auto &entry = result.entries[index];
    if (entry.sessionId != SessionId{index + 1} ||
        entry.nickname != "p" + std::to_string(index + 1) ||
        entry.finalAssetValue != 0 || entry.rank.has_value() || entry.isTop ||
        entry.exitStatus != ParticipantExitStatus::TerminalExited) {
      return false;
    }
  }
  return true;
}

// Required case 3: pure partial-holdings cancellation. One Claimed Drop is
// backed by exactly one holding and one Drop is Unclaimed; the frozen value is
// retained, every entry is SessionId ordered with null rank and top false.
bool cancellationWithPartialHoldingsKeepsFrozenValue() {
  auto source =
      sourceWith(BattleOutcome::CancelledNoActiveParticipants,
                 {participant(2, "p2", ParticipantExitStatus::VoluntaryLeft),
                  participant(1, "p1", ParticipantExitStatus::Disconnected)});
  source.drops = {drop(1, 2, 1, LootDropState::Claimed, 2),
                  drop(2, 1, 1, LootDropState::Unclaimed, std::nullopt)};
  source.holdings = {holdingProjection(2, 2, 1)};
  const auto built = buildFinalResult(source, RelicCatalog::v1Snapshot());
  if (built.status != ResultBuildStatus::Built || !built.result.has_value()) {
    return false;
  }
  const auto &entries = built.result->entries;
  return entries.size() == 2 && entries[0].sessionId == SessionId{1} &&
         entries[0].finalAssetValue == 0 &&
         entries[0].exitStatus == ParticipantExitStatus::TerminalExited &&
         !entries[0].rank.has_value() && !entries[0].isTop &&
         entries[1].sessionId == SessionId{2} &&
         entries[1].finalAssetValue == 300 &&
         entries[1].exitStatus == ParticipantExitStatus::TerminalExited &&
         !entries[1].rank.has_value() && !entries[1].isTop;
}

// Required case 4: a cancellation source containing a GameplayEligible or
// TerminalPresent captured participant is an invariant failure with no result.
bool cancellationRejectsPresentParticipants() {
  auto eligible =
      sourceWith(BattleOutcome::CancelledNoActiveParticipants,
                 {participant(1, "p1", ParticipantExitStatus::GameplayEligible),
                  participant(2, "p2", ParticipantExitStatus::VoluntaryLeft)});
  const auto eligibleBuilt =
      buildFinalResult(eligible, RelicCatalog::v1Snapshot());
  auto present =
      sourceWith(BattleOutcome::CancelledNoActiveParticipants,
                 {participant(1, "p1", ParticipantExitStatus::TerminalPresent),
                  participant(2, "p2", ParticipantExitStatus::Disconnected)});
  const auto presentBuilt =
      buildFinalResult(present, RelicCatalog::v1Snapshot());
  return eligibleBuilt.status == ResultBuildStatus::InvariantBroken &&
         !eligibleBuilt.result.has_value() &&
         presentBuilt.status == ResultBuildStatus::InvariantBroken &&
         !presentBuilt.result.has_value();
}

// Required case 5: a cancellation ledger containing an unknown generated item
// fails as CatalogIncomplete with no partial result.
bool cancellationMissingCatalogItemFails() {
  auto source =
      sourceWith(BattleOutcome::CancelledNoActiveParticipants,
                 {participant(1, "p1", ParticipantExitStatus::Disconnected),
                  participant(2, "p2", ParticipantExitStatus::VoluntaryLeft)});
  source.drops = {dropProjection(1, 99, 1), dropProjection(2, 1, 1)};
  const auto built = buildFinalResult(source, RelicCatalog::v1Snapshot());
  return built.status == ResultBuildStatus::CatalogIncomplete &&
         !built.result.has_value();
}

// Required case 6: cancellation checked multiplication and per-participant
// addition overflow each fail with no partial result. The ledgers are
// economically valid in every other respect; only the arithmetic guard fails,
// before an economic-conservation mismatch could mask it.
bool cancellationArithmeticOverflowFailsWithoutPartialResult() {
  auto multiplication =
      sourceWith(BattleOutcome::CancelledNoActiveParticipants,
                 {participant(1, "p1", ParticipantExitStatus::Disconnected),
                  participant(2, "p2", ParticipantExitStatus::VoluntaryLeft)});
  multiplication.drops = {drop(1, 2, 1, LootDropState::Claimed, 1),
                          drop(2, 1, 1, LootDropState::Claimed, 2)};
  multiplication.holdings = {holdingProjection(1, 2, UINT64_C(1) << 62)};
  const auto multiplied =
      buildFinalResult(multiplication, RelicCatalog::v1Snapshot());
  if (multiplied.status != ResultBuildStatus::ArithmeticOverflow ||
      multiplied.result.has_value()) {
    return false;
  }

  auto addition =
      sourceWith(BattleOutcome::CancelledNoActiveParticipants,
                 {participant(1, "p1", ParticipantExitStatus::Disconnected),
                  participant(2, "p2", ParticipantExitStatus::VoluntaryLeft)});
  addition.drops = {drop(1, 1, 1, LootDropState::Claimed, 1),
                    drop(2, 1, 1, LootDropState::Claimed, 2)};
  constexpr std::uint64_t kHugeQuantity = UINT64_C(93000000000000000);
  addition.holdings = {holdingProjection(1, 1, kHugeQuantity),
                       holdingProjection(1, 1, kHugeQuantity)};
  const auto summed = buildFinalResult(addition, RelicCatalog::v1Snapshot());
  return summed.status == ResultBuildStatus::ArithmeticOverflow &&
         !summed.result.has_value();
}

// Cancellation accepts exactly two ledger shapes: both empty (before loot
// generation) or the full resolved ledger (one terminal Drop per captured
// participant with holdings conserved against the Claimed Drops). Every other
// shape is invariant-broken with no result.
bool cancellationRejectsInvalidLedgerShapes() {
  auto holdingsWithoutDrops =
      sourceWith(BattleOutcome::CancelledNoActiveParticipants,
                 {participant(1, "p1", ParticipantExitStatus::Disconnected),
                  participant(2, "p2", ParticipantExitStatus::VoluntaryLeft)});
  holdingsWithoutDrops.holdings = {holdingProjection(1, 1, 1)};
  const auto unbacked =
      buildFinalResult(holdingsWithoutDrops, RelicCatalog::v1Snapshot());

  auto wrongCount =
      sourceWith(BattleOutcome::CancelledNoActiveParticipants,
                 {participant(1, "p1", ParticipantExitStatus::Disconnected),
                  participant(2, "p2", ParticipantExitStatus::VoluntaryLeft)});
  wrongCount.drops = {drop(1, 1, 1, LootDropState::Unclaimed, std::nullopt)};
  const auto counted = buildFinalResult(wrongCount, RelicCatalog::v1Snapshot());

  auto available =
      sourceWith(BattleOutcome::CancelledNoActiveParticipants,
                 {participant(1, "p1", ParticipantExitStatus::Disconnected),
                  participant(2, "p2", ParticipantExitStatus::VoluntaryLeft)});
  available.drops = {drop(1, 1, 1, LootDropState::Available, std::nullopt),
                     drop(2, 1, 1, LootDropState::Unclaimed, std::nullopt)};
  const auto unresolved =
      buildFinalResult(available, RelicCatalog::v1Snapshot());

  return unbacked.status == ResultBuildStatus::InvariantBroken &&
         !unbacked.result.has_value() &&
         counted.status == ResultBuildStatus::InvariantBroken &&
         !counted.result.has_value() &&
         unresolved.status == ResultBuildStatus::InvariantBroken &&
         !unresolved.result.has_value();
}

// Required case 7: all captured participants Leave/Disconnect before any
// combat terminal. The cancellation result commits exactly once with every
// captured entry, no combat terminal record is ever created, and repeated
// exits and the combat/loot deadlines cannot mutate it.
bool allExitsBeforeCombatCommitCancellationExactlyOnce() {
  auto battle = openCommittedBattle(1, 2);
  if (battle.leave(exitCommand(1, 1)) != BattleLoadResultCode::Ok) {
    return false;
  }
  if (battle.disconnect(exitCommand(2, 1)) != BattleLoadResultCode::Ok) {
    return false;
  }
  const auto projection = battle.resultProjection();
  if (projection.state != BattleResultState::Committed ||
      !projection.result.has_value()) {
    return false;
  }
  const auto &result = *projection.result;
  if (result.outcome != BattleOutcome::CancelledNoActiveParticipants ||
      result.roomId != RoomId{7} || result.battleId != BattleInstanceId{1} ||
      result.entries.size() != 2) {
    return false;
  }
  for (std::size_t index = 0; index < result.entries.size(); ++index) {
    const auto &entry = result.entries[index];
    if (entry.sessionId != SessionId{index + 1} || entry.finalAssetValue != 0 ||
        entry.rank.has_value() || entry.isTop ||
        entry.exitStatus != ParticipantExitStatus::TerminalExited) {
      return false;
    }
  }
  // No combat terminal record exists for the cancellation.
  const auto combat = battle.combatProjection();
  if (!combat.has_value() || combat->outcome != CombatOutcome::None ||
      combat->terminal.has_value()) {
    return false;
  }
  // Repeated exits and deadlines are no mutation.
  if (battle.leave(exitCommand(1, 1)) != BattleLoadResultCode::Ok ||
      battle.disconnect(exitCommand(2, 1)) != BattleLoadResultCode::Ok) {
    return false;
  }
  const auto completedAt =
      std::chrono::steady_clock::time_point{std::chrono::hours{2}};
  if (battle.expireCombat(CombatDeadlineCommand{BattleInstanceId{1}},
                          completedAt) !=
          CombatDeadlineResultCode::TerminalAlreadyDecided ||
      battle.expireLoot(LootDeadlineCommand{BattleInstanceId{1}}) !=
          LootDeadlineResultCode::NotEligible) {
    return false;
  }
  return battle.resultProjection() == projection;
}

// Required case 8: MonsterDefeated combat history and one claimed holding,
// then every participant exits during Open loot. The combat projection stays
// MonsterDefeated, the unresolved Drop becomes Unclaimed, and the final Battle
// outcome is cancellation with the frozen holding value and no rank/top.
bool monsterDefeatedThenAllExitDuringOpenLootCommitsCancellation() {
  auto battle = openCommittedBattle(1, 2);
  killMonster(battle, 1, 1);
  if (battle.lootProjection().resolution != LootResolutionState::Open) {
    return false;
  }
  if (claim(battle, 1, 1, 1, 2).code != ClaimLootResultCode::Ok) {
    return false;
  }
  if (battle.leave(exitCommand(1, 1)) != BattleLoadResultCode::Ok ||
      battle.disconnect(exitCommand(2, 1)) != BattleLoadResultCode::Ok) {
    return false;
  }
  const auto projection = battle.resultProjection();
  if (projection.state != BattleResultState::Committed ||
      !projection.result.has_value()) {
    return false;
  }
  const auto &result = *projection.result;
  if (result.outcome != BattleOutcome::CancelledNoActiveParticipants ||
      result.entries.size() != 2) {
    return false;
  }
  const auto &entries = result.entries;
  if (entries[0].sessionId != SessionId{1} ||
      entries[0].finalAssetValue != 100 ||
      entries[0].exitStatus != ParticipantExitStatus::TerminalExited ||
      entries[0].rank.has_value() || entries[0].isTop ||
      entries[1].sessionId != SessionId{2} || entries[1].finalAssetValue != 0 ||
      entries[1].exitStatus != ParticipantExitStatus::TerminalExited ||
      entries[1].rank.has_value() || entries[1].isTop) {
    return false;
  }
  // Combat history is untouched: the projection stays MonsterDefeated.
  const auto combat = battle.combatProjection();
  if (!combat.has_value() ||
      combat->outcome != CombatOutcome::MonsterDefeated ||
      !combat->terminal.has_value()) {
    return false;
  }
  // The unresolved Drop was closed to Unclaimed; the claimed holding stays.
  const auto loot = battle.lootProjection();
  if (loot.resolution != LootResolutionState::Resolved ||
      loot.drops.size() != 2 || loot.holdings.size() != 1) {
    return false;
  }
  bool claimedSeen = false;
  bool unclaimedSeen = false;
  for (const auto &dropEntry : loot.drops) {
    if (dropEntry.state == LootDropState::Claimed &&
        dropEntry.owner == std::optional<SessionId>{SessionId{1}}) {
      claimedSeen = true;
    } else if (dropEntry.state == LootDropState::Unclaimed &&
               !dropEntry.owner.has_value()) {
      unclaimedSeen = true;
    }
  }
  if (!claimedSeen || !unclaimedSeen) {
    return false;
  }
  // The sticky Packet A exit statuses are retained: the cancellation commit
  // stores the immutable result but never rewrites the captured exit
  // projection.
  const auto load = battle.projection();
  return exitStatusOf(load, 1) == ParticipantExitStatus::VoluntaryLeft &&
         exitStatusOf(load, 2) == ParticipantExitStatus::Disconnected;
}

} // namespace

int main() {
  if (!monsterDefeatedRanksThreeHundredAndOneHundred() ||
      !tenParticipantTieYieldsOneOneThree() ||
      !equalValuePermutationsProduceEqualOrderedResult() ||
      !exitedParticipantKeepsEntryWithFrozenHoldings() ||
      !combatTimeoutBuildsAllEntriesNoRankNoWinner() ||
      !combatTimeoutRejectsDropsOrHoldings() ||
      !unknownOutcomeIsInvariantFailure() ||
      !missingDropAndHoldingItemsFailGeneration() ||
      !arithmeticOverflowFailsWithoutPartialResult() ||
      !foreignHoldingAndZeroQuantityAreInvariantFailures() ||
      !monsterDefeatedWhileLootOpenRemainsNotReady() ||
      !claimAllAndDeadlineCommitExactlyOnce() ||
      !mutatedResultCopyDoesNotAffectBattleProjection() ||
      !combatTimeoutCommitsImmediatelyWithoutDrops() ||
      !exitedParticipantCommitsWithTerminalStatuses() ||
      !emptyMonsterDefeatedDropSourceFails() || !availableDropAtBuildFails() ||
      !claimOwnerStateViolationsFail() || !dropIdAndQuantityViolationsFail() ||
      !holdingClaimAggregationMismatchFails() ||
      !duplicateOrZeroCapturedSessionFails() ||
      !cancellationBeforeLootBuildsAllEntriesInSessionOrder() ||
      !cancellationWithPartialHoldingsKeepsFrozenValue() ||
      !cancellationRejectsPresentParticipants() ||
      !cancellationMissingCatalogItemFails() ||
      !cancellationArithmeticOverflowFailsWithoutPartialResult() ||
      !cancellationRejectsInvalidLedgerShapes() ||
      !allExitsBeforeCombatCommitCancellationExactlyOnce() ||
      !monsterDefeatedThenAllExitDuringOpenLootCommitsCancellation()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
