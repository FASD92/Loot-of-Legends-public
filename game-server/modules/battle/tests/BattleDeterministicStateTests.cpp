#include <lol/battle/BattleDeterministicState.hpp>
#include <lol/battle/BattleLoadApi.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {

using lol::battle::ArenaLoadCompleteCommand;
using lol::battle::AttackCommand;
using lol::battle::BattleAdmissionSnapshot;
using lol::battle::BattleDeterministicState;
using lol::battle::BattleInstance;
using lol::battle::BattleLoadResultCode;
using lol::battle::BattleLoadState;
using lol::battle::BattleRulesetVersion;
using lol::battle::BattleSeed;
using lol::battle::BattleStartCandidate;
using lol::battle::BattleTime;
using lol::battle::CandidateDisconnectedCommand;
using lol::battle::CombatDeadlineCommand;
using lol::battle::CombatDeadlineResultCode;
using lol::battle::CombatRuleset;
using lol::battle::CommandId;
using lol::battle::DirectionIntent;
using lol::battle::LootDeadlineCommand;
using lol::battle::LootDeadlineResultCode;
using lol::battle::MoveCommand;
using lol::battle::MovementResultCode;
using lol::battle::MovementTickCommand;
using lol::battle::RelicCatalog;
using lol::battle::RelicRuleset;
using lol::shared::AccountId;
using lol::shared::BattleInstanceId;
using lol::shared::RoomId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;

AccountId account(std::uint8_t suffix) {
  AccountId::Bytes bytes{};
  bytes.back() = suffix;
  return AccountId{bytes};
}

BattleAdmissionSnapshot admission(std::uint64_t seed = 17,
                                  std::uint64_t generationOne = 3,
                                  std::uint64_t generationTwo = 4) {
  return BattleAdmissionSnapshot{
      .roomId = RoomId{7},
      .battleId = BattleInstanceId{1},
      .candidates =
          {
              BattleStartCandidate{account(1), SessionId{11},
                                   SessionGeneration{generationOne},
                                   "private-one"},
              BattleStartCandidate{account(2), SessionId{12},
                                   SessionGeneration{generationTwo},
                                   "private-two"},
          },
      .rulesetVersion = BattleRulesetVersion{1},
      .seed = seed,
  };
}

BattleInstance committedBattle(std::uint64_t seed = 17,
                               std::uint64_t generationOne = 3,
                               std::uint64_t generationTwo = 4) {
  auto created =
      BattleInstance::create(admission(seed, generationOne, generationTwo));
  if (created.code != BattleLoadResultCode::Ok || !created.battle.has_value() ||
      created.battle->openLoadBarrier() != BattleLoadResultCode::Ok) {
    std::abort();
  }
  auto battle = std::move(*created.battle);
  for (const auto session : {11ULL, 12ULL}) {
    if (battle.completeLoad(
            ArenaLoadCompleteCommand{SessionId{session},
                                     session == 11
                                         ? SessionGeneration{generationOne}
                                         : SessionGeneration{generationTwo},
                                     RoomId{7}, BattleInstanceId{1}},
            true) != BattleLoadResultCode::Ok) {
      std::abort();
    }
  }
  if (battle.projection().state != BattleLoadState::GameplayCommitted) {
    std::abort();
  }
  return battle;
}

bool explicitSeedAndRulesetAreFrozen() {
  auto first = committedBattle(17);
  auto second = committedBattle(17);
  const auto firstState = first.exportDeterministicState();
  const auto secondState = second.exportDeterministicState();
  return firstState.rulesetVersion == 1 && firstState.seed == 17 &&
         firstState.battleTime == BattleTime::fromLogicalTick(0) &&
         firstState.combatDeadlineTick.has_value() &&
         *firstState.combatDeadlineTick == 600 &&
         !firstState.loadDeadlineTick.has_value() && firstState == secondState;
}

bool zeroSeedAndUnknownRulesetAreRejected() {
  auto zeroSeed = admission();
  zeroSeed.seed = 0;
  auto unknownRuleset = admission();
  unknownRuleset.rulesetVersion = 2;
  const auto zeroResult = BattleInstance::create(std::move(zeroSeed));
  const auto unknownResult = BattleInstance::create(std::move(unknownRuleset));
  const auto unknownDropRuleset = lol::battle::generateDrops(
      RoomId{7}, BattleInstanceId{1}, BattleSeed{17},
      static_cast<std::uint16_t>(RelicRuleset::version + 1), 2,
      RelicCatalog::v1Snapshot());
  return zeroResult.code == BattleLoadResultCode::InvalidArgument &&
         unknownResult.code == BattleLoadResultCode::InvalidArgument &&
         !unknownDropRuleset.has_value();
}

bool spawnUsesIntegerVersionedTable() {
  auto battle = committedBattle();
  const auto movement = battle.movementProjection();
  if (movement.players.size() != 2) {
    return false;
  }
  return movement.players[0].posXMillimeter == 2500 &&
         movement.players[0].posYMillimeter == 0 &&
         movement.players[1].posXMillimeter == -2500 &&
         movement.players[1].posYMillimeter == 0;
}

bool participantSlotsAreDenseAfterCandidateExit() {
  auto snapshot = admission();
  snapshot.candidates.push_back(BattleStartCandidate{
      account(3), SessionId{13}, SessionGeneration{5}, "private-three"});
  auto created = BattleInstance::create(std::move(snapshot));
  if (created.code != BattleLoadResultCode::Ok || !created.battle.has_value()) {
    return false;
  }
  auto battle = std::move(*created.battle);
  if (battle.openLoadBarrier() != BattleLoadResultCode::Ok ||
      battle.disconnect(
          CandidateDisconnectedCommand{.sessionId = SessionId{11},
                                       .generation = SessionGeneration{3},
                                       .roomId = RoomId{7},
                                       .battleId = BattleInstanceId{1}},
          BattleTime::fromLogicalTick(1)) != BattleLoadResultCode::Ok ||
      battle.completeLoad(
          ArenaLoadCompleteCommand{.sessionId = SessionId{12},
                                   .generation = SessionGeneration{4},
                                   .roomId = RoomId{7},
                                   .battleId = BattleInstanceId{1}},
          true) != BattleLoadResultCode::Ok ||
      battle.completeLoad(
          ArenaLoadCompleteCommand{.sessionId = SessionId{13},
                                   .generation = SessionGeneration{5},
                                   .roomId = RoomId{7},
                                   .battleId = BattleInstanceId{1}},
          true) != BattleLoadResultCode::Ok) {
    return false;
  }
  const auto state = battle.exportDeterministicState();
  return state.capturedParticipants.size() == 2 &&
         state.participants.size() == 2 &&
         state.capturedParticipants[0].slot == 1 &&
         state.capturedParticipants[1].slot == 2 &&
         state.participants[0].slot == 1 && state.participants[1].slot == 2 &&
         state.participants[0].sessionId == SessionId{12} &&
         state.participants[1].sessionId == SessionId{13};
}

bool stateRoundTripsWithoutPrivateIdentityOrGeneration() {
  auto battle = committedBattle();
  const auto time = BattleTime::fromLogicalTick(2);
  if (battle.acceptMove(MoveCommand{SessionId{11}, SessionGeneration{3},
                                    BattleInstanceId{1}, 1,
                                    DirectionIntent{1, 0, 0}},
                        time) != MovementResultCode::Ok ||
      battle.integrateMovement(MovementTickCommand{BattleInstanceId{1}, 1},
                               time) != MovementResultCode::Ok ||
      battle.attack(AttackCommand{CommandId{0, 1}, SessionId{11},
                                  SessionGeneration{3}, BattleInstanceId{1},
                                  CombatRuleset::monsterId},
                    BattleTime::fromLogicalTick(18))
              .code != lol::battle::AttackResultCode::Ok) {
    return false;
  }

  const BattleDeterministicState exported = battle.exportDeterministicState();
  auto restored = committedBattle(17, 31, 32);
  if (restored.importDeterministicState(exported) !=
      lol::battle::BattleStateImportResultCode::Ok) {
    return false;
  }
  const auto restoredMovement = restored.movementProjection();
  const auto originalMovement = battle.movementProjection();
  return restored.exportDeterministicState() == exported &&
         restoredMovement.battleId == originalMovement.battleId &&
         restoredMovement.serverTick == originalMovement.serverTick &&
         restoredMovement.players == originalMovement.players &&
         restored.combatProjection() == battle.combatProjection();
}

bool pureReplayDoesNotAdvanceLogicalTime() {
  auto battle = committedBattle();
  const AttackCommand command{CommandId{0, 1}, SessionId{11},
                              SessionGeneration{3}, BattleInstanceId{1},
                              CombatRuleset::monsterId};
  const auto first = battle.attack(command, BattleTime::fromLogicalTick(1));
  if (first.code != lol::battle::AttackResultCode::Ok) {
    return false;
  }
  const auto beforeReplay = battle.exportDeterministicState();
  const auto replay = battle.attack(command, BattleTime::fromLogicalTick(20));
  return replay == first && battle.exportDeterministicState() == beforeReplay;
}

bool idempotentInputControlDoesNotAdvanceLogicalTime() {
  auto battle = committedBattle();
  if (battle.suspendInput(SessionId{11}, SessionGeneration{3},
                          BattleTime::fromLogicalTick(2)) !=
      lol::battle::BattleInputResultCode::Ok) {
    return false;
  }
  const auto beforeDuplicate = battle.exportDeterministicState();
  if (battle.suspendInput(SessionId{11}, SessionGeneration{3},
                          BattleTime::fromLogicalTick(20)) !=
      lol::battle::BattleInputResultCode::Ok) {
    return false;
  }
  return battle.exportDeterministicState() == beforeDuplicate;
}

bool changedSeedChangesDeterministicState() {
  const auto first = lol::battle::generateDrops(
      RoomId{7}, BattleInstanceId{1}, BattleSeed{17}, RelicRuleset::version, 2,
      RelicCatalog::v1Snapshot());
  const auto second = lol::battle::generateDrops(
      RoomId{7}, BattleInstanceId{1}, BattleSeed{18}, RelicRuleset::version, 2,
      RelicCatalog::v1Snapshot());
  if (!first.has_value() || !second.has_value() || *first == *second) {
    return false;
  }
  auto firstBattle = committedBattle(17);
  auto secondBattle = committedBattle(18);
  const auto defeat = [](BattleInstance &battle) {
    for (std::uint64_t index = 0; index < 16; ++index) {
      if (battle
              .attack(AttackCommand{CommandId{0, index + 1}, SessionId{11},
                                    SessionGeneration{3}, BattleInstanceId{1},
                                    CombatRuleset::monsterId},
                      BattleTime::fromLogicalTick(index * 16))
              .code != lol::battle::AttackResultCode::Ok) {
        return false;
      }
    }
    return true;
  };
  if (!defeat(firstBattle) || !defeat(secondBattle)) {
    return false;
  }
  return firstBattle.exportDeterministicState().drops !=
         secondBattle.exportDeterministicState().drops;
}

bool everyNonzeroSeedIsTheActualRngInput() {
  auto battle = committedBattle(1);
  for (std::uint64_t index = 0; index < 16; ++index) {
    if (battle
            .attack(AttackCommand{CommandId{0, index + 1}, SessionId{11},
                                  SessionGeneration{3}, BattleInstanceId{1},
                                  CombatRuleset::monsterId},
                    BattleTime::fromLogicalTick(index * 16))
            .code != lol::battle::AttackResultCode::Ok) {
      return false;
    }
  }
  const auto expected = lol::battle::generateDrops(
      RoomId{7}, BattleInstanceId{1}, BattleSeed{1}, RelicRuleset::version, 2,
      RelicCatalog::v1Snapshot());
  if (!expected.has_value()) {
    return false;
  }
  const auto state = battle.exportDeterministicState();
  if (state.drops.size() != expected->size()) {
    return false;
  }
  for (std::size_t index = 0; index < expected->size(); ++index) {
    if (state.drops[index].drop != (*expected)[index]) {
      return false;
    }
  }
  return true;
}

bool loadDeadlineIsCapturedAndRequired() {
  auto created = BattleInstance::create(admission());
  if (created.code != BattleLoadResultCode::Ok || !created.battle.has_value()) {
    return false;
  }
  auto battle = std::move(*created.battle);
  if (battle.openLoadBarrier() != BattleLoadResultCode::Ok) {
    return false;
  }
  const auto exported = battle.exportDeterministicState();
  if (exported.state != BattleLoadState::LoadBarrierOpen ||
      exported.loadDeadlineTick != std::optional<std::uint64_t>{200} ||
      exported.combatDeadlineTick.has_value() ||
      exported.lootDeadlineTick.has_value()) {
    return false;
  }

  auto restored = BattleInstance::create(admission());
  if (restored.code != BattleLoadResultCode::Ok ||
      !restored.battle.has_value() ||
      restored.battle->openLoadBarrier() != BattleLoadResultCode::Ok) {
    return false;
  }
  auto invalid = exported;
  invalid.loadDeadlineTick.reset();
  return restored.battle->importDeterministicState(invalid) ==
         lol::battle::BattleStateImportResultCode::InvariantBroken;
}

bool phaseTransitionsFreezeDeadlinesInValueState() {
  auto battle = committedBattle();
  auto state = battle.exportDeterministicState();
  if (state.combatDeadlineTick != std::optional<std::uint64_t>{600} ||
      state.lootDeadlineTick.has_value()) {
    return false;
  }
  for (std::uint64_t index = 0; index < 16; ++index) {
    const auto result =
        battle.attack(AttackCommand{CommandId{0, index + 1}, SessionId{11},
                                    SessionGeneration{3}, BattleInstanceId{1},
                                    CombatRuleset::monsterId},
                      BattleTime::fromLogicalTick(index * 16));
    if (result.code != lol::battle::AttackResultCode::Ok) {
      return false;
    }
  }
  state = battle.exportDeterministicState();
  return state.combatTerminal.has_value() &&
         !state.combatDeadlineTick.has_value() &&
         state.lootDeadlineTick == std::optional<std::uint64_t>{540};
}

bool terminalStateRoundTripsWithResultAndLoot() {
  auto battle = committedBattle();
  for (std::uint64_t index = 0; index < 16; ++index) {
    if (battle
            .attack(AttackCommand{CommandId{0, index + 1}, SessionId{11},
                                  SessionGeneration{3}, BattleInstanceId{1},
                                  CombatRuleset::monsterId},
                    BattleTime::fromLogicalTick(index * 16))
            .code != lol::battle::AttackResultCode::Ok) {
      return false;
    }
  }
  if (battle.expireLoot(LootDeadlineCommand{BattleInstanceId{1}},
                        BattleTime::fromLogicalTick(540)) !=
      LootDeadlineResultCode::Ok) {
    return false;
  }
  const auto exported = battle.exportDeterministicState();
  if (exported.resultState != lol::battle::BattleResultState::Committed ||
      !exported.committedResult.has_value() ||
      exported.drops.size() != exported.participants.size()) {
    return false;
  }

  auto restored = committedBattle(17, 31, 32);
  return restored.importDeterministicState(exported) ==
             lol::battle::BattleStateImportResultCode::Ok &&
         restored.exportDeterministicState() == exported &&
         restored.resultProjection() == battle.resultProjection() &&
         restored.lootProjection() == battle.lootProjection();
}

bool canonicalCommandsUseExplicitLogicalTime() {
  auto first = committedBattle();
  auto second = committedBattle();
  const MoveCommand move{SessionId{11}, SessionGeneration{3},
                         BattleInstanceId{1}, 1, DirectionIntent{1, 0, 0}};
  const AttackCommand attack{CommandId{0, 1}, SessionId{11},
                             SessionGeneration{3}, BattleInstanceId{1},
                             CombatRuleset::monsterId};
  for (auto *battle : {&first, &second}) {
    if (battle->acceptMove(move, BattleTime::fromLogicalTick(1)) !=
            MovementResultCode::Ok ||
        battle->integrateMovement(MovementTickCommand{BattleInstanceId{1}, 1},
                                  BattleTime::fromLogicalTick(2)) !=
            MovementResultCode::Ok ||
        battle->attackWithApplied(attack, BattleTime::fromLogicalTick(16))
                .result.code != lol::battle::AttackResultCode::Ok) {
      return false;
    }
  }
  return first.exportDeterministicState() ==
             second.exportDeterministicState() &&
         first.exportDeterministicState().battleTime ==
             BattleTime::fromLogicalTick(16);
}

bool deadlineCommandsUseFrozenLogicalTicks() {
  auto load = BattleInstance::create(admission());
  if (load.code != BattleLoadResultCode::Ok || !load.battle.has_value() ||
      load.battle->openLoadBarrier() != BattleLoadResultCode::Ok) {
    return false;
  }
  const auto loadBefore = load.battle->exportDeterministicState();
  if (load.battle->expireLoadBarrier(
          lol::battle::LoadBarrierDeadlineCommand{RoomId{7},
                                                  BattleInstanceId{1}},
          BattleTime::fromLogicalTick(199)) !=
          BattleLoadResultCode::NotEligible ||
      load.battle->exportDeterministicState() != loadBefore ||
      load.battle->expireLoadBarrier(lol::battle::LoadBarrierDeadlineCommand{
          RoomId{7}, BattleInstanceId{1}}) != BattleLoadResultCode::Ok ||
      load.battle->exportDeterministicState().battleTime !=
          BattleTime::fromLogicalTick(200)) {
    return false;
  }

  auto combat = committedBattle();
  const auto combatBefore = combat.exportDeterministicState();
  if (combat.expireCombat(CombatDeadlineCommand{BattleInstanceId{1}},
                          BattleTime::fromLogicalTick(599)) !=
          CombatDeadlineResultCode::NotEligible ||
      combat.exportDeterministicState() != combatBefore ||
      combat.expireCombat(CombatDeadlineCommand{BattleInstanceId{1}}) !=
          CombatDeadlineResultCode::Ok) {
    return false;
  }
  const auto combatAfter = combat.exportDeterministicState();
  return combatAfter.battleTime == BattleTime::fromLogicalTick(600) &&
         combatAfter.resultState == lol::battle::BattleResultState::Committed;
}

bool restoredResultKeepsProductionOrdering() {
  const auto makeBattle = [] {
    auto snapshot = admission();
    std::swap(snapshot.candidates[0], snapshot.candidates[1]);
    auto created = BattleInstance::create(std::move(snapshot));
    if (created.code != BattleLoadResultCode::Ok ||
        !created.battle.has_value() ||
        created.battle->openLoadBarrier() != BattleLoadResultCode::Ok) {
      std::abort();
    }
    auto battle = std::move(*created.battle);
    for (const auto &[session, generation] :
         {std::pair{12ULL, 4ULL}, std::pair{11ULL, 3ULL}}) {
      if (battle.completeLoad(
              ArenaLoadCompleteCommand{SessionId{session},
                                       SessionGeneration{generation}, RoomId{7},
                                       BattleInstanceId{1}},
              true) != BattleLoadResultCode::Ok) {
        std::abort();
      }
    }
    return battle;
  };

  auto original = makeBattle();
  if (original.expireCombat(CombatDeadlineCommand{BattleInstanceId{1}},
                            BattleTime::fromLogicalTick(600)) !=
      CombatDeadlineResultCode::Ok) {
    return false;
  }
  auto canonicalState = original.exportDeterministicState();
  if (!canonicalState.committedResult.has_value()) {
    return false;
  }
  std::sort(
      canonicalState.committedResult->entries.begin(),
      canonicalState.committedResult->entries.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.slot < rhs.slot; });

  auto restored = makeBattle();
  return restored.importDeterministicState(canonicalState) ==
             lol::battle::BattleStateImportResultCode::Ok &&
         restored.resultProjection() == original.resultProjection();
}

} // namespace

int main() {
  if (!explicitSeedAndRulesetAreFrozen() ||
      !zeroSeedAndUnknownRulesetAreRejected() ||
      !spawnUsesIntegerVersionedTable() ||
      !participantSlotsAreDenseAfterCandidateExit() ||
      !stateRoundTripsWithoutPrivateIdentityOrGeneration() ||
      !pureReplayDoesNotAdvanceLogicalTime() ||
      !idempotentInputControlDoesNotAdvanceLogicalTime() ||
      !changedSeedChangesDeterministicState() ||
      !everyNonzeroSeedIsTheActualRngInput() ||
      !loadDeadlineIsCapturedAndRequired() ||
      !phaseTransitionsFreezeDeadlinesInValueState() ||
      !terminalStateRoundTripsWithResultAndLoot() ||
      !canonicalCommandsUseExplicitLogicalTime() ||
      !deadlineCommandsUseFrozenLogicalTicks() ||
      !restoredResultKeepsProductionOrdering()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
