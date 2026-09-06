#include "execution/RoomExecutionCell.hpp"
#include "workflows/BattleContinuityWorkflow.hpp"
#include "workflows/BattleRecoveryWorkflow.hpp"
#include "workflows/BattleTerminalWorkflow.hpp"
#include "workflows/CombatTickWorkflow.hpp"
#include "workflows/DurabilityCompletionWorkflow.hpp"
#include "workflows/GameplayTickWorkflow.hpp"
#include "workflows/HostStartWorkflow.hpp"
#include "workflows/LoadBarrierWorkflow.hpp"

#include <lol/battle/BattleTime.hpp>

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace lol::game_flow::execution {
namespace {

using namespace std::chrono_literals;

constexpr auto kLoadBarrierDeadline = 10s;
constexpr auto kCriticalSchedulingRetryDelay = 1ms;

settlement::ResultCommittedAt committedAtNow(battle::BattleTime battleTime) {
  const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
  return settlement::ResultCommittedAt{
      .unixEpochMilliseconds = static_cast<std::uint64_t>(wall),
      .monotonicNanoseconds = battleTime.battleElapsedNanos,
  };
}

} // namespace

std::shared_ptr<RoomExecutionCell> RoomExecutionCell::create(
    runtime::WorkerPool &workers, runtime::DeadlineScheduler &deadlines,
    lobby_room::Room room, WorkBudget budget, OutcomeSink outcomeSink) {
  return create(workers, deadlines, std::move(room), budget,
                std::move(outcomeSink), nullptr, nullptr, nullptr, 0U, nullptr);
}

std::shared_ptr<RoomExecutionCell> RoomExecutionCell::create(
    runtime::WorkerPool &workers, runtime::DeadlineScheduler &deadlines,
    lobby_room::Room room, WorkBudget budget, OutcomeSink outcomeSink,
    const GameplayTransportReadinessPort *readiness) {
  if (budget.maxCommands == 0 || budget.maxWallTime.count() <= 0 ||
      !outcomeSink) {
    throw std::invalid_argument(
        "RoomExecutionCell requires a positive budget and outcome sink");
  }
  return std::shared_ptr<RoomExecutionCell>{new RoomExecutionCell{
      workers, deadlines, std::move(room), budget, std::move(outcomeSink),
      readiness, nullptr, nullptr, 0U, nullptr}};
}

std::shared_ptr<RoomExecutionCell> RoomExecutionCell::create(
    runtime::WorkerPool &workers, runtime::DeadlineScheduler &deadlines,
    lobby_room::Room room, WorkBudget budget, OutcomeSink outcomeSink,
    const GameplayTransportReadinessPort *readiness,
    settlement::SettlementCapacityGate *capacityGate) {
  return create(workers, deadlines, std::move(room), budget,
                std::move(outcomeSink), readiness, capacityGate, nullptr, 0U,
                nullptr);
}

std::shared_ptr<RoomExecutionCell> RoomExecutionCell::create(
    runtime::WorkerPool &workers, runtime::DeadlineScheduler &deadlines,
    lobby_room::Room room, WorkBudget budget, OutcomeSink outcomeSink,
    const GameplayTransportReadinessPort *readiness,
    settlement::SettlementCapacityGate *capacityGate,
    settlement::SettlementStoragePort *storage,
    std::uint32_t writerRecoveryEpoch,
    battle_continuity::DurableTickWritePort *continuityStorage) {
  if (budget.maxCommands == 0 || budget.maxWallTime.count() <= 0 ||
      !outcomeSink) {
    throw std::invalid_argument(
        "RoomExecutionCell requires a positive budget and outcome sink");
  }
  return std::shared_ptr<RoomExecutionCell>{
      new RoomExecutionCell{workers, deadlines, std::move(room), budget,
                            std::move(outcomeSink), readiness, capacityGate,
                            storage, writerRecoveryEpoch, continuityStorage}};
}

std::optional<std::shared_ptr<RoomExecutionCell>>
RoomExecutionCell::createRecovered(
    runtime::WorkerPool &workers, runtime::DeadlineScheduler &deadlines,
    RecoveredRoomExecutionState recovered, WorkBudget budget,
    OutcomeSink outcomeSink, const GameplayTransportReadinessPort *readiness,
    settlement::SettlementCapacityGate *capacityGate,
    settlement::SettlementStoragePort *storage,
    std::uint32_t writerRecoveryEpoch,
    battle_continuity::DurableTickWritePort *continuityStorage) {
  if (budget.maxCommands == 0U || budget.maxWallTime.count() <= 0 ||
      !outcomeSink || capacityGate == nullptr || storage == nullptr ||
      writerRecoveryEpoch == 0U || continuityStorage == nullptr ||
      (recovered.settlementReservation.has_value() &&
       !recovered.settlementReservation->valid()) ||
      recovered.recording.takePendingBatch().has_value()) {
    return std::nullopt;
  }
  const auto detail = recovered.room.detail();
  const auto projection = recovered.battle.projection();
  const auto state = recovered.battle.exportDeterministicState();
  if (!detail.has_value() || detail->roomId != projection.roomId ||
      state.roomId != projection.roomId ||
      state.battleId != projection.battleId ||
      !recovered.nextBattleOrdinal.has_value() ||
      *recovered.nextBattleOrdinal == 0U ||
      projection.battleId.value() ==
          std::numeric_limits<std::uint64_t>::max() ||
      *recovered.nextBattleOrdinal != projection.battleId.value() + 1U ||
      static_cast<std::uint32_t>(projection.roomId.value() >> 32U) == 0U ||
      recovered.recording.records().empty() ||
      recovered.recording.records().back().header.recordType !=
          battle_continuity::RecordType::TickCommit) {
    return std::nullopt;
  }
  const auto &persistedRoomState = recovered.recording.roomRecoveryState();
  if (!persistedRoomState.has_value() ||
      persistedRoomState->nextBattleOrdinal != *recovered.nextBattleOrdinal) {
    return std::nullopt;
  }
  const auto capturedRoomState = workflows::captureRoomRecoveryState(
      recovered.room, recovered.battle, *recovered.nextBattleOrdinal);
  if (!capturedRoomState.has_value() ||
      *capturedRoomState != *persistedRoomState) {
    return std::nullopt;
  }
  const bool terminal =
      state.resultState == battle::BattleResultState::Committed;
  const bool hasReservation = recovered.settlementReservation.has_value();
  if (recovered.settlementAlreadyDurable ? (!terminal || hasReservation)
                                         : !hasReservation) {
    return std::nullopt;
  }
  const bool lifecycleMatches =
      (state.state == battle::BattleLoadState::LoadBarrierOpen &&
       detail->lifecycle == lobby_room::RoomLifecycle::Loading && !terminal) ||
      (state.state == battle::BattleLoadState::GameplayCommitted &&
       detail->lifecycle == lobby_room::RoomLifecycle::InProgress &&
       !terminal) ||
      (state.state == battle::BattleLoadState::GameplayCommitted && terminal &&
       detail->lifecycle ==
           lobby_room::RoomLifecycle::AwaitingSettlementDurability);
  const bool loading = state.state == battle::BattleLoadState::LoadBarrierOpen;
  const std::size_t participantCount =
      loading
          ? static_cast<std::size_t>(std::ranges::count_if(
                state.candidates,
                [](const auto &value) {
                  return value.state ==
                             battle::LoadCandidateState::PendingLoad ||
                         value.state == battle::LoadCandidateState::Ready;
                }))
          : static_cast<std::size_t>(std::ranges::count_if(
                projection.capturedParticipants, [terminal](const auto &value) {
                  return value.exitStatus ==
                         (terminal
                              ? battle::ParticipantExitStatus::TerminalPresent
                              : battle::ParticipantExitStatus::
                                    GameplayEligible);
                }));
  if (!lifecycleMatches || terminal != recovered.settlementBatch.has_value() ||
      detail->members.size() != participantCount) {
    return std::nullopt;
  }
  for (const auto &member : detail->members) {
    if (loading) {
      const auto candidate = std::ranges::find_if(
          projection.candidates, [&member](const auto &value) {
            return value.sessionId == member.sessionId &&
                   value.generation == member.sessionGeneration &&
                   (value.state == battle::LoadCandidateState::PendingLoad ||
                    value.state == battle::LoadCandidateState::Ready);
          });
      if (candidate == projection.candidates.end()) {
        return std::nullopt;
      }
    } else {
      const auto participant = std::ranges::find_if(
          projection.capturedParticipants,
          [&member, terminal](const auto &candidate) {
            return candidate.accountId == member.accountId &&
                   candidate.sessionId == member.sessionId &&
                   candidate.generation == member.sessionGeneration &&
                   candidate.nickname == member.nickname &&
                   candidate.exitStatus ==
                       (terminal
                            ? battle::ParticipantExitStatus::TerminalPresent
                            : battle::ParticipantExitStatus::GameplayEligible);
          });
      if (participant == projection.capturedParticipants.end()) {
        return std::nullopt;
      }
    }
  }
  if (recovered.settlementBatch.has_value() &&
      (recovered.settlementBatch->roomId() != projection.roomId ||
       recovered.settlementBatch->battleId() != projection.battleId)) {
    return std::nullopt;
  }

  auto cell = std::shared_ptr<RoomExecutionCell>{new RoomExecutionCell{
      workers, deadlines, std::move(recovered.room), budget,
      std::move(outcomeSink), readiness, capacityGate, storage,
      writerRecoveryEpoch, continuityStorage}};
  {
    std::lock_guard lock{cell->mutex_};
    cell->battle_ = std::move(recovered.battle);
    cell->battleRecording_ = std::move(recovered.recording);
    cell->settlementBatch_ = std::move(recovered.settlementBatch);
    cell->settlementReservation_ = std::move(recovered.settlementReservation);
    cell->nextBattleOrdinal_ = *recovered.nextBattleOrdinal;
    cell->battleClockAnchor_.reset();
    cell->recoveredActivationPending_ = true;
    if (terminal) {
      cell->battleCompletedLiveAt_ = std::chrono::steady_clock::now();
      cell->queueSettlementAppendLocked();
    }
  }
  return cell;
}

RoomExecutionCell::RoomExecutionCell(
    runtime::WorkerPool &workers, runtime::DeadlineScheduler &deadlines,
    lobby_room::Room room, WorkBudget budget, OutcomeSink outcomeSink,
    const GameplayTransportReadinessPort *readiness,
    settlement::SettlementCapacityGate *capacityGate,
    settlement::SettlementStoragePort *storage,
    std::uint32_t writerRecoveryEpoch,
    battle_continuity::DurableTickWritePort *continuityStorage)
    : workers_(workers), deadlines_(deadlines), room_(std::move(room)),
      budget_(budget), outcomeSink_(std::move(outcomeSink)),
      readiness_(readiness),
      capacityGate_(capacityGate != nullptr
                        ? std::optional{*capacityGate}
                        : std::optional<settlement::SettlementCapacityGate>{}),
      storage_(storage), writerRecoveryEpoch_(writerRecoveryEpoch),
      continuityStorage_(continuityStorage) {}

RoomCommandAdmission RoomExecutionCell::enqueue(RoomCommandEnvelope command) {
  std::lock_guard lock{mutex_};
  if (retired_) {
    return RoomCommandAdmission::RoomRetired;
  }
  if (recoveredActivationPending_) {
    return RoomCommandAdmission::RoomOverloaded;
  }
  if (continuityRecordingFailed_) {
    return RoomCommandAdmission::RoomOverloaded;
  }
  if (externalQueue_.size() >= kExternalQueueCapacity) {
    ++externalRejections_;
    return RoomCommandAdmission::RoomOverloaded;
  }
  externalQueue_.push_back(QueuedRoomCommand{
      .admissionOrdinal = nextAdmissionOrdinal_++,
      .lane = CommandLane::External,
      .usesCriticalControlReservation = false,
      .envelope = std::move(command),
  });
  if (continuityDurabilityPending_) {
    return RoomCommandAdmission::Accepted;
  }
  if (scheduled_) {
    return RoomCommandAdmission::Accepted;
  }
  scheduled_ = true;
  if (scheduleLocked()) {
    return RoomCommandAdmission::Accepted;
  }
  scheduled_ = false;
  externalQueue_.pop_back();
  ++schedulingFailures_;
  return RoomCommandAdmission::SchedulingUnavailable;
}

RoomCommandAdmission
RoomExecutionCell::enqueueControl(RoomControlEnvelope command) {
  std::lock_guard lock{mutex_};
  if (retired_) {
    return RoomCommandAdmission::RoomRetired;
  }
  if (recoveredActivationPending_) {
    return RoomCommandAdmission::ControlReserveExhausted;
  }
  if (continuityRecordingFailed_) {
    return RoomCommandAdmission::ControlReserveExhausted;
  }
  const auto generalCapacity =
      kControlQueueCapacity - (criticalControlReserved_ ? 1u : 0u);
  if (generalControlQueueDepth_ >= generalCapacity) {
    ++controlAdmissionFailures_;
    return RoomCommandAdmission::ControlReserveExhausted;
  }
  controlQueue_.push_back(QueuedRoomCommand{
      .admissionOrdinal = nextAdmissionOrdinal_++,
      .lane = CommandLane::InternalControl,
      .usesCriticalControlReservation = false,
      .envelope = std::move(command),
  });
  ++generalControlQueueDepth_;
  internalReserveHighWatermark_ =
      std::max(internalReserveHighWatermark_, controlQueue_.size());
  if (continuityDurabilityPending_) {
    return RoomCommandAdmission::Accepted;
  }
  if (scheduled_) {
    return RoomCommandAdmission::Accepted;
  }
  scheduled_ = true;
  if (scheduleLocked()) {
    return RoomCommandAdmission::Accepted;
  }
  scheduled_ = false;
  controlQueue_.pop_back();
  --generalControlQueueDepth_;
  ++schedulingFailures_;
  return RoomCommandAdmission::SchedulingUnavailable;
}

void RoomExecutionCell::retire() {
  std::lock_guard lock{mutex_};
  retired_ = true;
  externalQueue_.clear();
  controlQueue_.clear();
  generalControlQueueDepth_ = 0;
  releaseCriticalDeadlineLocked();
  pendingAppendRequest_.reset();
  pendingContinuityWrite_.reset();
  pendingContinuityReceipt_.reset();
  heldContinuityOutcome_.reset();
  releasedContinuityOutcome_.reset();
  continuityCompletion_.reset();
  continuityDurabilityPending_ = false;
  if (continuityCompletionLease_) {
    continuityCompletionLease_->cancel();
    continuityCompletionLease_.reset();
  }
  settlementReservation_.reset();
  retainedLootResults_.clear();
  recoveredActivationPending_ = false;
}

bool RoomExecutionCell::waitUntilIdle(std::chrono::milliseconds timeout) {
  std::unique_lock lock{mutex_};
  return idle_.wait_for(lock, timeout, [this] {
    return externalQueue_.empty() && controlQueue_.empty() && !scheduled_ &&
           activeRuns_ == 0 && !continuityDurabilityPending_ &&
           !pendingContinuityWrite_.has_value() &&
           !pendingContinuityReceipt_.has_value() &&
           !heldContinuityOutcome_.has_value() &&
           !releasedContinuityOutcome_.has_value() &&
           !continuityCompletion_.has_value();
  });
}

RoomExecutionMetrics RoomExecutionCell::metrics() const {
  std::lock_guard lock{mutex_};
  return RoomExecutionMetrics{
      .queueDepth = externalQueue_.size() + controlQueue_.size(),
      .externalQueueDepth = externalQueue_.size(),
      .controlQueueDepth = controlQueue_.size(),
      .internalReserveHighWatermark = internalReserveHighWatermark_,
      .activeRuns = activeRuns_,
      .maximumConcurrentRuns = maximumConcurrentRuns_,
      .processedCommands = processedCommands_,
      .processedExternalCommands = processedExternalCommands_,
      .processedControlCommands = processedControlCommands_,
      .lastProcessedOrdinal = lastProcessedOrdinal_,
      .externalRejections = externalRejections_,
      .controlAdmissionFailures = controlAdmissionFailures_,
      .rescheduleCount = rescheduleCount_,
      .schedulingFailures = schedulingFailures_,
  };
}

std::optional<lobby_room::RoomDetailProjection>
RoomExecutionCell::detail() const {
  std::lock_guard lock{mutex_};
  return room_.detail();
}

lobby_room::RoomLifecycle RoomExecutionCell::lifecycle() const {
  std::lock_guard lock{mutex_};
  return room_.lifecycle();
}

std::optional<settlement::SettlementIntentBatch>
RoomExecutionCell::settlementBatch() const {
  std::lock_guard lock{mutex_};
  return settlementBatch_;
}

std::optional<std::vector<std::uint8_t>>
RoomExecutionCell::battleJournal() const {
  std::lock_guard lock{mutex_};
  if (!battleRecording_.has_value()) {
    return std::nullopt;
  }
  return workflows::encodeJournal(*battleRecording_);
}

bool RoomExecutionCell::scheduleLocked() {
  auto self = shared_from_this();
  return workers_.submit([self = std::move(self)] { self->runTurn(); });
}

void RoomExecutionCell::submitPendingAppend() {
  std::optional<settlement::DurableAppendRequest> request;
  {
    std::lock_guard lock{mutex_};
    if (retired_ || storage_ == nullptr || !pendingAppendRequest_.has_value()) {
      return;
    }
    request = std::move(pendingAppendRequest_);
    pendingAppendRequest_.reset();
  }

  const auto batchId = request->batchId;
  const auto roomId = request->roomId;
  const auto battleId = request->battleId;
  const auto weak = weak_from_this();
  const auto submitted = storage_->submit(
      std::move(*request), [weak](settlement::DurableAppendOutcome outcome) {
        const auto cell = weak.lock();
        if (!cell) {
          return;
        }
        const auto admission = cell->enqueueControl(RoomControlEnvelope{
            .command = std::visit(
                [](auto value) -> RoomControlCommand {
                  return RoomControlCommand{std::move(value)};
                },
                std::move(outcome)),
            .occurredAt = std::chrono::steady_clock::now(),
        });
        if (admission == RoomCommandAdmission::RoomRetired) {
          return;
        }
        if (admission != RoomCommandAdmission::Accepted) {
          std::terminate();
        }
      });
  if (submitted == settlement::SubmitAppendResult::Accepted) {
    return;
  }
  const auto failure = submitted == settlement::SubmitAppendResult::QueueFull
                           ? settlement::SettlementStorageFailure::QueueFull
                           : settlement::SettlementStorageFailure::IoFailure;
  const auto admission = enqueueControl(RoomControlEnvelope{
      .command = RoomControlCommand{settlement::DurableAppendFailed{
          .batchId = batchId,
          .roomId = roomId,
          .battleId = battleId,
          .failure = failure,
      }},
      .occurredAt = std::chrono::steady_clock::now(),
  });
  if (admission != RoomCommandAdmission::Accepted &&
      admission != RoomCommandAdmission::RoomRetired) {
    std::terminate();
  }
}

void RoomExecutionCell::submitPendingContinuityWrite() {
  std::optional<battle_continuity::DurableTickWriteRequest> request;
  battle_continuity::DurableTickWritePort *storage = nullptr;
  {
    std::lock_guard lock{mutex_};
    if (retired_ || continuityStorage_ == nullptr ||
        !pendingContinuityWrite_.has_value()) {
      return;
    }
    storage = continuityStorage_;
    request = std::move(pendingContinuityWrite_);
    pendingContinuityWrite_.reset();
  }

  const auto identity = request->batch.identity;
  const auto writerEpoch = request->batch.writerRecoveryEpoch;
  const auto lastSequence = request->batch.lastRecordSequence;
  const auto weak = weak_from_this();
  const auto submitted = storage->submit(
      std::move(*request),
      [weak](battle_continuity::DurableTickWriteOutcome outcome) {
        if (const auto cell = weak.lock()) {
          cell->enqueueContinuityCompletion(std::move(outcome));
        }
      });
  if (submitted == battle_continuity::DurableTickSubmitResult::Accepted) {
    return;
  }
  const auto failure =
      submitted == battle_continuity::DurableTickSubmitResult::QueueFull
          ? battle_continuity::DurableTickWriteFailure::QueueFull
      : submitted ==
              battle_continuity::DurableTickSubmitResult::StaleWriterEpoch
          ? battle_continuity::DurableTickWriteFailure::StaleWriterEpoch
      : submitted == battle_continuity::DurableTickSubmitResult::InvalidRequest
          ? battle_continuity::DurableTickWriteFailure::InvalidBatch
          : battle_continuity::DurableTickWriteFailure::Stopped;
  enqueueContinuityCompletion(battle_continuity::DurableTickWriteFailed{
      .identity = identity,
      .writerRecoveryEpoch = writerEpoch,
      .lastRecordSequence = lastSequence,
      .failure = failure,
  });
}

void RoomExecutionCell::enqueueContinuityCompletion(
    battle_continuity::DurableTickWriteOutcome outcome) {
  std::lock_guard lock{mutex_};
  if (retired_ || !continuityDurabilityPending_ ||
      continuityCompletion_.has_value()) {
    return;
  }
  continuityCompletion_ = QueuedRoomCommand{
      .admissionOrdinal = nextAdmissionOrdinal_++,
      .lane = CommandLane::InternalControl,
      .usesCriticalControlReservation = false,
      .envelope =
          RoomControlEnvelope{
              .command = std::visit(
                  [](auto value) -> RoomControlCommand {
                    return RoomControlCommand{std::move(value)};
                  },
                  std::move(outcome)),
              .occurredAt = std::chrono::steady_clock::now(),
          },
  };
  if (scheduled_) {
    return;
  }
  scheduled_ = true;
  if (scheduleLocked()) {
    return;
  }
  scheduled_ = false;
  ++schedulingFailures_;
  armContinuityCompletionRetryLocked();
}

void RoomExecutionCell::retryContinuityCompletionScheduling() {
  std::lock_guard lock{mutex_};
  if (retired_ || !continuityCompletion_.has_value() || scheduled_) {
    return;
  }
  scheduled_ = true;
  if (scheduleLocked()) {
    return;
  }
  scheduled_ = false;
  ++schedulingFailures_;
  armContinuityCompletionRetryLocked();
}

void RoomExecutionCell::armContinuityCompletionRetryLocked() {
  if (!continuityCompletionLease_) {
    return;
  }
  const auto weak = weak_from_this();
  static_cast<void>(continuityCompletionLease_->armAfter(
      kCriticalSchedulingRetryDelay, [weak] {
        if (const auto cell = weak.lock()) {
          cell->retryContinuityCompletionScheduling();
        }
      }));
}

bool RoomExecutionCell::armRecoveredDeadlineLocked() {
  if (!battle_.has_value()) {
    return false;
  }
  const auto state = battle_->exportDeterministicState();
  std::optional<std::uint64_t> deadlineTick;
  std::optional<RoomControlCommand> command;
  if (state.state == battle::BattleLoadState::LoadBarrierOpen &&
      !state.combatDeadlineTick.has_value() &&
      !state.lootDeadlineTick.has_value() &&
      state.loadDeadlineTick.has_value()) {
    deadlineTick = state.loadDeadlineTick;
    command = RoomControlCommand{battle::LoadBarrierDeadlineCommand{
        .roomId = state.roomId,
        .battleId = state.battleId,
    }};
  } else if (state.state == battle::BattleLoadState::GameplayCommitted &&
             state.resultState == battle::BattleResultState::Committed &&
             !state.loadDeadlineTick.has_value() &&
             !state.combatDeadlineTick.has_value() &&
             !state.lootDeadlineTick.has_value()) {
    return true;
  } else if (state.state == battle::BattleLoadState::GameplayCommitted &&
             !state.loadDeadlineTick.has_value() &&
             !state.lootDeadlineTick.has_value() &&
             state.combatDeadlineTick.has_value()) {
    deadlineTick = state.combatDeadlineTick;
    command = RoomControlCommand{
        battle::CombatDeadlineCommand{.battleId = state.battleId}};
  } else if (state.state == battle::BattleLoadState::GameplayCommitted &&
             !state.loadDeadlineTick.has_value() &&
             !state.combatDeadlineTick.has_value() &&
             state.lootDeadlineTick.has_value()) {
    deadlineTick = state.lootDeadlineTick;
    command = RoomControlCommand{
        battle::LootDeadlineCommand{.battleId = state.battleId}};
  } else {
    return false;
  }

  const auto remainingTicks = *deadlineTick > state.battleTime.logicalTick
                                  ? *deadlineTick - state.battleTime.logicalTick
                                  : 0U;
  using MillisecondsRep = std::chrono::milliseconds::rep;
  constexpr auto maximumDelay =
      static_cast<std::uint64_t>(std::numeric_limits<MillisecondsRep>::max());
  constexpr std::uint64_t millisecondsPerTick = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::nanoseconds{battle::BattleTime::tickNanos})
          .count());
  if (remainingTicks > maximumDelay / millisecondsPerTick) {
    return false;
  }
  return reserveAndArmLoadDeadlineLocked(
      std::chrono::milliseconds{
          static_cast<MillisecondsRep>(remainingTicks * millisecondsPerTick)},
      std::move(*command), state.battleId);
}

void RoomExecutionCell::queueSettlementAppendLocked() {
  if (!settlementBatch_.has_value() || storage_ == nullptr ||
      continuityDurabilityPending_ ||
      settlementAppendState_ != SettlementAppendState::None) {
    return;
  }
  std::vector<std::vector<std::uint8_t>> canonicalIntents;
  canonicalIntents.reserve(settlementBatch_->intents().size());
  for (const auto &intent : settlementBatch_->intents()) {
    canonicalIntents.push_back(settlement::canonicalPayload(intent));
  }
  pendingAppendRequest_ = settlement::DurableAppendRequest{
      .batchId = settlementBatch_->id(),
      .roomId = settlementBatch_->roomId(),
      .battleId = settlementBatch_->battleId(),
      .canonicalIntents = std::move(canonicalIntents),
  };
  settlementAppendState_ = SettlementAppendState::Appending;
}

bool RoomExecutionCell::activateRecovered() {
  {
    std::lock_guard lock{mutex_};
    if (!recoveredActivationPending_) {
      return !retired_;
    }
    if (retired_ || !armRecoveredDeadlineLocked()) {
      return false;
    }
    recoveredActivationPending_ = false;
  }
  submitPendingAppend();
  return true;
}

bool RoomExecutionCell::reserveAndArmLoadDeadlineLocked(
    std::chrono::milliseconds delay, RoomControlCommand command,
    shared::BattleInstanceId battleId) {
  if (criticalControlReserved_ || criticalDeadlineLease_ ||
      generalControlQueueDepth_ >= kControlQueueCapacity) {
    return false;
  }
  auto lease = deadlines_.tryReserve();
  if (!lease) {
    return false;
  }
  criticalControlReserved_ = true;
  criticalDeadlineBattleId_ = battleId;
  criticalDeadlineLease_ = std::move(lease);
  if (rearmCriticalDeadlineLocked(delay, std::move(command), battleId)) {
    return true;
  }
  releaseCriticalDeadlineLocked();
  return false;
}

bool RoomExecutionCell::rearmCriticalDeadlineLocked(
    std::chrono::milliseconds delay, RoomControlCommand command,
    shared::BattleInstanceId battleId) {
  if (!criticalControlReserved_ || !criticalDeadlineLease_ ||
      criticalDeadlineBattleId_ != battleId) {
    return false;
  }
  cancelQueuedCriticalDeadlineLocked();
  const auto token = ++criticalDeadlineToken_;
  const auto weak = weak_from_this();
  return criticalDeadlineLease_->armAfter(
      delay, [weak, token, battleId, command = std::move(command)]() mutable {
        const auto cell = weak.lock();
        if (!cell) {
          return;
        }
        cell->enqueueCriticalDeadlineControl(token, battleId,
                                             std::move(command));
      });
}

void RoomExecutionCell::enqueueCriticalDeadlineControl(
    std::uint64_t token, shared::BattleInstanceId battleId,
    RoomControlCommand command) {
  std::lock_guard lock{mutex_};
  if (retired_ || !criticalControlReserved_ ||
      criticalDeadlineToken_ != token ||
      criticalDeadlineBattleId_ != battleId || !battle_.has_value() ||
      battle_->projection().battleId != battleId || criticalControlQueued_) {
    return;
  }
  if (controlQueue_.size() >= kControlQueueCapacity) {
    ++controlAdmissionFailures_;
    return;
  }
  controlQueue_.push_back(QueuedRoomCommand{
      .admissionOrdinal = nextAdmissionOrdinal_++,
      .lane = CommandLane::InternalControl,
      .usesCriticalControlReservation = true,
      .envelope =
          RoomControlEnvelope{
              .command = std::move(command),
              .occurredAt = std::chrono::steady_clock::now(),
          },
  });
  criticalControlQueued_ = true;
  internalReserveHighWatermark_ =
      std::max(internalReserveHighWatermark_, controlQueue_.size());
  if (continuityDurabilityPending_) {
    return;
  }
  if (scheduled_) {
    return;
  }
  scheduled_ = true;
  if (scheduleLocked()) {
    return;
  }
  scheduled_ = false;
  ++schedulingFailures_;
  armCriticalSchedulingRetryLocked(token, battleId);
}

void RoomExecutionCell::retryCriticalDeadlineScheduling(
    std::uint64_t token, shared::BattleInstanceId battleId) {
  std::lock_guard lock{mutex_};
  if (retired_ || criticalDeadlineToken_ != token ||
      criticalDeadlineBattleId_ != battleId || !criticalControlQueued_ ||
      scheduled_) {
    return;
  }
  scheduled_ = true;
  if (scheduleLocked()) {
    return;
  }
  scheduled_ = false;
  ++schedulingFailures_;
  armCriticalSchedulingRetryLocked(token, battleId);
}

void RoomExecutionCell::armCriticalSchedulingRetryLocked(
    std::uint64_t token, shared::BattleInstanceId battleId) {
  if (!criticalDeadlineLease_) {
    return;
  }
  const auto weak = weak_from_this();
  static_cast<void>(criticalDeadlineLease_->armAfter(
      kCriticalSchedulingRetryDelay, [weak, token, battleId] {
        const auto cell = weak.lock();
        if (cell) {
          cell->retryCriticalDeadlineScheduling(token, battleId);
        }
      }));
}

void RoomExecutionCell::cancelQueuedCriticalDeadlineLocked() {
  std::erase_if(controlQueue_, [](const QueuedRoomCommand &command) {
    return command.usesCriticalControlReservation;
  });
  criticalControlQueued_ = false;
}

void RoomExecutionCell::releaseCriticalDeadlineLocked() {
  cancelQueuedCriticalDeadlineLocked();
  ++criticalDeadlineToken_;
  if (criticalDeadlineLease_) {
    criticalDeadlineLease_->cancel();
    criticalDeadlineLease_.reset();
  }
  criticalDeadlineBattleId_.reset();
  criticalControlReserved_ = false;
}

battle::ClaimLootTerminalResult RoomExecutionCell::routeRetainedLootLocked(
    const battle::ClaimLootCommand &command,
    std::chrono::steady_clock::time_point receivedAt) {
  std::erase_if(retainedLootResults_,
                [receivedAt](const battle::RetainedLootResults &retained) {
                  return retained.expired(receivedAt);
                });
  const auto retained = std::ranges::find_if(
      retainedLootResults_, [&command](const auto &candidate) {
        return candidate.battleId() == command.battleId;
      });
  if (retained != retainedLootResults_.end()) {
    return retained->route(command, receivedAt);
  }
  return battle::ClaimLootTerminalResult{
      .commandId = command.commandId,
      .battleId = command.battleId,
      .dropId = command.dropId,
      .code = battle::ClaimLootResultCode::StaleBattle,
  };
}

battle::BattleTime RoomExecutionCell::battleTimeForLocked(
    std::chrono::steady_clock::time_point liveTime) {
  if (!battle_.has_value()) {
    return {};
  }
  const auto current = battle_->battleTime();
  const auto battleId = battle_->projection().battleId;
  if (!battleClockAnchor_.has_value() ||
      battleClockAnchor_->battleId != battleId) {
    battleClockAnchor_ = LiveBattleClockAnchor{
        .battleId = battleId,
        .liveTime = liveTime,
        .battleTime = current,
    };
    return current;
  }
  if (liveTime <= battleClockAnchor_->liveTime) {
    return current;
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           liveTime - battleClockAnchor_->liveTime)
                           .count();
  if (elapsed <= 0) {
    return current;
  }
  const auto relative =
      battle::BattleTime::fromElapsedNanos(static_cast<std::uint64_t>(elapsed));
  constexpr auto maximumTick =
      std::numeric_limits<std::uint64_t>::max() / battle::BattleTime::tickNanos;
  const auto anchorTick = battleClockAnchor_->battleTime.logicalTick;
  const auto candidateTick =
      anchorTick + std::min(relative.logicalTick, maximumTick - anchorTick);
  return battle::BattleTime::fromLogicalTick(
      std::max(current.logicalTick, candidateTick));
}

std::optional<RoomExecutionCell::QueuedRoomCommand>
RoomExecutionCell::popNextLocked() {
  if (continuityCompletion_.has_value()) {
    auto command = std::move(*continuityCompletion_);
    continuityCompletion_.reset();
    return command;
  }
  if (continuityDurabilityPending_) {
    return std::nullopt;
  }
  if (externalQueue_.empty() && controlQueue_.empty()) {
    return std::nullopt;
  }
  const bool takeExternal =
      controlQueue_.empty() ||
      (!externalQueue_.empty() && externalQueue_.front().admissionOrdinal <
                                      controlQueue_.front().admissionOrdinal);
  auto &queue = takeExternal ? externalQueue_ : controlQueue_;
  auto command = std::move(queue.front());
  queue.pop_front();
  if (command.lane == CommandLane::InternalControl) {
    if (command.usesCriticalControlReservation) {
      criticalControlQueued_ = false;
    } else {
      --generalControlQueueDepth_;
    }
  }
  return command;
}

RoomCommandOutcome RoomExecutionCell::applyLocked(QueuedRoomCommand queued) {
  const auto liveOccurredAt = std::visit(
      [](const auto &envelope) {
        if constexpr (std::is_same_v<std::decay_t<decltype(envelope)>,
                                     RoomCommandEnvelope>) {
          return envelope.receivedAt;
        } else {
          return envelope.occurredAt;
        }
      },
      queued.envelope);
  lobby_room::RoomResultCode code = lobby_room::RoomResultCode::InvalidArgument;
  RoomCommandKind kind = RoomCommandKind::SetReady;
  std::optional<shared::RequestId> requestId;
  std::optional<shared::SessionId> actorSessionId;
  std::optional<shared::SessionGeneration> actorGeneration;
  std::optional<shared::SessionId> targetSessionId;
  std::optional<shared::SessionGeneration> targetGeneration;
  std::optional<lobby_room::BattleAdmissionSnapshot> admission;
  std::optional<battle::BattleLoadResultCode> battleCode;
  std::optional<battle::BattleInputResultCode> inputControlCode;
  std::optional<battle::MovementResultCode> movementCode;
  std::optional<battle::AttackTerminalResult> attackResult;
  std::optional<battle::AttackAppliedRecord> attackApplied;
  std::optional<battle::CombatDeadlineResultCode> combatDeadlineCode;
  std::optional<battle::ClaimLootTerminalResult> lootClaimResult;
  std::optional<battle::LootDeadlineResultCode> lootDeadlineCode;
  std::optional<battle::BattleLoadProjection> battleProjection;
  std::optional<battle::BattleResumeProjection> resumeProjection;
  std::optional<battle::StateSnapshotProjection> snapshot;
  std::optional<battle::CombatProjection> combat;
  std::optional<BattleRecoveryNotice> recoveryNotice;
  std::vector<lobby_room::RoomMemberSnapshot> recoveryParticipants;
  std::optional<battle::BattleFinalResult> finalResult;
  bool lootResolutionOpened = false;
  bool gameplayStartCommitted = false;
  bool settlementDurable = false;
  const auto recordingBefore =
      battleRecording_.has_value() && battle_.has_value()
          ? std::optional{battle_->exportDeterministicState()}
          : std::nullopt;
  const auto recordingBeforeRoom =
      battleRecording_.has_value() && battle_.has_value()
          ? workflows::captureRoomRecoveryState(room_, *battle_,
                                                 nextBattleOrdinal_)
          : std::nullopt;
  std::optional<battle_continuity::CanonicalCommand> canonicalCommand;
  std::uint16_t canonicalDecisionCode = 0U;
  bool continuityRecordingFailed =
      battleRecording_.has_value() && battle_.has_value() &&
      !recordingBeforeRoom.has_value();
  if (auto *envelope = std::get_if<RoomCommandEnvelope>(&queued.envelope)) {
    requestId = envelope->requestId;
    std::visit(
        [this, &code, &kind, &actorSessionId, &actorGeneration,
         &targetSessionId, &targetGeneration, &admission, &battleCode,
         &movementCode, &attackResult, &attackApplied, &lootClaimResult,
         &battleProjection, &combat, &lootResolutionOpened,
         &gameplayStartCommitted, &recordingBefore, &canonicalCommand,
         &canonicalDecisionCode, &continuityRecordingFailed,
         receivedAt = envelope->receivedAt](auto &&command) {
          using Command = std::remove_cvref_t<decltype(command)>;
          if constexpr (std::is_same_v<Command, lobby_room::JoinRoomCommand>) {
            kind = RoomCommandKind::Join;
            actorSessionId = command.member.sessionId;
            actorGeneration = command.member.generation;
            code = room_.join(std::move(command));
          } else if constexpr (std::is_same_v<Command,
                                              lobby_room::LeaveRoomCommand>) {
            kind = RoomCommandKind::Leave;
            actorSessionId = command.sessionId;
            actorGeneration = command.generation;
            if (recordingBefore.has_value()) {
              if (const auto slot =
                      workflows::participantSlotForSession(*recordingBefore,
                                                           command.sessionId);
                  slot.has_value()) {
                canonicalCommand =
                    battle_continuity::CanonicalCommand::participantExit(*slot,
                                                                         true);
              }
            }
            const bool hadBattle = battle_.has_value();
            auto result = workflows::exitParticipant(
                room_, battle_, command.sessionId, command.generation,
                battle::ParticipantExitStatus::VoluntaryLeft,
                battleTimeForLocked(receivedAt));
            code = result.roomCode;
            if (hadBattle) {
              battleCode = result.battleCode;
              canonicalDecisionCode =
                  static_cast<std::uint16_t>(result.battleCode);
            }
            battleProjection = std::move(result.battle);
            gameplayStartCommitted = result.gameplayStartCommitted;
          } else if constexpr (std::is_same_v<Command,
                                              lobby_room::SetReadyCommand>) {
            kind = RoomCommandKind::SetReady;
            actorSessionId = command.sessionId;
            actorGeneration = command.generation;
            code = room_.setReady(command);
          } else if constexpr (std::is_same_v<
                                   Command,
                                   lobby_room::KickRoomMemberCommand>) {
            kind = RoomCommandKind::Kick;
            actorSessionId = command.actorSessionId;
            actorGeneration = command.actorGeneration;
            targetSessionId = command.targetSessionId;
            targetGeneration = command.targetGeneration;
            code = room_.kick(command);
          } else if constexpr (std::is_same_v<
                                   Command,
                                   lobby_room::HostStartEligibilityCommand>) {
            kind = RoomCommandKind::HostStartEligibility;
            actorSessionId = command.actorSessionId;
            actorGeneration = command.actorGeneration;
            bool loadDeadlineReserved = false;
            auto result = workflows::commitHostStart(
                room_, shared::BattleInstanceId{nextBattleOrdinal_}, command,
                capacityGate_.has_value() ? &*capacityGate_ : nullptr,
                [this, &loadDeadlineReserved](
                    battle::LoadBarrierDeadlineCommand deadline) {
                  loadDeadlineReserved = reserveAndArmLoadDeadlineLocked(
                      kLoadBarrierDeadline, RoomControlCommand{deadline},
                      deadline.battleId);
                  return loadDeadlineReserved;
                });
            code = result.code;
            admission = std::move(result.admission);
            if (result.battle.has_value()) {
              settlementReservation_ = std::move(result.reservation);
              battle_ = std::move(result.battle);
              battleClockAnchor_.reset();
              battleCompletedLiveAt_.reset();
              battleRecording_.reset();
              const auto activeBattleId = battle_->projection().battleId.value();
              if (activeBattleId ==
                  std::numeric_limits<std::uint64_t>::max()) {
                continuityRecordingFailed = true;
              } else {
                ++nextBattleOrdinal_;
              }
              if (writerRecoveryEpoch_ != 0U) {
                const auto roomId = battle_->projection().roomId;
                const auto originEpoch =
                    static_cast<std::uint32_t>(roomId.value() >> 32U);
                const auto roomState =
                    continuityStorage_ != nullptr &&
                            !continuityRecordingFailed
                        ? workflows::captureRoomRecoveryState(
                              room_, *battle_, nextBattleOrdinal_)
                        : std::nullopt;
                auto recording =
                    continuityStorage_ != nullptr && roomState.has_value()
                        ? battle_continuity::BattleRecording::start(
                              *battle_,
                              battle_continuity::BattleIdentity{
                                  .originRecoveryEpoch = originEpoch,
                                  .roomId = roomId,
                                  .battleInstanceId =
                                      battle_->projection().battleId,
                              },
                              writerRecoveryEpoch_, *roomState)
                        : std::nullopt;
                if (!recording.has_value()) {
                  continuityRecordingFailed = true;
                } else {
                  battleRecording_ = std::move(recording);
                }
              }
            } else if (loadDeadlineReserved) {
              releaseCriticalDeadlineLocked();
            }
          } else if constexpr (std::is_same_v<
                                   Command, battle::ArenaLoadCompleteCommand>) {
            kind = RoomCommandKind::ArenaLoadComplete;
            actorSessionId = command.sessionId;
            actorGeneration = command.generation;
            if (recordingBefore.has_value()) {
              if (const auto slot =
                      workflows::participantSlotForSession(*recordingBefore,
                                                           command.sessionId);
                  slot.has_value()) {
                canonicalCommand =
                    battle_continuity::CanonicalCommand::arenaLoadComplete(
                        *slot);
              }
            }
            auto result =
                workflows::completeLoad(room_, battle_, command, readiness_,
                                        battleTimeForLocked(receivedAt));
            battleCode = result.code;
            canonicalDecisionCode = static_cast<std::uint16_t>(result.code);
            battleProjection = std::move(result.battle);
            gameplayStartCommitted = result.gameplayStartCommitted;
            code = result.code == battle::BattleLoadResultCode::Ok
                       ? lobby_room::RoomResultCode::Ok
                       : lobby_room::RoomResultCode::InvalidArgument;
          } else if constexpr (std::is_same_v<Command, battle::MoveCommand>) {
            kind = RoomCommandKind::Move;
            actorSessionId = command.sessionId;
            actorGeneration = command.generation;
            if (recordingBefore.has_value()) {
              if (const auto slot =
                      workflows::participantSlotForSession(*recordingBefore,
                                                           command.sessionId);
                  slot.has_value()) {
                canonicalCommand = battle_continuity::CanonicalCommand::move(
                    *slot,
                    battle_continuity::CommandId{.high = 0U,
                                                 .low = command.actionSequence},
                    command.direction);
              }
            }
            movementCode = battle_.has_value()
                               ? battle_->acceptMove(
                                     command, battleTimeForLocked(receivedAt))
                               : battle::MovementResultCode::StaleBattle;
            canonicalDecisionCode = static_cast<std::uint16_t>(*movementCode);
            code = lobby_room::RoomResultCode::Ok;
          } else if constexpr (std::is_same_v<Command, battle::AttackCommand>) {
            kind = RoomCommandKind::Attack;
            actorSessionId = command.sessionId;
            actorGeneration = command.generation;
            if (recordingBefore.has_value()) {
              if (const auto slot =
                      workflows::participantSlotForSession(*recordingBefore,
                                                           command.sessionId);
                  slot.has_value()) {
                canonicalCommand = battle_continuity::CanonicalCommand::attack(
                    *slot,
                    battle_continuity::CommandId{.high = command.commandId.high,
                                                 .low = command.commandId.low},
                    command.targetHint);
              }
            }
            auto result = workflows::applyAttack(
                battle_, command, battleTimeForLocked(receivedAt));
            attackResult = std::move(result.result);
            canonicalDecisionCode =
                static_cast<std::uint16_t>(attackResult->code);
            attackApplied = std::move(result.applied);
            combat = std::move(result.combat);
            lootResolutionOpened = result.lootResolutionOpened;
            code = lobby_room::RoomResultCode::Ok;
          } else {
            kind = RoomCommandKind::ClaimLoot;
            actorSessionId = command.sessionId;
            actorGeneration = command.generation;
            if (battle_.has_value() &&
                battle_->projection().battleId == command.battleId) {
              if (recordingBefore.has_value()) {
                if (const auto slot =
                        workflows::participantSlotForSession(
                            *recordingBefore, command.sessionId);
                    slot.has_value()) {
                  canonicalCommand =
                      battle_continuity::CanonicalCommand::claimLoot(
                          *slot,
                          battle_continuity::CommandId{
                              .high = command.commandId.high,
                              .low = command.commandId.low},
                          command.dropId.value);
                }
              }
              lootClaimResult =
                  battle_->claimLoot(command, battleTimeForLocked(receivedAt));
              canonicalDecisionCode =
                  static_cast<std::uint16_t>(lootClaimResult->code);
            } else {
              lootClaimResult = routeRetainedLootLocked(command, receivedAt);
            }
            code = lobby_room::RoomResultCode::Ok;
          }
        },
        std::move(envelope->command));
  } else {
    auto controlEnvelope =
        std::get<RoomControlEnvelope>(std::move(queued.envelope));
    std::visit(
        [this, &code, &kind, &actorSessionId, &actorGeneration, &battleCode,
         &inputControlCode, &movementCode, &combatDeadlineCode,
         &lootDeadlineCode, &battleProjection, &resumeProjection, &snapshot,
         &combat, &recoveryNotice, &finalResult, &gameplayStartCommitted,
         &settlementDurable, &recordingBefore, &canonicalCommand,
         &canonicalDecisionCode, &continuityRecordingFailed,
         occurredAt = controlEnvelope.occurredAt](const auto &command) {
          using Command = std::remove_cvref_t<decltype(command)>;
          if constexpr (std::is_same_v<Command, ConfirmedDisconnectCommand>) {
            kind = RoomCommandKind::ConfirmedDisconnect;
            actorSessionId = command.sessionId;
            actorGeneration = command.generation;
            if (recordingBefore.has_value()) {
              if (const auto slot =
                      workflows::participantSlotForSession(*recordingBefore,
                                                           command.sessionId);
                  slot.has_value()) {
                canonicalCommand =
                    battle_continuity::CanonicalCommand::participantExit(*slot,
                                                                         false);
              }
            }
            const bool hadBattle = battle_.has_value();
            auto result = workflows::disconnect(
                room_, battle_, command.sessionId, command.generation,
                battleTimeForLocked(occurredAt));
            code = result.roomCode;
            if (hadBattle) {
              battleCode = result.battleCode;
              canonicalDecisionCode =
                  static_cast<std::uint16_t>(result.battleCode);
            }
            battleProjection = std::move(result.battle);
            gameplayStartCommitted = result.gameplayStartCommitted;
          } else if constexpr (std::is_same_v<
                                   Command,
                                   battle::SuspendBattleInputCommand>) {
            kind = RoomCommandKind::SuspendBattleInput;
            actorSessionId = command.sessionId;
            actorGeneration = command.generation;
            if (recordingBefore.has_value()) {
              if (const auto slot =
                      workflows::participantSlotForSession(*recordingBefore,
                                                           command.sessionId);
                  slot.has_value()) {
                canonicalCommand =
                    battle_continuity::CanonicalCommand::suspendInput(*slot);
              }
            }
            inputControlCode = battle_.has_value()
                                   ? battle_->suspendInput(
                                         command.sessionId, command.generation,
                                         battleTimeForLocked(occurredAt))
                                   : battle::BattleInputResultCode::StaleBattle;
            canonicalDecisionCode =
                static_cast<std::uint16_t>(*inputControlCode);
            code = lobby_room::RoomResultCode::Ok;
          } else if constexpr (std::is_same_v<
                                   Command, battle::ResumeBattleInputCommand>) {
            kind = RoomCommandKind::ResumeBattleInput;
            actorSessionId = command.sessionId;
            actorGeneration = command.generation;
            if (recordingBefore.has_value()) {
              if (const auto slot =
                      workflows::participantSlotForSession(*recordingBefore,
                                                           command.sessionId);
                  slot.has_value()) {
                canonicalCommand =
                    battle_continuity::CanonicalCommand::resumeInput(*slot);
              }
            }
            inputControlCode = battle_.has_value()
                                   ? battle_->resumeInput(
                                         command.sessionId, command.generation,
                                         battleTimeForLocked(occurredAt))
                                   : battle::BattleInputResultCode::StaleBattle;
            canonicalDecisionCode =
                static_cast<std::uint16_t>(*inputControlCode);
            if (battle_.has_value()) {
              resumeProjection = battle_->resumeProjection(command.sessionId,
                                                           command.generation);
            }
            code = lobby_room::RoomResultCode::Ok;
          } else if constexpr (std::is_same_v<
                                   Command,
                                   battle::LoadBarrierDeadlineCommand>) {
            kind = RoomCommandKind::LoadBarrierDeadline;
            if (recordingBefore.has_value()) {
              canonicalCommand =
                  battle_continuity::CanonicalCommand::loadBarrierDeadline();
            }
            auto result = workflows::expireLoadBarrier(room_, battle_, command);
            battleCode = result.code;
            canonicalDecisionCode = static_cast<std::uint16_t>(result.code);
            battleProjection = std::move(result.battle);
            gameplayStartCommitted = result.gameplayStartCommitted;
            code = result.code == battle::BattleLoadResultCode::Ok
                       ? lobby_room::RoomResultCode::Ok
                       : lobby_room::RoomResultCode::InvalidArgument;
          } else if constexpr (std::is_same_v<Command,
                                              battle::MovementTickCommand>) {
            kind = RoomCommandKind::MovementTick;
            if (recordingBefore.has_value()) {
              canonicalCommand =
                  battle_continuity::CanonicalCommand::movementTick(
                      command.serverTick);
            }
            auto result = workflows::advanceGameplayTick(
                battle_, command, battleTimeForLocked(occurredAt));
            movementCode = result.code;
            canonicalDecisionCode = static_cast<std::uint16_t>(result.code);
            snapshot = std::move(result.snapshot);
            code = lobby_room::RoomResultCode::Ok;
          } else if constexpr (std::is_same_v<Command,
                                              battle::CombatDeadlineCommand>) {
            kind = RoomCommandKind::CombatDeadline;
            if (recordingBefore.has_value()) {
              canonicalCommand =
                  battle_continuity::CanonicalCommand::combatDeadline();
            }
            auto result = workflows::expireCombat(battle_, command);
            combatDeadlineCode = result.code;
            canonicalDecisionCode = static_cast<std::uint16_t>(result.code);
            combat = std::move(result.combat);
            code = lobby_room::RoomResultCode::Ok;
          } else if constexpr (std::is_same_v<Command,
                                              battle::LootDeadlineCommand>) {
            kind = RoomCommandKind::LootDeadline;
            if (recordingBefore.has_value()) {
              canonicalCommand =
                  battle_continuity::CanonicalCommand::lootDeadline();
            }
            lootDeadlineCode = workflows::expireLoot(battle_, command).code;
            canonicalDecisionCode =
                static_cast<std::uint16_t>(*lootDeadlineCode);
            code = lobby_room::RoomResultCode::Ok;
          } else if constexpr (std::is_same_v<
                                   Command,
                                   battle_continuity::DurableTickCommitted>) {
            kind = RoomCommandKind::ContinuityTickCommitted;
            const bool correlated =
                continuityDurabilityPending_ &&
                pendingContinuityReceipt_.has_value() &&
                heldContinuityOutcome_.has_value() &&
                command.identity == pendingContinuityReceipt_->identity &&
                command.writerRecoveryEpoch ==
                    pendingContinuityReceipt_->writerRecoveryEpoch &&
                command.lastRecordSequence ==
                    pendingContinuityReceipt_->lastRecordSequence;
            if (!correlated) {
              code = lobby_room::RoomResultCode::InvalidArgument;
              return;
            }
            continuityDurabilityPending_ = false;
            pendingContinuityReceipt_.reset();
            if (continuityCompletionLease_) {
              continuityCompletionLease_->cancel();
              continuityCompletionLease_.reset();
            }
            releasedContinuityOutcome_ = std::move(heldContinuityOutcome_);
            heldContinuityOutcome_.reset();
            code = lobby_room::RoomResultCode::Ok;
          } else if constexpr (std::is_same_v<
                                   Command,
                                   battle_continuity::DurableTickWriteFailed>) {
            kind = RoomCommandKind::ContinuityTickFailed;
            const bool correlated =
                continuityDurabilityPending_ &&
                pendingContinuityReceipt_.has_value() &&
                command.identity == pendingContinuityReceipt_->identity &&
                command.writerRecoveryEpoch ==
                    pendingContinuityReceipt_->writerRecoveryEpoch &&
                command.lastRecordSequence ==
                    pendingContinuityReceipt_->lastRecordSequence;
            if (!correlated) {
              code = lobby_room::RoomResultCode::InvalidArgument;
              return;
            }
            continuityRecordingFailed = true;
            code = lobby_room::RoomResultCode::InvalidArgument;
          } else if constexpr (std::is_same_v<
                                   Command,
                                   settlement::DurableAppendCompleted>) {
            kind = RoomCommandKind::DurableAppendCompleted;
            if (settlementAppendState_ != SettlementAppendState::Appending &&
                settlementAppendState_ !=
                    SettlementAppendState::RetryableStorageFailure) {
              code = lobby_room::RoomResultCode::InvalidArgument;
              return;
            }
            auto result = workflows::completeSettlementDurability(
                room_, battle_, settlementBatch_, command);
            code = result.code;
            finalResult = std::move(result.finalResult);
            battleProjection = std::move(result.retiredBattle);
            settlementDurable = result.applied;
            if (result.applied) {
              if (result.retainedLootResults.has_value()) {
                if (battleCompletedLiveAt_.has_value()) {
                  static_cast<void>(result.retainedLootResults->expired(
                      *battleCompletedLiveAt_));
                }
                retainedLootResults_.push_back(
                    std::move(*result.retainedLootResults));
              }
              battleCompletedLiveAt_.reset();
              settlementAppendState_ = SettlementAppendState::None;
              settlementRecoveryNoticeEmitted_ = false;
            }
          } else {
            kind = RoomCommandKind::DurableAppendFailed;
            const bool correlated =
                settlementBatch_.has_value() && battle_.has_value() &&
                command.batchId == settlementBatch_->id() &&
                command.roomId == settlementBatch_->roomId() &&
                command.battleId == settlementBatch_->battleId();
            if (correlated &&
                settlementAppendState_ == SettlementAppendState::Appending) {
              settlementAppendState_ =
                  SettlementAppendState::RetryableStorageFailure;
              if (!settlementRecoveryNoticeEmitted_) {
                recoveryNotice = BattleRecoveryNotice{
                    .roomId = command.roomId,
                    .battleId = command.battleId,
                    .reason = BattleRecoveryReason::SettlementRecoveryPending,
                };
                settlementRecoveryNoticeEmitted_ = true;
              }
            }
            code = lobby_room::RoomResultCode::InvalidArgument;
          }
        },
        controlEnvelope.command);
  }
  if (!battleCompletedLiveAt_.has_value() && battle_.has_value() &&
      battle_->resultProjection().state ==
          battle::BattleResultState::Committed) {
    battleCompletedLiveAt_ = liveOccurredAt;
  }
  if (!battle_.has_value()) {
    battleClockAnchor_.reset();
    battleCompletedLiveAt_.reset();
    battleRecording_.reset();
    settlementReservation_.reset();
  }
  if (gameplayStartCommitted && battle_.has_value() &&
      !rearmCriticalDeadlineLocked(
          std::chrono::milliseconds{
              battle::CombatRuleset::combatDeadlineMillis},
          RoomControlCommand{battle::CombatDeadlineCommand{
              .battleId = battle_->projection().battleId,
          }},
          battle_->projection().battleId)) {
    ++schedulingFailures_;
    releaseCriticalDeadlineLocked();
  }
  if (lootResolutionOpened &&
      !rearmCriticalDeadlineLocked(
          std::chrono::milliseconds{
              battle::RelicRuleset::resolutionWindowMillis},
          RoomControlCommand{battle::LootDeadlineCommand{
              .battleId = battle_->projection().battleId,
          }},
          battle_->projection().battleId)) {
    ++schedulingFailures_;
    releaseCriticalDeadlineLocked();
  }
  bool terminalBatchCreated = false;
  if (!settlementBatch_.has_value() && battle_.has_value()) {
    auto batch = workflows::holdTerminalForSettlementDurability(
        room_, *battle_, committedAtNow(battle_->battleTime()));
    if (batch.has_value()) {
      settlementBatch_ = std::move(batch);
      terminalBatchCreated = true;
    }
  }
  const auto recordingAfterRoom =
      battleRecording_.has_value() && battle_.has_value()
          ? workflows::captureRoomRecoveryState(room_, *battle_,
                                                 nextBattleOrdinal_)
          : std::nullopt;
  if (battleRecording_.has_value() && battle_.has_value() &&
      !recordingAfterRoom.has_value()) {
    continuityRecordingFailed = true;
  }
  if (battleRecording_.has_value() && recordingBefore.has_value() &&
      battle_.has_value()) {
    if (!recordingBeforeRoom.has_value() || !recordingAfterRoom.has_value() ||
        (!canonicalCommand.has_value() &&
         *recordingBefore != battle_->exportDeterministicState())) {
      continuityRecordingFailed = true;
    }
    std::optional<battle_continuity::TerminalRecording> terminal;
    if (terminalBatchCreated) {
      terminal = workflows::captureTerminalRecording(*battle_,
                                                      *settlementBatch_);
      if (!canonicalCommand.has_value() || !terminal.has_value()) {
        continuityRecordingFailed = true;
      }
    }
    if (!continuityRecordingFailed && canonicalCommand.has_value() &&
        !battleRecording_->recordDecision(
            *canonicalCommand, *recordingBefore, *recordingBeforeRoom,
            *battle_, *recordingAfterRoom, canonicalDecisionCode,
            std::move(terminal))) {
      continuityRecordingFailed = true;
    }
  }
  if (!continuityRecordingFailed && battleRecording_.has_value()) {
    if (auto batch = battleRecording_->takePendingBatch();
        batch.has_value() && continuityStorage_ != nullptr) {
      std::optional<battle_continuity::Bytes> privateEnvelope;
      if (batch->firstRecordSequence == 1U) {
        privateEnvelope =
            battle_.has_value()
                ? workflows::captureRecoveryEnvelope(room_, *battle_,
                                                      batch->identity)
                : std::nullopt;
        if (!privateEnvelope.has_value()) {
          continuityRecordingFailed = true;
        }
      }
      if (continuityDurabilityPending_ || pendingContinuityWrite_.has_value() ||
          pendingContinuityReceipt_.has_value() ||
          heldContinuityOutcome_.has_value()) {
        continuityRecordingFailed = true;
      } else if (!continuityRecordingFailed) {
        auto lease = deadlines_.tryReserve();
        if (!lease) {
          continuityRecordingFailed = true;
        } else {
          pendingContinuityReceipt_ = battle_continuity::DurableTickCommitted{
              .identity = batch->identity,
              .writerRecoveryEpoch = batch->writerRecoveryEpoch,
              .lastRecordSequence = batch->lastRecordSequence,
          };
          pendingContinuityWrite_ = battle_continuity::DurableTickWriteRequest{
              .batch = std::move(*batch),
              .privateEnvelopePlaintext = std::move(privateEnvelope),
          };
          continuityCompletionLease_ = std::move(lease);
          continuityDurabilityPending_ = true;
        }
      }
    }
  }
  if (continuityRecordingFailed && battle_.has_value()) {
    continuityRecordingFailed_ = true;
    const auto failedBattle = battle_->projection();
    recoveryNotice = BattleRecoveryNotice{
        .roomId = failedBattle.roomId,
        .battleId = failedBattle.battleId,
        .reason = BattleRecoveryReason::ContinuityRecordingFailed,
    };
    if (const auto detail = room_.detail(); detail.has_value()) {
      recoveryParticipants = detail->members;
    }
    battleProjection = failedBattle;
    releaseCriticalDeadlineLocked();
    externalQueue_.clear();
    controlQueue_.clear();
    generalControlQueueDepth_ = 0U;
    criticalControlQueued_ = false;
    pendingAppendRequest_.reset();
    pendingContinuityWrite_.reset();
    pendingContinuityReceipt_.reset();
    heldContinuityOutcome_.reset();
    releasedContinuityOutcome_.reset();
    continuityCompletion_.reset();
    continuityDurabilityPending_ = false;
    if (continuityCompletionLease_) {
      continuityCompletionLease_->cancel();
      continuityCompletionLease_.reset();
    }
  } else {
    queueSettlementAppendLocked();
  }
  if (!continuityRecordingFailed_ && battle_.has_value() &&
      !settlementBatch_.has_value()) {
    const auto projection = battle_->projection();
    auto recovery = workflows::recoverResultGenerationFailure(
        room_, projection, battle_->resultProjection(),
        emittedResultFailureBattle_);
    if (recovery.has_value()) {
      recoveryNotice = recovery->notice;
      recoveryParticipants = std::move(recovery->participants);
      battleProjection = projection;
      battle_.reset();
      battleRecording_.reset();
      settlementReservation_.reset();
    }
  }
  if (battle_.has_value() &&
      battle_->projection().state == battle::BattleLoadState::LoadCancelled &&
      !continuityDurabilityPending_) {
    battleProjection = battle_->projection();
    battle_.reset();
    battleRecording_.reset();
    battleClockAnchor_.reset();
    settlementReservation_.reset();
  }
  if (!battle_.has_value() || settlementBatch_.has_value()) {
    releaseCriticalDeadlineLocked();
  }
  return RoomCommandOutcome{
      .admissionOrdinal = queued.admissionOrdinal,
      .lane = queued.lane,
      .kind = kind,
      .lifecycle = room_.lifecycle(),
      .requestId = requestId,
      .actorSessionId = actorSessionId,
      .actorGeneration = actorGeneration,
      .targetSessionId = targetSessionId,
      .targetGeneration = targetGeneration,
      .code = code,
      .battleCode = battleCode,
      .inputControlCode = inputControlCode,
      .movementCode = movementCode,
      .attackResult = std::move(attackResult),
      .attackApplied = std::move(attackApplied),
      .combatDeadlineCode = combatDeadlineCode,
      .lootClaimResult = std::move(lootClaimResult),
      .lootDeadlineCode = lootDeadlineCode,
      .loot = battle_.has_value() ? std::optional{battle_->lootProjection()}
                                  : std::nullopt,
      .summary = room_.summary(),
      .detail = room_.detail(),
      .admission = std::move(admission),
      .battle =
          battleProjection.has_value()
              ? std::move(battleProjection)
              : (battle_.has_value() ? std::optional{battle_->projection()}
                                     : std::nullopt),
      .resumeProjection = std::move(resumeProjection),
      .snapshot = std::move(snapshot),
      .combat = std::move(combat),
      .recoveryNotice = std::move(recoveryNotice),
      .recoveryParticipants = std::move(recoveryParticipants),
      .finalResult = std::move(finalResult),
      .gameplayStartCommitted = gameplayStartCommitted,
      .lootResolutionOpened = lootResolutionOpened,
      .settlementDurable = settlementDurable,
      .queueDelay = std::chrono::nanoseconds::zero(),
      .processingDuration = std::chrono::nanoseconds::zero(),
      .criticalTerminalLatency = std::nullopt,
  };
}

void RoomExecutionCell::runTurn() noexcept {
  {
    std::lock_guard lock{mutex_};
    ++activeRuns_;
    maximumConcurrentRuns_ = std::max(maximumConcurrentRuns_, activeRuns_);
  }

  while (true) {
    const auto startedAt = std::chrono::steady_clock::now();
    std::size_t processed = 0;
    while (processed < budget_.maxCommands) {
      std::optional<RoomCommandOutcome> outcome;
      {
        std::lock_guard lock{mutex_};
        auto command = popNextLocked();
        if (!command.has_value()) {
          break;
        }
        const auto processingStartedAt = std::chrono::steady_clock::now();
        const auto enqueuedAt = std::visit(
            [](const auto &envelope) {
              if constexpr (std::is_same_v<std::decay_t<decltype(envelope)>,
                                           RoomCommandEnvelope>) {
                return envelope.receivedAt;
              } else {
                return envelope.occurredAt;
              }
            },
            command->envelope);
        const auto processedOrdinal = command->admissionOrdinal;
        const auto processedLane = command->lane;
        auto applied = applyLocked(std::move(*command));
        const auto completedAt = std::chrono::steady_clock::now();
        const bool releasingDurableOutcome =
            releasedContinuityOutcome_.has_value();
        if (releasingDurableOutcome) {
          outcome = std::move(releasedContinuityOutcome_);
          releasedContinuityOutcome_.reset();
        } else {
          outcome = std::move(applied);
          if (enqueuedAt.time_since_epoch() >
                  std::chrono::steady_clock::duration::zero() &&
              enqueuedAt <= processingStartedAt) {
            outcome->queueDelay = processingStartedAt - enqueuedAt;
          }
          outcome->processingDuration = completedAt - processingStartedAt;
          if (outcome->attackResult.has_value() ||
              outcome->lootClaimResult.has_value()) {
            outcome->criticalTerminalLatency =
                outcome->queueDelay + outcome->processingDuration;
          }
        }

        if (continuityDurabilityPending_ &&
            pendingContinuityWrite_.has_value() &&
            !heldContinuityOutcome_.has_value()) {
          heldContinuityOutcome_ = std::move(outcome);
          outcome.reset();
        }

        ++processedCommands_;
        if (processedLane == CommandLane::External) {
          ++processedExternalCommands_;
        } else {
          ++processedControlCommands_;
        }
        lastProcessedOrdinal_ = processedOrdinal;
      }
      submitPendingContinuityWrite();
      submitPendingAppend();
      if (outcome.has_value()) {
        outcomeSink_(std::move(*outcome));
      }
      ++processed;
      if (std::chrono::steady_clock::now() - startedAt >= budget_.maxWallTime) {
        break;
      }
    }

    std::lock_guard lock{mutex_};
    if (continuityDurabilityPending_ && !continuityCompletion_.has_value()) {
      --activeRuns_;
      scheduled_ = false;
      idle_.notify_all();
      return;
    }
    if (externalQueue_.empty() && controlQueue_.empty() &&
        !continuityCompletion_.has_value()) {
      --activeRuns_;
      scheduled_ = false;
      idle_.notify_all();
      return;
    }
    ++rescheduleCount_;
    if (scheduleLocked()) {
      --activeRuns_;
      return;
    }
    ++schedulingFailures_;
  }
}

} // namespace lol::game_flow::execution
