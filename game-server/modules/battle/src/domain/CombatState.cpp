#include <lol/battle/CombatApi.hpp>

#include <cstdint>

namespace lol::battle {

bool CombatRuleset::inAttackRange(CombatPosition attacker,
                                  CombatPosition target) noexcept {
  const auto deltaX = static_cast<std::int64_t>(attacker.xMillimeter) -
                      static_cast<std::int64_t>(target.xMillimeter);
  const auto deltaY = static_cast<std::int64_t>(attacker.yMillimeter) -
                      static_cast<std::int64_t>(target.yMillimeter);
  const auto range = static_cast<std::int64_t>(attackRangeMillimeters);
  if (deltaX < -range || deltaX > range || deltaY < -range || deltaY > range) {
    return false;
  }
  return (deltaX * deltaX) + (deltaY * deltaY) <= range * range;
}

Monster Monster::spawn() noexcept {
  return Monster{CombatRuleset::monsterId, CombatRuleset::spawnPosition,
                 CombatRuleset::monsterHitPoints};
}

Monster::Monster(std::uint64_t id, CombatPosition position,
                 std::uint32_t hitPoints) noexcept
    : id_(id), position_(position), hitPoints_(hitPoints) {}

MonsterDamageResult Monster::applyAttack() noexcept {
  if (state_ != MonsterState::Alive) {
    return MonsterDamageResult::NotAlive;
  }
  if (hitPoints_ <= CombatRuleset::attackDamage) {
    hitPoints_ = 0;
    state_ = MonsterState::Dying;
    return MonsterDamageResult::Lethal;
  }
  hitPoints_ -= CombatRuleset::attackDamage;
  return MonsterDamageResult::Applied;
}

bool Monster::completeDeath() noexcept {
  if (state_ != MonsterState::Dying) {
    return false;
  }
  state_ = MonsterState::Dead;
  return true;
}

bool Monster::timeout() noexcept {
  if (state_ != MonsterState::Alive) {
    return false;
  }
  state_ = MonsterState::TimedOut;
  return true;
}

} // namespace lol::battle
