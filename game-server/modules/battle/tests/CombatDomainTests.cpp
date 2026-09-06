#include <lol/battle/CombatApi.hpp>

#include <cstdint>
#include <cstdlib>

namespace {

using lol::battle::CombatPosition;
using lol::battle::CombatRuleset;
using lol::battle::Monster;
using lol::battle::MonsterDamageResult;
using lol::battle::MonsterState;

bool rulesetMatchesParticipantScaledV4Contract() {
  return CombatRuleset::version == 4 && CombatRuleset::monsterCount == 1 &&
         CombatRuleset::monsterId == 1 &&
         CombatRuleset::spawnPosition == CombatPosition{0, 0} &&
         CombatRuleset::monsterHitPointsForParticipants(2) == 1600 &&
         CombatRuleset::monsterHitPointsForParticipants(10) == 8000 &&
         CombatRuleset::attackDamage == 100 &&
         CombatRuleset::attackRangeMillimeters == 12000 &&
         CombatRuleset::perPlayerCooldownMillis == 750 &&
         CombatRuleset::combatDeadlineMillis == 30000 &&
         CombatRuleset::attackRatePerSecond == 8 &&
         CombatRuleset::attackBurst == 4;
}

bool rangeUsesServerMillimetersAndIncludesBoundary() {
  return CombatRuleset::inAttackRange(CombatPosition{0, 0},
                                      CombatPosition{12000, 0}) &&
         CombatRuleset::inAttackRange(CombatPosition{-12000, 0},
                                      CombatPosition{0, 0}) &&
         !CombatRuleset::inAttackRange(CombatPosition{0, 0},
                                       CombatPosition{12001, 0}) &&
         !CombatRuleset::inAttackRange(CombatPosition{-10000, -10000},
                                       CombatPosition{10000, 10000});
}

bool monsterSpawnsFromCommittedParticipantCountAndAppliesFixedDamage() {
  auto monster =
      Monster::spawn(CombatRuleset::monsterHitPointsForParticipants(2));
  if (monster.id() != CombatRuleset::monsterId ||
      monster.position() != CombatRuleset::spawnPosition ||
      monster.hitPoints() != 1600 || monster.state() != MonsterState::Alive ||
      monster.applyAttack() != MonsterDamageResult::Applied ||
      monster.hitPoints() != 1500 || monster.state() != MonsterState::Alive) {
    return false;
  }

  for (std::uint32_t attack = 0; attack < 14; ++attack) {
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

bool tenParticipantMonsterUsesTheSameFixedDamage() {
  auto monster =
      Monster::spawn(CombatRuleset::monsterHitPointsForParticipants(10));
  return monster.hitPoints() == 8000 &&
         monster.applyAttack() == MonsterDamageResult::Applied &&
         monster.hitPoints() == 7900;
}

} // namespace

int main() {
  if (!rulesetMatchesParticipantScaledV4Contract() ||
      !rangeUsesServerMillimetersAndIncludesBoundary() ||
      !monsterSpawnsFromCommittedParticipantCountAndAppliesFixedDamage() ||
      !tenParticipantMonsterUsesTheSameFixedDamage()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
