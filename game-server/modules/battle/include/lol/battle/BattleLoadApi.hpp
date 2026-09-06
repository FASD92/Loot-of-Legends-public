#pragma once

#include <lol/battle/BattleAdmission.hpp>
#include <lol/battle/BattleDeterministicState.hpp>
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

// Input ownership is controlled by the RoomExecutionCell while the Battle
// aggregate remains alive during a reconnect grace period. This is not an
// exit transition: the participant remains captured and timers continue.
enum class BattleInputResultCode : std::uint8_t {
  Ok,
  StaleSession,
  StaleBattle,
  NotEligible,
};

struct SuspendBattleInputCommand final {
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
};

struct ResumeBattleInputCommand final {
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
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

struct BattleResumeParticipantProjection final {
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
  std::int32_t posXMillimeter;
  std::int32_t posYMillimeter;
  ParticipantExitStatus exitStatus;
  bool inputEnabled;

  bool operator==(const BattleResumeParticipantProjection &) const = default;
};

struct BattleResumeProjection final {
  shared::RoomId roomId;
  shared::BattleInstanceId battleId;
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
  BattleLoadState battleState;
  std::uint32_t serverTick;
  bool inputEnabled;
  std::vector<BattleResumeParticipantProjection> participants;
  std::optional<CombatProjection> combat;
  LootProjection loot;
  BattleResultProjection result;

  bool operator==(const BattleResumeProjection &) const = default;
};

struct CreateBattleResult;

class BattleInstance final {
public:
  [[nodiscard]] static CreateBattleResult
  create(BattleAdmissionSnapshot admission);

  [[nodiscard]] BattleLoadResultCode openLoadBarrier();
  [[nodiscard]] BattleLoadResultCode openLoadBarrier(BattleTime at);
  [[nodiscard]] BattleLoadResultCode
  completeLoad(const ArenaLoadCompleteCommand &command,
               bool gameplayTransportReady);
  [[nodiscard]] BattleLoadResultCode
  completeLoad(const ArenaLoadCompleteCommand &command,
               bool gameplayTransportReady, BattleTime at);
  [[nodiscard]] BattleLoadResultCode
  disconnect(const CandidateDisconnectedCommand &command);
  [[nodiscard]] BattleLoadResultCode
  disconnect(const CandidateDisconnectedCommand &command,
             BattleTime completedAt);
  [[nodiscard]] BattleLoadResultCode
  disconnect(const CandidateDisconnectedCommand &command,
             std::chrono::steady_clock::time_point completedAt);
  // Voluntary leave uses the same validated transition as disconnect and
  // freezes the captured participant with ParticipantExitStatus::VoluntaryLeft.
  // Before gameplay commit it is a no-op: only the room membership removal
  // applies, matching the pre-Task-3 leave behavior.
  [[nodiscard]] BattleLoadResultCode
  leave(const CandidateDisconnectedCommand &command);
  [[nodiscard]] BattleLoadResultCode
  leave(const CandidateDisconnectedCommand &command, BattleTime completedAt);
  [[nodiscard]] BattleLoadResultCode
  leave(const CandidateDisconnectedCommand &command,
        std::chrono::steady_clock::time_point completedAt);
  [[nodiscard]] BattleLoadResultCode
  expireLoadBarrier(const LoadBarrierDeadlineCommand &command);
  [[nodiscard]] BattleLoadResultCode
  expireLoadBarrier(const LoadBarrierDeadlineCommand &command, BattleTime at);
  [[nodiscard]] BattleInputResultCode
  suspendInput(shared::SessionId sessionId,
               shared::SessionGeneration generation);
  [[nodiscard]] BattleInputResultCode
  suspendInput(shared::SessionId sessionId,
               shared::SessionGeneration generation, BattleTime at);
  [[nodiscard]] BattleInputResultCode
  resumeInput(shared::SessionId sessionId,
              shared::SessionGeneration generation);
  [[nodiscard]] BattleInputResultCode
  resumeInput(shared::SessionId sessionId, shared::SessionGeneration generation,
              BattleTime at);
  [[nodiscard]] std::optional<BattleResumeProjection>
  resumeProjection(shared::SessionId sessionId,
                   shared::SessionGeneration generation) const;
  [[nodiscard]] BattleLoadProjection projection() const;
  [[nodiscard]] MovementResultCode acceptMove(const MoveCommand &command);
  [[nodiscard]] MovementResultCode acceptMove(const MoveCommand &command,
                                              BattleTime receivedAt);
  [[nodiscard]] MovementResultCode
  acceptMove(const MoveCommand &command,
             std::chrono::steady_clock::time_point receivedAt);
  [[nodiscard]] MovementResultCode
  integrateMovement(const MovementTickCommand &command, BattleTime at);
  [[nodiscard]] MovementResultCode
  integrateMovement(const MovementTickCommand &command);
  [[nodiscard]] MovementProjection movementProjection() const;
  [[nodiscard]] std::optional<StateSnapshotProjection> captureStateSnapshot();
  [[nodiscard]] AttackTerminalResult attack(const AttackCommand &command);
  [[nodiscard]] AttackTerminalResult attack(const AttackCommand &command,
                                            BattleTime receivedAt);
  [[nodiscard]] AttackTerminalResult
  attack(const AttackCommand &command,
         std::chrono::steady_clock::time_point receivedAt);
  [[nodiscard]] AttackExecution attackWithApplied(const AttackCommand &command);
  [[nodiscard]] AttackExecution attackWithApplied(const AttackCommand &command,
                                                  BattleTime receivedAt);
  [[nodiscard]] AttackExecution
  attackWithApplied(const AttackCommand &command,
                    std::chrono::steady_clock::time_point receivedAt);
  [[nodiscard]] CombatDeadlineResultCode
  expireCombat(const CombatDeadlineCommand &command);
  [[nodiscard]] CombatDeadlineResultCode
  expireCombat(const CombatDeadlineCommand &command, BattleTime completedAt);
  [[nodiscard]] CombatDeadlineResultCode
  expireCombat(const CombatDeadlineCommand &command,
               std::chrono::steady_clock::time_point completedAt);
  [[nodiscard]] std::optional<CombatProjection> combatProjection() const;
  [[nodiscard]] ClaimLootTerminalResult
  claimLoot(const ClaimLootCommand &command);
  [[nodiscard]] ClaimLootTerminalResult
  claimLoot(const ClaimLootCommand &command, BattleTime receivedAt);
  [[nodiscard]] ClaimLootTerminalResult
  claimLoot(const ClaimLootCommand &command,
            std::chrono::steady_clock::time_point receivedAt);
  // 15-second Drop resolution deadline. Every still-Available Drop becomes
  // Unclaimed exactly once and resolution becomes Resolved; a deadline after
  // early all-claimed resolution or a stale battle is explicit no mutation.
  [[nodiscard]] LootDeadlineResultCode
  expireLoot(const LootDeadlineCommand &command);
  [[nodiscard]] LootDeadlineResultCode
  expireLoot(const LootDeadlineCommand &command, BattleTime completedAt);
  [[nodiscard]] LootDeadlineResultCode
  expireLoot(const LootDeadlineCommand &command,
             std::chrono::steady_clock::time_point completedAt);
  [[nodiscard]] LootProjection lootProjection() const;
  // Battle-owned immutable final result. NotReady until the terminal commit
  // condition is reached; once Committed or ResultGenerationFailed, later
  // commands and reads cannot rebuild or mutate the committed value.
  [[nodiscard]] BattleResultProjection resultProjection() const;
  [[nodiscard]] std::optional<RetainedLootResults> retainedLootResults() const;
  [[nodiscard]] BattleTime battleTime() const noexcept { return battleTime_; }
  [[nodiscard]] BattleDeterministicState exportDeterministicState() const;
  [[nodiscard]] BattleStateImportResultCode
  importDeterministicState(const BattleDeterministicState &state);

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
    ParticipantSlot slot;
    shared::SessionId sessionId;
    shared::SessionGeneration generation;
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
    AttackResultStore attackResults;
    LootResultStore lootResults;
    bool gameplayEligible;
    bool inputEnabled{true};
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
               ParticipantExitStatus exitStatus, BattleTime completedAt);
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
  void commitResultIfReady(BattleTime completedAt);
  void markLootResultsCompleted(BattleTime completedAt);

  [[nodiscard]] BattleTime battleTimeFromLiveClock(
      std::chrono::steady_clock::time_point at) const noexcept;
  void observeBattleTime(BattleTime at) noexcept;
  [[nodiscard]] ParticipantSlot
  slotForSession(shared::SessionId sessionId) const noexcept;
  [[nodiscard]] std::optional<shared::SessionId>
  sessionForSlot(ParticipantSlot slot) const noexcept;

  shared::RoomId roomId_;
  shared::BattleInstanceId battleId_;
  BattleRulesetVersion rulesetVersion_;
  BattleSeed seed_;
  BattleTime battleTime_{};
  BattleLoadState state_{BattleLoadState::Created};
  std::vector<CandidateRecord> candidates_;
  std::vector<CapturedParticipant> capturedParticipants_;
  std::vector<ParticipantRecord> participantRecords_;
  std::optional<std::uint64_t> loadDeadlineTick_;
  std::optional<std::uint64_t> combatDeadlineTick_;
  std::optional<std::uint64_t> lootDeadlineTick_;
  std::optional<std::uint32_t> lastIntegratedServerTick_;
  std::uint32_t nextSnapshotSequence_{1};
  std::uint32_t nextAttackAppliedSequence_{1};
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
  std::optional<std::uint64_t> lootResultsCompletedAtNanos_;
};

struct CreateBattleResult final {
  BattleLoadResultCode code;
  std::optional<BattleInstance> battle;
};

} // namespace lol::battle
