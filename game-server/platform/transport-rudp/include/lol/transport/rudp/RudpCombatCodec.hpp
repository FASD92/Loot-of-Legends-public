#pragma once

#include <lol/transport/rudp/RudpHeader.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace lol::transport::rudp {

struct RudpCommandId final {
  std::uint64_t high;
  std::uint64_t low;

  bool operator==(const RudpCommandId &) const = default;
};

struct RudpEventId final {
  std::uint64_t high;
  std::uint64_t low;

  bool operator==(const RudpEventId &) const = default;
};

enum class RudpAttackResultCode : std::uint16_t {
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

enum class RudpCombatOutcome : std::uint8_t {
  None = 0,
  MonsterDefeated = 1,
  CombatTimeout = 2,
};

enum class RudpMonsterState : std::uint8_t {
  Alive = 0,
  Dying = 1,
  Dead = 2,
  TimedOut = 3,
};

enum class RudpEventStreamKind : std::uint8_t {
  CombatLifecycle = 1,
  LootLifecycle = 2,
};

struct RudpAttackIntent final {
  RudpCommandId commandId;
  std::uint64_t battleInstanceId;
  std::uint64_t targetHint;

  bool operator==(const RudpAttackIntent &) const = default;
};

struct RudpAttackTerminalResult final {
  RudpCommandId commandId;
  std::uint64_t battleInstanceId;
  RudpAttackResultCode resultCode;
  std::uint64_t monsterId;
  std::uint32_t remainingHitPoints;
  std::uint16_t rulesetVersion;
  RudpCombatOutcome combatOutcome;

  bool operator==(const RudpAttackTerminalResult &) const = default;
};

struct RudpMonsterSpawned final {
  RudpEventId eventId;
  std::uint64_t battleInstanceId;
  RudpEventStreamKind eventStreamKind;
  std::uint32_t eventSequence;
  std::uint64_t monsterId;
  std::int32_t posXMillimeter;
  std::int32_t posYMillimeter;
  std::uint32_t maximumHitPoints;
  std::uint16_t rulesetVersion;

  bool operator==(const RudpMonsterSpawned &) const = default;
};

struct RudpCombatTerminalEvent final {
  RudpEventId eventId;
  std::uint64_t battleInstanceId;
  RudpEventStreamKind eventStreamKind;
  std::uint32_t eventSequence;
  RudpCombatOutcome combatOutcome;
  std::uint64_t monsterId;
  std::uint32_t serverTick;
  std::uint16_t rulesetVersion;

  bool operator==(const RudpCombatTerminalEvent &) const = default;
};

struct RudpMonsterStateSnapshot final {
  std::uint64_t battleInstanceId;
  std::uint32_t snapshotSequence;
  std::uint32_t serverTick;
  std::uint64_t monsterId;
  std::uint32_t hitPoints;
  RudpMonsterState monsterState;

  bool operator==(const RudpMonsterStateSnapshot &) const = default;
};

using RudpCombatMessage =
    std::variant<RudpAttackIntent, RudpAttackTerminalResult, RudpMonsterSpawned,
                 RudpCombatTerminalEvent, RudpMonsterStateSnapshot>;

enum class RudpCombatCodecError : std::uint8_t {
  None,
  Header,
  UnsupportedMessage,
  MalformedPayload,
};

struct DecodedRudpCombat final {
  RudpCombatCodecError error{RudpCombatCodecError::Header};
  std::optional<RudpHeader> header;
  std::optional<RudpCombatMessage> message;
};

class RudpCombatCodec final {
public:
  [[nodiscard]] static std::optional<std::vector<std::byte>>
  encode(const RudpHeader &header, const RudpCombatMessage &message);
  [[nodiscard]] static DecodedRudpCombat
  decode(std::span<const std::byte> datagram);
};

} // namespace lol::transport::rudp
