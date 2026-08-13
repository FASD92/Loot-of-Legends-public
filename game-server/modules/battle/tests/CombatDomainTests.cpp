#include <lol/battle/CombatApi.hpp>

#include <cstdint>
#include <cstdlib>

namespace {

using lol::battle::CombatPosition;
using lol::battle::CombatRuleset;
using lol::battle::Monster;
using lol::battle::MonsterDamageResult;
using lol::battle::MonsterState;

bool rulesetMatchesFrozenV1Contract() {
  return CombatRuleset::version == 1 && CombatRuleset::monsterCount == 1 &&
         CombatRuleset::monsterId == 1 &&
         CombatRuleset::spawnPosition == CombatPosition{0, 0} &&
         CombatRuleset::monsterHitPoints == 1600 &&
         CombatRuleset::attackDamage == 20 &&
         CombatRuleset::attackRangeMillimeters == 3000 &&
         CombatRuleset::perPlayerCooldownMillis == 750 &&
         CombatRuleset::combatDeadlineMillis == 30000 &&
         CombatRuleset::attackRatePerSecond == 8 &&
         CombatRuleset::attackBurst == 4;
}

bool rangeUsesServerMillimetersAndIncludesBoundary() {
  return CombatRuleset::inAttackRange(CombatPosition{0, 0},
                                      CombatPosition{3000, 0}) &&
         CombatRuleset::inAttackRange(CombatPosition{-3000, 0},
                                      CombatPosition{0, 0}) &&
         !CombatRuleset::inAttackRange(CombatPosition{0, 0},
                                       CombatPosition{3001, 0}) &&
         !CombatRuleset::inAttackRange(CombatPosition{-10000, -10000},
                                       CombatPosition{10000, 10000});
}

bool monsterSpawnsFromRulesAndAppliesOnlyFixedDamage() {
  auto monster = Monster::spawn();
  if (monster.id() != CombatRuleset::monsterId ||
      monster.position() != CombatRuleset::spawnPosition ||
      monster.hitPoints() != CombatRuleset::monsterHitPoints ||
      monster.state() != MonsterState::Alive ||
      monster.applyAttack() != MonsterDamageResult::Applied ||
      monster.hitPoints() != 1580 || monster.state() != MonsterState::Alive) {
    return false;
  }

  for (std::uint32_t attack = 0; attack < 78; ++attack) {
    if (monster.applyAttack() != MonsterDamageResult::Applied) {
      return false;
    }
  }
  if (monster.hitPoints() != CombatRuleset::attackDamage ||
      monster.applyAttack() != MonsterDamageResult::Lethal ||
      monster.hitPoints() != 0 || monster.state() != MonsterState::Dying) {
    return false;
  }

  const auto beforeHitPoints = monster.hitPoints();
  const auto beforeState = monster.state();
  return monster.applyAttack() == MonsterDamageResult::NotAlive &&
         monster.hitPoints() == beforeHitPoints &&
         monster.state() == beforeState;
}

} // namespace

int main() {
  if (!rulesetMatchesFrozenV1Contract() ||
      !rangeUsesServerMillimetersAndIncludesBoundary() ||
      !monsterSpawnsFromRulesAndAppliesOnlyFixedDamage()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
