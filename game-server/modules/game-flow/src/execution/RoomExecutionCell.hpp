#pragma once

#include <lol/battle/BattleLoadApi.hpp>
#include <lol/battle_continuity/Durability.hpp>
#include <lol/battle_continuity/FlightRecorder.hpp>
#include <lol/game_flow/BattleRecovery.hpp>
#include <lol/game_flow/GameplayTransportReadinessPort.hpp>
#include <lol/lobby_room/RoomApi.hpp>
#include <lol/runtime/DeadlineScheduler.hpp>
#include <lol/runtime/WorkerPool.hpp>
#include <lol/settlement/SettlementCapacityGate.hpp>
#include <lol/settlement/SettlementIntent.hpp>
#include <lol/settlement/SettlementPublication.hpp>
#include <lol/shared/Identifiers.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <variant>
#include <vector>

namespace lol::game_flow::execution {

using RoomCellCommand =
    std::variant<lobby_room::JoinRoomCommand, lobby_room::LeaveRoomCommand,
                 lobby_room::SetReadyCommand, lobby_room::KickRoomMemberCommand,
                 lobby_room::HostStartEligibilityCommand,
                 battle::ArenaLoadCompleteCommand, battle::MoveCommand,
                 battle::AttackCommand, battle::ClaimLootCommand>;

struct RoomCommandEnvelope final {
  std::optional<shared::RequestId> requestId;
  RoomCellCommand command;
  std::chrono::steady_clock::time_point receivedAt{};
};

struct ConfirmedDisconnectCommand final {
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
};

using RoomControlCommand = std::variant<
    ConfirmedDisconnectCommand, battle::SuspendBattleInputCommand,
    battle::ResumeBattleInputCommand, battle::LoadBarrierDeadlineCommand,
    battle::MovementTickCommand, battle::CombatDeadlineCommand,
    battle::LootDeadlineCommand, settlement::DurableAppendCompleted,
    settlement::DurableAppendFailed, battle_continuity::DurableTickCommitted,
    battle_continuity::DurableTickWriteFailed>;

struct RoomControlEnvelope final {
  RoomControlCommand command;
  std::chrono::steady_clock::time_point occurredAt{};
};

enum class CommandLane : std::uint8_t {
  External,
  InternalControl,
};

enum class RoomCommandAdmission : std::uint8_t {
  Accepted,
  RoomRetired,
  RoomOverloaded,
  ControlReserveExhausted,
  SchedulingUnavailable,
};

enum class RoomCommandKind : std::uint8_t {
  Join,
  Leave,
  SetReady,
  Kick,
  HostStartEligibility,
  ArenaLoadComplete,
  Move,
  Attack,
  ClaimLoot,
  ConfirmedDisconnect,
  SuspendBattleInput,
  ResumeBattleInput,
  LoadBarrierDeadline,
  MovementTick,
  CombatDeadline,
  LootDeadline,
  DurableAppendCompleted,
  DurableAppendFailed,
  ContinuityTickCommitted,
  ContinuityTickFailed,
};

struct RoomCommandOutcome final {
  std::uint64_t admissionOrdinal;
  CommandLane lane;
  RoomCommandKind kind;
  lobby_room::RoomLifecycle lifecycle;
  std::optional<shared::RequestId> requestId;
  std::optional<shared::SessionId> actorSessionId;
  std::optional<shared::SessionGeneration> actorGeneration;
  std::optional<shared::SessionId> targetSessionId;
  std::optional<shared::SessionGeneration> targetGeneration;
  lobby_room::RoomResultCode code;
  std::optional<battle::BattleLoadResultCode> battleCode;
  std::optional<battle::BattleInputResultCode> inputControlCode;
  std::optional<battle::MovementResultCode> movementCode;
  std::optional<battle::AttackTerminalResult> attackResult;
  std::optional<battle::AttackAppliedRecord> attackApplied;
  std::optional<battle::CombatDeadlineResultCode> combatDeadlineCode;
  std::optional<battle::ClaimLootTerminalResult> lootClaimResult;
  std::optional<battle::LootDeadlineResultCode> lootDeadlineCode;
  std::optional<battle::LootProjection> loot;
  std::optional<lobby_room::RoomSummary> summary;
  std::optional<lobby_room::RoomDetailProjection> detail;
  std::optional<lobby_room::BattleAdmissionSnapshot> admission;
  std::optional<battle::BattleLoadProjection> battle;
  std::optional<battle::BattleResumeProjection> resumeProjection;
  std::optional<battle::StateSnapshotProjection> snapshot;
  std::optional<battle::CombatProjection> combat;
  std::optional<BattleRecoveryNotice> recoveryNotice;
  std::vector<lobby_room::RoomMemberSnapshot> recoveryParticipants;
  std::optional<battle::BattleFinalResult> finalResult;
  bool gameplayStartCommitted;
  bool lootResolutionOpened;
  bool settlementDurable;
  std::chrono::nanoseconds queueDelay;
  std::chrono::nanoseconds processingDuration;
  std::optional<std::chrono::nanoseconds> criticalTerminalLatency;
};

struct WorkBudget final {
  std::size_t maxCommands;
  std::chrono::milliseconds maxWallTime;
};

struct RoomExecutionMetrics final {
  std::size_t queueDepth;
  std::size_t externalQueueDepth;
  std::size_t controlQueueDepth;
  std::size_t internalReserveHighWatermark;
  std::size_t activeRuns;
  std::size_t maximumConcurrentRuns;
  std::uint64_t processedCommands;
  std::uint64_t processedExternalCommands;
  std::uint64_t processedControlCommands;
  std::uint64_t lastProcessedOrdinal;
  std::uint64_t externalRejections;
  std::uint64_t controlAdmissionFailures;
  std::uint64_t rescheduleCount;
  std::uint64_t schedulingFailures;
};

struct RecoveredRoomExecutionState final {
  lobby_room::Room room;
  battle::BattleInstance battle;
  battle_continuity::BattleRecording recording;
  std::optional<std::uint64_t> nextBattleOrdinal;
  std::optional<settlement::SettlementIntentBatch> settlementBatch;
  std::optional<settlement::SettlementCapacityReservation>
      settlementReservation;
  bool settlementAlreadyDurable{};
};

class RoomExecutionCell final
    : public std::enable_shared_from_this<RoomExecutionCell> {
public:
  using OutcomeSink = std::function<void(RoomCommandOutcome)>;

  [[nodiscard]] static std::shared_ptr<RoomExecutionCell>
  create(runtime::WorkerPool &workers, runtime::DeadlineScheduler &deadlines,
         lobby_room::Room room, WorkBudget budget, OutcomeSink outcomeSink);
  [[nodiscard]] static std::shared_ptr<RoomExecutionCell>
  create(runtime::WorkerPool &workers, runtime::DeadlineScheduler &deadlines,
         lobby_room::Room room, WorkBudget budget, OutcomeSink outcomeSink,
         const GameplayTransportReadinessPort *readiness);
  [[nodiscard]] static std::shared_ptr<RoomExecutionCell>
  create(runtime::WorkerPool &workers, runtime::DeadlineScheduler &deadlines,
         lobby_room::Room room, WorkBudget budget, OutcomeSink outcomeSink,
         const GameplayTransportReadinessPort *readiness,
         settlement::SettlementCapacityGate *capacityGate);
  [[nodiscard]] static std::shared_ptr<RoomExecutionCell>
  create(runtime::WorkerPool &workers, runtime::DeadlineScheduler &deadlines,
         lobby_room::Room room, WorkBudget budget, OutcomeSink outcomeSink,
         const GameplayTransportReadinessPort *readiness,
         settlement::SettlementCapacityGate *capacityGate,
         settlement::SettlementStoragePort *storage,
         std::uint32_t writerRecoveryEpoch = 0U,
         battle_continuity::DurableTickWritePort *continuityStorage = nullptr);
  [[nodiscard]] static std::optional<std::shared_ptr<RoomExecutionCell>>
  createRecovered(runtime::WorkerPool &workers,
                  runtime::DeadlineScheduler &deadlines,
                  RecoveredRoomExecutionState recovered, WorkBudget budget,
                  OutcomeSink outcomeSink,
                  const GameplayTransportReadinessPort *readiness,
                  settlement::SettlementCapacityGate *capacityGate,
                  settlement::SettlementStoragePort *storage,
                  std::uint32_t writerRecoveryEpoch,
                  battle_continuity::DurableTickWritePort *continuityStorage);

  [[nodiscard]] RoomCommandAdmission enqueue(RoomCommandEnvelope command);
  [[nodiscard]] RoomCommandAdmission
  enqueueControl(RoomControlEnvelope command);
  // Linearizes the Active -> Retired transition under the Cell mutex and clears
  // both queues. Performs no callback, scheduling, outcome emission, or
  // directory operation; the RoomExecutionDirectory is the authoritative
  // lifetime owner and calls this immediately before erasing the entry.
  void retire();
  [[nodiscard]] bool waitUntilIdle(std::chrono::milliseconds timeout);
  [[nodiscard]] RoomExecutionMetrics metrics() const;
  [[nodiscard]] std::optional<lobby_room::RoomDetailProjection> detail() const;
  [[nodiscard]] lobby_room::RoomLifecycle lifecycle() const;
  [[nodiscard]] std::optional<settlement::SettlementIntentBatch>
  settlementBatch() const;
  [[nodiscard]] std::optional<std::vector<std::uint8_t>> battleJournal() const;
  [[nodiscard]] bool activateRecovered();

private:
  friend class RoomExecutionDirectory;

  static constexpr std::size_t kExternalQueueCapacity = 224;
  static constexpr std::size_t kControlQueueCapacity = 32;

  using CellEnvelope = std::variant<RoomCommandEnvelope, RoomControlEnvelope>;

  struct QueuedRoomCommand final {
    std::uint64_t admissionOrdinal;
    CommandLane lane;
    bool usesCriticalControlReservation;
    CellEnvelope envelope;
  };

  struct LiveBattleClockAnchor final {
    shared::BattleInstanceId battleId;
    std::chrono::steady_clock::time_point liveTime;
    battle::BattleTime battleTime;
  };

  RoomExecutionCell(runtime::WorkerPool &workers,
                    runtime::DeadlineScheduler &deadlines,
                    lobby_room::Room room, WorkBudget budget,
                    OutcomeSink outcomeSink,
                    const GameplayTransportReadinessPort *readiness,
                    settlement::SettlementCapacityGate *capacityGate,
                    settlement::SettlementStoragePort *storage,
                    std::uint32_t writerRecoveryEpoch,
                    battle_continuity::DurableTickWritePort *continuityStorage);

  [[nodiscard]] bool scheduleLocked();
  void submitPendingAppend();
  void submitPendingContinuityWrite();
  void enqueueContinuityCompletion(
      battle_continuity::DurableTickWriteOutcome outcome);
  void retryContinuityCompletionScheduling();
  void armContinuityCompletionRetryLocked();
  [[nodiscard]] bool armRecoveredDeadlineLocked();
  void queueSettlementAppendLocked();
  [[nodiscard]] std::optional<QueuedRoomCommand> popNextLocked();
  [[nodiscard]] RoomCommandOutcome applyLocked(QueuedRoomCommand command);
  [[nodiscard]] bool
  reserveAndArmLoadDeadlineLocked(std::chrono::milliseconds delay,
                                  RoomControlCommand command,
                                  shared::BattleInstanceId battleId);
  [[nodiscard]] bool
  rearmCriticalDeadlineLocked(std::chrono::milliseconds delay,
                              RoomControlCommand command,
                              shared::BattleInstanceId battleId);
  void enqueueCriticalDeadlineControl(std::uint64_t token,
                                      shared::BattleInstanceId battleId,
                                      RoomControlCommand command);
  void retryCriticalDeadlineScheduling(std::uint64_t token,
                                       shared::BattleInstanceId battleId);
  void armCriticalSchedulingRetryLocked(std::uint64_t token,
                                        shared::BattleInstanceId battleId);
  void cancelQueuedCriticalDeadlineLocked();
  void releaseCriticalDeadlineLocked();
  [[nodiscard]] battle::ClaimLootTerminalResult
  routeRetainedLootLocked(const battle::ClaimLootCommand &command,
                          std::chrono::steady_clock::time_point receivedAt);
  [[nodiscard]] battle::BattleTime
  battleTimeForLocked(std::chrono::steady_clock::time_point liveTime);
  void runTurn() noexcept;

  runtime::WorkerPool &workers_;
  runtime::DeadlineScheduler &deadlines_;
  lobby_room::Room room_;
  std::optional<battle::BattleInstance> battle_;
  std::optional<battle_continuity::BattleRecording> battleRecording_;
  std::optional<LiveBattleClockAnchor> battleClockAnchor_;
  std::optional<std::chrono::steady_clock::time_point> battleCompletedLiveAt_;
  std::optional<settlement::SettlementIntentBatch> settlementBatch_;
  std::deque<battle::RetainedLootResults> retainedLootResults_;
  std::optional<settlement::DurableAppendRequest> pendingAppendRequest_;
  std::optional<battle_continuity::DurableTickWriteRequest>
      pendingContinuityWrite_;
  std::optional<battle_continuity::DurableTickCommitted>
      pendingContinuityReceipt_;
  std::optional<RoomCommandOutcome> heldContinuityOutcome_;
  std::optional<RoomCommandOutcome> releasedContinuityOutcome_;
  std::optional<QueuedRoomCommand> continuityCompletion_;
  std::optional<settlement::SettlementCapacityReservation>
      settlementReservation_;
  std::uint64_t nextBattleOrdinal_{1};
  const WorkBudget budget_;
  OutcomeSink outcomeSink_;
  const GameplayTransportReadinessPort *readiness_;
  std::optional<settlement::SettlementCapacityGate> capacityGate_;
  settlement::SettlementStoragePort *storage_;
  const std::uint32_t writerRecoveryEpoch_;
  battle_continuity::DurableTickWritePort *continuityStorage_;
  enum class SettlementAppendState : std::uint8_t {
    None,
    Appending,
    RetryableStorageFailure,
  };
  SettlementAppendState settlementAppendState_{SettlementAppendState::None};
  std::optional<shared::BattleInstanceId> emittedResultFailureBattle_;
  bool settlementRecoveryNoticeEmitted_{false};
  bool continuityDurabilityPending_{false};
  bool continuityRecordingFailed_{false};
  mutable std::mutex mutex_;
  std::condition_variable idle_;
  std::deque<QueuedRoomCommand> externalQueue_;
  std::deque<QueuedRoomCommand> controlQueue_;
  std::unique_ptr<runtime::DeadlineLease> criticalDeadlineLease_;
  std::unique_ptr<runtime::DeadlineLease> continuityCompletionLease_;
  std::optional<shared::BattleInstanceId> criticalDeadlineBattleId_;
  std::uint64_t criticalDeadlineToken_{0};
  std::size_t generalControlQueueDepth_{0};
  bool criticalControlReserved_{false};
  bool criticalControlQueued_{false};
  std::size_t internalReserveHighWatermark_{0};
  std::size_t activeRuns_{0};
  std::size_t maximumConcurrentRuns_{0};
  std::uint64_t nextAdmissionOrdinal_{1};
  std::uint64_t processedCommands_{0};
  std::uint64_t processedExternalCommands_{0};
  std::uint64_t processedControlCommands_{0};
  std::uint64_t lastProcessedOrdinal_{0};
  std::uint64_t externalRejections_{0};
  std::uint64_t controlAdmissionFailures_{0};
  std::uint64_t rescheduleCount_{0};
  std::uint64_t schedulingFailures_{0};
  bool scheduled_{false};
  bool retired_{false};
  bool recoveredActivationPending_{false};
};

} // namespace lol::game_flow::execution
