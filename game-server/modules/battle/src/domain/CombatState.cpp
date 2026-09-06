#include <lol/battle/CombatApi.hpp>

#include <array>
#include <cstdint>

namespace lol::battle {

namespace {

// This table is the rounded v1 replacement for the old floating-point circle
// calculation. The index is participantCount - 2, then participant slot - 1.
constexpr std::array<std::array<CombatPosition, 10>, 9>
    participantSpawnPositions{{
        {{{2500, 0},
          {-2500, 0},
          {0, 0},
          {0, 0},
          {0, 0},
          {0, 0},
          {0, 0},
          {0, 0},
          {0, 0},
          {0, 0}}},
        {{{2500, 0},
          {-1250, 2165},
          {-1250, -2165},
          {0, 0},
          {0, 0},
          {0, 0},
          {0, 0},
          {0, 0},
          {0, 0},
          {0, 0}}},
        {{{2500, 0},
          {0, 2500},
          {-2500, 0},
          {0, -2500},
          {0, 0},
          {0, 0},
          {0, 0},
          {0, 0},
          {0, 0},
          {0, 0}}},
        {{{2500, 0},
          {773, 2378},
          {-2023, 1469},
          {-2023, -1469},
          {773, -2378},
          {0, 0},
          {0, 0},
          {0, 0},
          {0, 0},
          {0, 0}}},
        {{{2500, 0},
          {1250, 2165},
          {-1250, 2165},
          {-2500, 0},
          {-1250, -2165},
          {1250, -2165},
          {0, 0},
          {0, 0},
          {0, 0},
          {0, 0}}},
        {{{2500, 0},
          {1559, 1955},
          {-556, 2437},
          {-2252, 1085},
          {-2252, -1085},
          {-556, -2437},
          {1559, -1955},
          {0, 0},
          {0, 0},
          {0, 0}}},
        {{{2500, 0},
          {1768, 1768},
          {0, 2500},
          {-1768, 1768},
          {-2500, 0},
          {-1768, -1768},
          {0, -2500},
          {1768, -1768},
          {0, 0},
          {0, 0}}},
        {{{2500, 0},
          {1915, 1607},
          {434, 2462},
          {-1250, 2165},
          {-2349, 855},
          {-2349, -855},
          {-1250, -2165},
          {434, -2462},
          {1915, -1607},
          {0, 0}}},
        {{{2500, 0},
          {2023, 1469},
          {773, 2378},
          {-773, 2378},
          {-2023, 1469},
          {-2500, 0},
          {-2023, -1469},
          {-773, -2378},
          {773, -2378},
          {2023, -1469}}},
    }};

} // namespace

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

CombatPosition CombatRuleset::participantSpawnPosition(
    std::uint32_t participantCount, std::uint32_t participantIndex) noexcept {
  if (participantCount < minimumParticipants ||
      participantCount > maximumParticipants ||
      participantIndex >= participantCount) {
    return CombatPosition{0, 0};
  }
  return participantSpawnPositions[participantCount - minimumParticipants]
                                  [participantIndex];
}

Monster Monster::spawn(std::uint32_t maximumHitPoints) noexcept {
  return Monster{CombatRuleset::monsterId, CombatRuleset::spawnPosition,
                 maximumHitPoints};
}

Monster Monster::restore(std::uint64_t id, CombatPosition position,
                         std::uint32_t hitPoints, MonsterState state) noexcept {
  auto monster = Monster{id, position, hitPoints};
  monster.state_ = state;
  return monster;
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
