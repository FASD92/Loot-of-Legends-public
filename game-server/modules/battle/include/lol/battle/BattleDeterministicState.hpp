#pragma once

#include <lol/battle/BattleAdmission.hpp>
#include <lol/battle/BattleResult.hpp>
#include <lol/battle/BattleTime.hpp>
#include <lol/battle/CombatResultStore.hpp>
#include <lol/battle/LootResultStore.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace lol::battle {

using ParticipantSlot = std::uint16_t;
inline constexpr ParticipantSlot systemParticipantSlot = 0;

struct BattleDeterministicCandidateState final {
  ParticipantSlot slot;
  shared::SessionId sessionId;
  LoadCandidateState state;

  bool operator==(const BattleDeterministicCandidateState &) const = default;
};

struct BattleDeterministicCapturedParticipantState final {
  ParticipantSlot slot;
  shared::SessionId sessionId;
  ParticipantExitStatus exitStatus;

  bool operator==(const BattleDeterministicCapturedParticipantState &) const =
      default;
};

struct BattleDeterministicParticipantState final {
  ParticipantSlot slot;
  shared::SessionId sessionId;
  std::int32_t posXMillimeter;
  std::int32_t posYMillimeter;
  std::int32_t tickDeltaXMillimeter;
  std::int32_t tickDeltaYMillimeter;
  std::optional<std::uint32_t> latestSeenActionSequence;
  std::optional<std::uint64_t> lastRateUpdateNanos;
  std::uint64_t rateCreditUnits;
  std::optional<std::uint64_t> lastAttackRateUpdateNanos;
  std::uint64_t attackRateCreditUnits;
  std::optional<std::uint64_t> lastClaimRateUpdateNanos;
  std::uint64_t claimRateCreditUnits;
  std::optional<std::uint64_t> lastAcceptedAttackNanos;
  AttackResultStoreState attackResults;
  LootResultStoreState lootResults;
  bool gameplayEligible;
  bool inputEnabled;
  ParticipantExitStatus exitStatus;

  bool operator==(const BattleDeterministicParticipantState &) const = default;
};

struct BattleDeterministicMonsterState final {
  std::uint64_t id;
  CombatPosition position;
  std::uint32_t hitPoints;
  MonsterState state;

  bool operator==(const BattleDeterministicMonsterState &) const = default;
};

struct BattleDeterministicDropState final {
  RelicDrop drop;
  LootDropState state;
  std::optional<ParticipantSlot> ownerSlot;

  bool operator==(const BattleDeterministicDropState &) const = default;
};

struct BattleDeterministicHoldingState final {
  ParticipantSlot ownerSlot;
  ItemId itemId;
  std::uint64_t quantity;

  bool operator==(const BattleDeterministicHoldingState &) const = default;
};

struct BattleDeterministicResultEntry final {
  ParticipantSlot slot;
  ParticipantExitStatus exitStatus;
  std::uint64_t finalAssetValue;
  std::optional<std::uint32_t> rank;
  bool isTop;

  bool operator==(const BattleDeterministicResultEntry &) const = default;
};

struct BattleDeterministicFinalResult final {
  BattleOutcome outcome;
  std::vector<BattleDeterministicResultEntry> entries;

  bool operator==(const BattleDeterministicFinalResult &) const = default;
};

// Battle-owned value image. It intentionally contains neither AccountId,
// nickname, nor SessionGeneration. Those belong to the private participant
// mapping supplied to importDeterministicState.
struct BattleDeterministicState final {
  shared::RoomId roomId;
  shared::BattleInstanceId battleId;
  BattleRulesetVersion rulesetVersion;
  BattleSeed seed;
  BattleTime battleTime;
  BattleLoadState state;
  std::vector<BattleDeterministicCandidateState> candidates;
  std::vector<BattleDeterministicCapturedParticipantState> capturedParticipants;
  std::vector<BattleDeterministicParticipantState> participants;
  std::optional<std::uint64_t> loadDeadlineTick;
  std::optional<std::uint64_t> combatDeadlineTick;
  std::optional<std::uint64_t> lootDeadlineTick;
  std::optional<std::uint32_t> lastIntegratedServerTick;
  std::uint32_t nextSnapshotSequence;
  std::uint32_t nextAttackAppliedSequence;
  std::optional<BattleDeterministicMonsterState> monster;
  std::optional<CombatTerminalRecord> combatTerminal;
  std::optional<BattleOutcome> battleTerminalOutcome;
  std::vector<BattleDeterministicDropState> drops;
  std::vector<BattleDeterministicHoldingState> holdings;
  LootResolutionState lootResolution;
  BattleResultState resultState;
  std::optional<BattleDeterministicFinalResult> committedResult;
  std::optional<std::uint64_t> lootResultsCompletedAtNanos;

  bool operator==(const BattleDeterministicState &) const = default;
};

enum class BattleStateImportResultCode : std::uint8_t {
  Ok,
  InvalidArgument,
  IdentityMismatch,
  RulesetMismatch,
  InvariantBroken,
};

} // namespace lol::battle
