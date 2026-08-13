#pragma once

#include <lol/battle/BattleAdmission.hpp>
#include <lol/battle/BattleProjections.hpp>
#include <lol/battle/BattleResult.hpp>
#include <lol/battle/CombatResultStore.hpp>
#include <lol/battle/LootApi.hpp>
#include <lol/battle/LootResultStore.hpp>
#include <lol/battle/MovementApi.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace lol::battle {

enum class BattleLoadResultCode : std::uint16_t {
  Ok = 0,
  InvalidArgument = 1,
  RoomNotFound = 2,
  RoomNotOpen = 3,
  NotInRoom = 4,
  NotHost = 5,
  NotEnoughPlayers = 6,
  NotAllReady = 7,
  StartGateClosed = 8,
  StaleSession = 9,
  StaleBattle = 10,
  NotEligible = 11,
  Overloaded = 12,
};

struct ArenaLoadCompleteCommand final {
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
  shared::RoomId roomId;
  shared::BattleInstanceId battleId;
};

struct CandidateDisconnectedCommand final {
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
  shared::RoomId roomId;
  shared::BattleInstanceId battleId;
};

struct LoadBarrierDeadlineCommand final {
  shared::RoomId roomId;
  shared::BattleInstanceId battleId;
};

struct CreateBattleResult;

class BattleInstance final {
public:
  [[nodiscard]] static CreateBattleResult
  create(BattleAdmissionSnapshot admission);

  [[nodiscard]] BattleLoadResultCode openLoadBarrier();
  [[nodiscard]] BattleLoadResultCode
  completeLoad(const ArenaLoadCompleteCommand &command,
               bool gameplayTransportReady);
  [[nodiscard]] BattleLoadResultCode
  disconnect(const CandidateDisconnectedCommand &command,
             std::chrono::steady_clock::time_point completedAt =
                 std::chrono::steady_clock::now());
  // Voluntary leave uses the same validated transition as disconnect and
  // freezes the captured participant with ParticipantExitStatus::VoluntaryLeft.
  // Before gameplay commit it is a no-op: only the room membership removal
  // applies, matching the pre-Task-3 leave behavior.
  [[nodiscard]] BattleLoadResultCode
  leave(const CandidateDisconnectedCommand &command,
        std::chrono::steady_clock::time_point completedAt =
            std::chrono::steady_clock::now());
  [[nodiscard]] BattleLoadResultCode
  expireLoadBarrier(const LoadBarrierDeadlineCommand &command);
  [[nodiscard]] BattleLoadProjection projection() const;
  [[nodiscard]] MovementResultCode
  acceptMove(const MoveCommand &command,
             std::chrono::steady_clock::time_point receivedAt);
  [[nodiscard]] MovementResultCode
  integrateMovement(const MovementTickCommand &command);
  [[nodiscard]] MovementProjection movementProjection() const;
  [[nodiscard]] std::optional<StateSnapshotProjection> captureStateSnapshot();
  [[nodiscard]] AttackTerminalResult
  attack(const AttackCommand &command,
         std::chrono::steady_clock::time_point receivedAt);
  [[nodiscard]] CombatDeadlineResultCode
  expireCombat(const CombatDeadlineCommand &command,
               std::chrono::steady_clock::time_point completedAt);
  [[nodiscard]] std::optional<CombatProjection> combatProjection() const;
  [[nodiscard]] ClaimLootTerminalResult
  claimLoot(const ClaimLootCommand &command,
            std::chrono::steady_clock::time_point receivedAt);
  // 15-second Drop resolution deadline. Every still-Available Drop becomes
  // Unclaimed exactly once and resolution becomes Resolved; a deadline after
  // early all-claimed resolution or a stale battle is explicit no mutation.
  [[nodiscard]] LootDeadlineResultCode
  expireLoot(const LootDeadlineCommand &command,
             std::chrono::steady_clock::time_point completedAt =
                 std::chrono::steady_clock::now());
  [[nodiscard]] LootProjection lootProjection() const;
  // Battle-owned immutable final result. NotReady until the terminal commit
  // condition is reached; once Committed or ResultGenerationFailed, later
  // commands and reads cannot rebuild or mutate the committed value.
  [[nodiscard]] BattleResultProjection resultProjection() const;
  [[nodiscard]] std::optional<RetainedLootResults> retainedLootResults() const;

private:
  struct CandidateRecord final {
    BattleStartCandidate identity;
    LoadCandidateState state;
  };

  // A generated RelicDrop stays the immutable Task 1 definition; only the
  // claimable state and owner are mutable Battle-private state.
  struct LootDropRecord final {
    RelicDrop drop;
    LootDropState state{LootDropState::Available};
    std::optional<shared::SessionId> owner;
  };

  struct HoldingRecord final {
    shared::SessionId sessionId;
    ItemId itemId;
    std::uint64_t quantity;
  };

  static constexpr std::uint64_t claimRateTokenUnits = 1'000'000'000ULL;
  static constexpr std::uint64_t claimRateCapacityUnits =
      static_cast<std::uint64_t>(RelicRuleset::claimBurst) *
      claimRateTokenUnits;
  static constexpr std::uint64_t claimRateUnitsPerNanosecond =
      RelicRuleset::claimRatePerSecond;
  static constexpr auto fullClaimRateRefill = std::chrono::nanoseconds{
      claimRateCapacityUnits / claimRateUnitsPerNanosecond};

  struct ParticipantRecord final {
    shared::SessionId sessionId;
    shared::SessionGeneration generation;
    std::int32_t posXMillimeter;
    std::int32_t posYMillimeter;
    std::int32_t tickDeltaXMillimeter;
    std::int32_t tickDeltaYMillimeter;
    std::optional<std::uint32_t> latestSeenActionSequence;
    std::optional<std::chrono::steady_clock::time_point> lastRateUpdate;
    std::uint64_t rateCreditUnits;
    std::optional<std::chrono::steady_clock::time_point> lastAttackRateUpdate;
    std::uint64_t attackRateCreditUnits;
    std::optional<std::chrono::steady_clock::time_point> lastClaimRateUpdate;
    std::uint64_t claimRateCreditUnits;
    std::optional<std::chrono::steady_clock::time_point> lastAcceptedAttackAt;
    AttackResultStore attackResults;
    LootResultStore lootResults;
    bool gameplayEligible;
    // Current exit status; GameplayEligible until the first exit. Sticky.
    ParticipantExitStatus exitStatus{ParticipantExitStatus::GameplayEligible};
  };

  explicit BattleInstance(BattleAdmissionSnapshot admission);

  [[nodiscard]] BattleLoadResultCode
  correlate(shared::RoomId roomId, shared::BattleInstanceId battleId,
            shared::SessionId sessionId, shared::SessionGeneration generation,
            CandidateRecord *&candidate);
  [[nodiscard]] BattleLoadResultCode
  freezeExited(const CandidateDisconnectedCommand &command,
               ParticipantExitStatus exitStatus,
               std::chrono::steady_clock::time_point completedAt);
  [[nodiscard]] ParticipantRecord *
  participantRecord(shared::SessionId sessionId) noexcept;
  void resolveIfComplete();
  // Exactly-once Battle cancellation transition: when the first sticky exit
  // leaves zero GameplayEligible captured participants, sets the Battle
  // terminal outcome to CancelledNoActiveParticipants without creating a
  // combat terminal record or touching historical combat, and closes any Open
  // loot source. A prior Battle terminal outcome or a committed/failed result
  // makes later exits no mutation.
  void applyCancellationIfNoActiveParticipants();
  // Commits the immutable result exactly when the terminal commit condition is
  // reached: immediately for CombatTimeout, and only after loot resolution
  // becomes Resolved for MonsterDefeated. An already terminal result state
  // makes later calls no mutation.
  void commitResultIfReady(std::chrono::steady_clock::time_point completedAt);
  void
  markLootResultsCompleted(std::chrono::steady_clock::time_point completedAt);

  shared::RoomId roomId_;
  shared::BattleInstanceId battleId_;
  BattleLoadState state_{BattleLoadState::Created};
  std::vector<CandidateRecord> candidates_;
  std::vector<CapturedParticipant> capturedParticipants_;
  std::vector<ParticipantRecord> participantRecords_;
  std::optional<std::uint32_t> lastIntegratedServerTick_;
  std::uint32_t nextSnapshotSequence_{1};
  std::optional<Monster> monster_;
  std::optional<CombatTerminalRecord> combatTerminal_;
  // Battle-owned terminal outcome source; set exactly once (first-terminal-
  // wins) on result commit or on all-active-participants-exited cancellation.
  std::optional<BattleOutcome> battleTerminalOutcome_;
  std::vector<LootDropRecord> drops_;
  std::vector<HoldingRecord> holdings_;
  LootResolutionState lootResolution_{LootResolutionState::NotStarted};
  BattleResultState resultState_{BattleResultState::NotReady};
  std::optional<BattleFinalResult> committedResult_;
  std::optional<std::chrono::steady_clock::time_point> lootResultsCompletedAt_;
};

struct CreateBattleResult final {
  BattleLoadResultCode code;
  std::optional<BattleInstance> battle;
};

} // namespace lol::battle
