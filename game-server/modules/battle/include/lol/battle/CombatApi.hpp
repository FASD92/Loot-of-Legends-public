#pragma once

#include <lol/shared/Identifiers.hpp>

#include <cstdint>
#include <optional>

namespace lol::battle {

struct CommandId final {
  std::uint64_t high;
  std::uint64_t low;

  bool operator==(const CommandId &) const = default;
};

struct EventId final {
  std::uint64_t high;
  std::uint64_t low;

  bool operator==(const EventId &) const = default;
};

enum class AttackResultCode : std::uint16_t {
  Ok = 0,
  NotEligible = 1,
  StaleSession = 2,
  StaleBattle = 3,
  InvalidTarget = 4,
  OutOfRange = 5,
  Cooldown = 6,
  Overloaded = 7,
  CommandConflict = 8,
  TerminalAlreadyDecided = 9,
};

enum class CombatOutcome : std::uint8_t {
  None = 0,
  MonsterDefeated = 1,
  CombatTimeout = 2,
};

struct CombatDeadlineCommand final {
  shared::BattleInstanceId battleId;
};

enum class CombatDeadlineResultCode : std::uint8_t {
  Ok,
  NotEligible,
  StaleBattle,
  TerminalAlreadyDecided,
};

struct AttackCommand final {
  CommandId commandId;
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
  shared::BattleInstanceId battleId;
  std::uint64_t targetHint;
};

struct AttackTerminalResult final {
  CommandId commandId;
  shared::BattleInstanceId battleId;
  AttackResultCode code;
  std::uint64_t monsterId;
  std::uint32_t remainingHitPoints;
  std::uint16_t rulesetVersion;
  CombatOutcome outcome;

  bool operator==(const AttackTerminalResult &) const = default;
};

struct CombatPosition final {
  std::int32_t xMillimeter;
  std::int32_t yMillimeter;

  bool operator==(const CombatPosition &) const = default;
};

struct CombatRuleset final {
  static constexpr std::uint16_t version = 1;
  static constexpr std::uint32_t monsterCount = 1;
  static constexpr std::uint64_t monsterId = 1;
  static constexpr CombatPosition spawnPosition{0, 0};
  static constexpr std::uint32_t monsterHitPoints = 1600;
  static constexpr std::uint32_t attackDamage = 20;
  static constexpr std::int32_t attackRangeMillimeters = 3000;
  static constexpr std::uint32_t perPlayerCooldownMillis = 750;
  static constexpr std::uint32_t combatDeadlineMillis = 30000;
  static constexpr std::uint32_t attackRatePerSecond = 8;
  static constexpr std::uint32_t attackBurst = 4;

  [[nodiscard]] static bool inAttackRange(CombatPosition attacker,
                                          CombatPosition target) noexcept;
};

enum class MonsterState : std::uint8_t {
  Alive = 0,
  Dying = 1,
  Dead = 2,
  TimedOut = 3,
};

struct CombatTerminalRecord final {
  EventId eventId;
  shared::BattleInstanceId battleId;
  std::uint32_t eventSequence;
  CombatOutcome outcome;
  std::uint64_t monsterId;
  std::uint32_t serverTick;
  std::uint16_t rulesetVersion;

  bool operator==(const CombatTerminalRecord &) const = default;
};

struct CombatProjection final {
  shared::BattleInstanceId battleId;
  std::uint64_t monsterId;
  std::uint32_t hitPoints;
  MonsterState monsterState;
  CombatOutcome outcome;
  std::optional<CombatTerminalRecord> terminal;
  std::uint32_t serverTick;

  bool operator==(const CombatProjection &) const = default;
};

enum class MonsterDamageResult : std::uint8_t {
  Applied,
  Lethal,
  NotAlive,
};

class Monster final {
public:
  [[nodiscard]] static Monster spawn() noexcept;

  [[nodiscard]] std::uint64_t id() const noexcept { return id_; }
  [[nodiscard]] CombatPosition position() const noexcept { return position_; }
  [[nodiscard]] std::uint32_t hitPoints() const noexcept { return hitPoints_; }
  [[nodiscard]] MonsterState state() const noexcept { return state_; }
  [[nodiscard]] MonsterDamageResult applyAttack() noexcept;
  [[nodiscard]] bool completeDeath() noexcept;
  [[nodiscard]] bool timeout() noexcept;

private:
  Monster(std::uint64_t id, CombatPosition position,
          std::uint32_t hitPoints) noexcept;

  std::uint64_t id_;
  CombatPosition position_;
  std::uint32_t hitPoints_;
  MonsterState state_{MonsterState::Alive};
};

} // namespace lol::battle
