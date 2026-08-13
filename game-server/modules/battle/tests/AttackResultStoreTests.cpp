#include <lol/battle/CombatApi.hpp>
#include <lol/battle/CombatResultStore.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>

namespace {

using namespace std::chrono_literals;
using lol::battle::AttackCommand;
using lol::battle::AttackResultCode;
using lol::battle::AttackResultStore;
using lol::battle::AttackResultStoreDecision;
using lol::battle::AttackTerminalResult;
using lol::battle::CombatOutcome;
using lol::battle::CombatRuleset;
using lol::battle::CommandId;
using lol::shared::BattleInstanceId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;

AttackCommand command(std::uint64_t id, std::uint64_t target = 1,
                      std::uint64_t session = 11, std::uint64_t generation = 2,
                      std::uint64_t battle = 7) {
  return AttackCommand{
      .commandId = CommandId{.high = 0, .low = id},
      .sessionId = SessionId{session},
      .generation = SessionGeneration{generation},
      .battleId = BattleInstanceId{battle},
      .targetHint = target,
  };
}

AttackTerminalResult accepted(const AttackCommand &attack,
                              std::uint32_t remainingHitPoints = 1580) {
  return AttackTerminalResult{
      .commandId = attack.commandId,
      .battleId = attack.battleId,
      .code = AttackResultCode::Ok,
      .monsterId = CombatRuleset::monsterId,
      .remainingHitPoints = remainingHitPoints,
      .rulesetVersion = CombatRuleset::version,
      .outcome = CombatOutcome::None,
  };
}

AttackResultStore store() {
  return AttackResultStore{SessionId{11}, SessionGeneration{2},
                           BattleInstanceId{7}};
}

bool duplicateReplaysAndConflictPreservesResult() {
  auto results = store();
  const auto original = command(1);
  if (results.inspect(original).decision !=
          AttackResultStoreDecision::Available ||
      !results.retain(original, accepted(original))) {
    return false;
  }

  const auto replay = results.inspect(original);
  const auto conflict = results.inspect(command(1, 2));
  return replay.decision == AttackResultStoreDecision::Replay &&
         replay.result == accepted(original) &&
         conflict.decision == AttackResultStoreDecision::Conflict &&
         !conflict.result.has_value() && results.size() == 1 &&
         results.inspect(original).result == accepted(original);
}

bool scopeMismatchNeverConsumesCapacity() {
  auto results = store();
  return results.inspect(command(1, 1, 12)).decision ==
             AttackResultStoreDecision::ScopeMismatch &&
         results.inspect(command(1, 1, 11, 3)).decision ==
             AttackResultStoreDecision::ScopeMismatch &&
         results.inspect(command(1, 1, 11, 2, 8)).decision ==
             AttackResultStoreDecision::ScopeMismatch &&
         !results.retain(command(1, 1, 12), accepted(command(1, 1, 12))) &&
         results.size() == 0;
}

bool capacityRejectsNewCommandButKeepsReplay() {
  auto results = store();
  for (std::uint64_t id = 1; id <= AttackResultStore::maximumResults; ++id) {
    const auto attack = command(id);
    if (results.inspect(attack).decision !=
            AttackResultStoreDecision::Available ||
        !results.retain(attack, accepted(attack))) {
      return false;
    }
  }

  const auto overflow = command(AttackResultStore::maximumResults + 1);
  return results.size() == AttackResultStore::maximumResults &&
         results.inspect(overflow).decision ==
             AttackResultStoreDecision::Overloaded &&
         !results.retain(overflow, accepted(overflow)) &&
         results.inspect(command(1)).decision ==
             AttackResultStoreDecision::Replay &&
         results.inspect(command(1, 2)).decision ==
             AttackResultStoreDecision::Conflict &&
         results.size() == AttackResultStore::maximumResults;
}

bool expiryStartsAtBattleCompletionAndIsInclusive() {
  auto results = store();
  const auto first = command(1);
  const auto second = command(2);
  if (!results.retain(first, accepted(first)) ||
      !results.retain(second, accepted(second)) ||
      results.evictExpired(AttackResultStore::Clock::time_point{} + 1h) != 0) {
    return false;
  }

  const auto completedAt = AttackResultStore::Clock::time_point{} + 2h;
  results.markBattleCompleted(completedAt);
  return results.evictExpired(completedAt + 29999ms) == 0 &&
         results.size() == 2 &&
         results.evictExpired(completedAt + 30000ms) == 2 &&
         results.size() == 0;
}

} // namespace

int main() {
  if (!duplicateReplaysAndConflictPreservesResult() ||
      !scopeMismatchNeverConsumesCapacity() ||
      !capacityRejectsNewCommandButKeepsReplay() ||
      !expiryStartsAtBattleCompletionAndIsInclusive()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
