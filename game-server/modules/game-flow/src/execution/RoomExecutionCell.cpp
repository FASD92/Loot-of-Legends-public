#include "execution/RoomExecutionCell.hpp"
#include "workflows/BattleRecoveryWorkflow.hpp"
#include "workflows/BattleTerminalWorkflow.hpp"
#include "workflows/CombatTickWorkflow.hpp"
#include "workflows/DurabilityCompletionWorkflow.hpp"
#include "workflows/GameplayTickWorkflow.hpp"
#include "workflows/HostStartWorkflow.hpp"
#include "workflows/LoadBarrierWorkflow.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace lol::game_flow::execution {
namespace {

using namespace std::chrono_literals;

constexpr auto kLoadBarrierDeadline = 10s;
constexpr auto kCriticalSchedulingRetryDelay = 1ms;

settlement::ResultCommittedAt committedAtNow() {
  const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
  const auto monotonic =
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();
  return settlement::ResultCommittedAt{
      .unixEpochMilliseconds = static_cast<std::uint64_t>(wall),
      .monotonicNanoseconds = static_cast<std::uint64_t>(monotonic),
  };
}

} // namespace

std::shared_ptr<RoomExecutionCell> RoomExecutionCell::create(
    runtime::WorkerPool &workers, runtime::DeadlineScheduler &deadlines,
    lobby_room::Room room, WorkBudget budget, OutcomeSink outcomeSink) {
  return create(workers, deadlines, std::move(room), budget,
                std::move(outcomeSink), nullptr, nullptr, nullptr);
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
      readiness, nullptr, nullptr}};
}

std::shared_ptr<RoomExecutionCell> RoomExecutionCell::create(
    runtime::WorkerPool &workers, runtime::DeadlineScheduler &deadlines,
    lobby_room::Room room, WorkBudget budget, OutcomeSink outcomeSink,
    const GameplayTransportReadinessPort *readiness,
    settlement::SettlementCapacityGate *capacityGate) {
  return create(workers, deadlines, std::move(room), budget,
                std::move(outcomeSink), readiness, capacityGate, nullptr);
}

std::shared_ptr<RoomExecutionCell> RoomExecutionCell::create(
    runtime::WorkerPool &workers, runtime::DeadlineScheduler &deadlines,
    lobby_room::Room room, WorkBudget budget, OutcomeSink outcomeSink,
    const GameplayTransportReadinessPort *readiness,
    settlement::SettlementCapacityGate *capacityGate,
    settlement::SettlementStoragePort *storage) {
  if (budget.maxCommands == 0 || budget.maxWallTime.count() <= 0 ||
      !outcomeSink) {
    throw std::invalid_argument(
        "RoomExecutionCell requires a positive budget and outcome sink");
  }
  return std::shared_ptr<RoomExecutionCell>{new RoomExecutionCell{
      workers, deadlines, std::move(room), budget, std::move(outcomeSink),
      readiness, capacityGate, storage}};
}

RoomExecutionCell::RoomExecutionCell(
    runtime::WorkerPool &workers, runtime::DeadlineScheduler &deadlines,
    lobby_room::Room room, WorkBudget budget, OutcomeSink outcomeSink,
    const GameplayTransportReadinessPort *readiness,
    settlement::SettlementCapacityGate *capacityGate,
    settlement::SettlementStoragePort *storage)
    : workers_(workers), deadlines_(deadlines), room_(std::move(room)),
      budget_(budget), outcomeSink_(std::move(outcomeSink)),
      readiness_(readiness),
      capacityGate_(capacityGate != nullptr
                        ? std::optional{*capacityGate}
                        : std::optional<settlement::SettlementCapacityGate>{}),
      storage_(storage) {}

RoomCommandAdmission RoomExecutionCell::enqueue(RoomCommandEnvelope command) {
  std::lock_guard lock{mutex_};
  if (retired_) {
    return RoomCommandAdmission::RoomRetired;
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
  settlementReservation_.reset();
  retainedLootResults_.clear();
}

bool RoomExecutionCell::waitUntilIdle(std::chrono::milliseconds timeout) {
  std::unique_lock lock{mutex_};
  return idle_.wait_for(lock, timeout, [this] {
    return externalQueue_.empty() && controlQueue_.empty() && !scheduled_ &&
           activeRuns_ == 0;
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

std::optional<RoomExecutionCell::QueuedRoomCommand>
RoomExecutionCell::popNextLocked() {
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
  lobby_room::RoomResultCode code = lobby_room::RoomResultCode::InvalidArgument;
  RoomCommandKind kind = RoomCommandKind::SetReady;
  std::optional<shared::RequestId> requestId;
  std::optional<shared::SessionId> actorSessionId;
  std::optional<shared::SessionGeneration> actorGeneration;
  std::optional<shared::SessionId> targetSessionId;
  std::optional<shared::SessionGeneration> targetGeneration;
  std::optional<lobby_room::BattleAdmissionSnapshot> admission;
  std::optional<battle::BattleLoadResultCode> battleCode;
  std::optional<battle::MovementResultCode> movementCode;
  std::optional<battle::AttackTerminalResult> attackResult;
  std::optional<battle::CombatDeadlineResultCode> combatDeadlineCode;
  std::optional<battle::ClaimLootTerminalResult> lootClaimResult;
  std::optional<battle::LootDeadlineResultCode> lootDeadlineCode;
  std::optional<battle::BattleLoadProjection> battleProjection;
  std::optional<battle::StateSnapshotProjection> snapshot;
  std::optional<battle::CombatProjection> combat;
  std::optional<BattleRecoveryNotice> recoveryNotice;
  std::vector<lobby_room::RoomMemberSnapshot> recoveryParticipants;
  std::optional<battle::BattleFinalResult> finalResult;
  bool lootResolutionOpened = false;
  bool gameplayStartCommitted = false;
  bool settlementDurable = false;
  if (auto *envelope = std::get_if<RoomCommandEnvelope>(&queued.envelope)) {
    requestId = envelope->requestId;
    std::visit(
        [this, &code, &kind, &actorSessionId, &actorGeneration,
         &targetSessionId, &targetGeneration, &admission, &battleCode,
         &movementCode, &attackResult, &lootClaimResult, &battleProjection,
         &combat, &lootResolutionOpened, &gameplayStartCommitted,
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
            const bool hadBattle = battle_.has_value();
            auto result = workflows::exitParticipant(
                room_, battle_, command.sessionId, command.generation,
                battle::ParticipantExitStatus::VoluntaryLeft, receivedAt);
            code = result.roomCode;
            if (hadBattle) {
              battleCode = result.battleCode;
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
              ++nextBattleOrdinal_;
            } else if (loadDeadlineReserved) {
              releaseCriticalDeadlineLocked();
            }
          } else if constexpr (std::is_same_v<
                                   Command, battle::ArenaLoadCompleteCommand>) {
            kind = RoomCommandKind::ArenaLoadComplete;
            actorSessionId = command.sessionId;
            actorGeneration = command.generation;
            auto result =
                workflows::completeLoad(room_, battle_, command, readiness_);
            battleCode = result.code;
            battleProjection = std::move(result.battle);
            gameplayStartCommitted = result.gameplayStartCommitted;
            code = result.code == battle::BattleLoadResultCode::Ok
                       ? lobby_room::RoomResultCode::Ok
                       : lobby_room::RoomResultCode::InvalidArgument;
          } else if constexpr (std::is_same_v<Command, battle::MoveCommand>) {
            kind = RoomCommandKind::Move;
            actorSessionId = command.sessionId;
            actorGeneration = command.generation;
            movementCode = battle_.has_value()
                               ? battle_->acceptMove(command, receivedAt)
                               : battle::MovementResultCode::StaleBattle;
            code = lobby_room::RoomResultCode::Ok;
          } else if constexpr (std::is_same_v<Command, battle::AttackCommand>) {
            kind = RoomCommandKind::Attack;
            actorSessionId = command.sessionId;
            actorGeneration = command.generation;
            auto result = workflows::applyAttack(battle_, command, receivedAt);
            attackResult = std::move(result.result);
            combat = std::move(result.combat);
            lootResolutionOpened = result.lootResolutionOpened;
            code = lobby_room::RoomResultCode::Ok;
          } else {
            kind = RoomCommandKind::ClaimLoot;
            actorSessionId = command.sessionId;
            actorGeneration = command.generation;
            if (battle_.has_value() &&
                battle_->projection().battleId == command.battleId) {
              lootClaimResult = battle_->claimLoot(command, receivedAt);
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
         &movementCode, &combatDeadlineCode, &lootDeadlineCode,
         &battleProjection, &snapshot, &combat, &recoveryNotice, &finalResult,
         &gameplayStartCommitted, &settlementDurable,
         occurredAt = controlEnvelope.occurredAt](const auto &command) {
          using Command = std::remove_cvref_t<decltype(command)>;
          if constexpr (std::is_same_v<Command, ConfirmedDisconnectCommand>) {
            kind = RoomCommandKind::ConfirmedDisconnect;
            actorSessionId = command.sessionId;
            actorGeneration = command.generation;
            const bool hadBattle = battle_.has_value();
            auto result =
                workflows::disconnect(room_, battle_, command.sessionId,
                                      command.generation, occurredAt);
            code = result.roomCode;
            if (hadBattle) {
              battleCode = result.battleCode;
            }
            battleProjection = std::move(result.battle);
            gameplayStartCommitted = result.gameplayStartCommitted;
          } else if constexpr (std::is_same_v<
                                   Command,
                                   battle::LoadBarrierDeadlineCommand>) {
            kind = RoomCommandKind::LoadBarrierDeadline;
            auto result = workflows::expireLoadBarrier(room_, battle_, command);
            battleCode = result.code;
            battleProjection = std::move(result.battle);
            gameplayStartCommitted = result.gameplayStartCommitted;
            code = result.code == battle::BattleLoadResultCode::Ok
                       ? lobby_room::RoomResultCode::Ok
                       : lobby_room::RoomResultCode::InvalidArgument;
          } else if constexpr (std::is_same_v<Command,
                                              battle::MovementTickCommand>) {
            kind = RoomCommandKind::MovementTick;
            auto result = workflows::advanceGameplayTick(battle_, command);
            movementCode = result.code;
            snapshot = std::move(result.snapshot);
            code = lobby_room::RoomResultCode::Ok;
          } else if constexpr (std::is_same_v<Command,
                                              battle::CombatDeadlineCommand>) {
            kind = RoomCommandKind::CombatDeadline;
            auto result = workflows::expireCombat(battle_, command, occurredAt);
            combatDeadlineCode = result.code;
            combat = std::move(result.combat);
            code = lobby_room::RoomResultCode::Ok;
          } else if constexpr (std::is_same_v<Command,
                                              battle::LootDeadlineCommand>) {
            kind = RoomCommandKind::LootDeadline;
            lootDeadlineCode =
                workflows::expireLoot(battle_, command, occurredAt).code;
            code = lobby_room::RoomResultCode::Ok;
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
                retainedLootResults_.push_back(
                    std::move(*result.retainedLootResults));
              }
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
  if (!battle_.has_value()) {
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
  if (!settlementBatch_.has_value() && battle_.has_value()) {
    settlementBatch_ = workflows::holdTerminalForSettlementDurability(
        room_, *battle_, committedAtNow());
    if (settlementBatch_.has_value() && storage_ != nullptr &&
        settlementAppendState_ == SettlementAppendState::None) {
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
  }
  if (battle_.has_value() && !settlementBatch_.has_value()) {
    const auto projection = battle_->projection();
    auto recovery = workflows::recoverResultGenerationFailure(
        room_, projection, battle_->resultProjection(),
        emittedResultFailureBattle_);
    if (recovery.has_value()) {
      recoveryNotice = recovery->notice;
      recoveryParticipants = std::move(recovery->participants);
      battleProjection = projection;
      battle_.reset();
      settlementReservation_.reset();
    }
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
      .movementCode = movementCode,
      .attackResult = std::move(attackResult),
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
        outcome = applyLocked(std::move(*command));
        const auto completedAt = std::chrono::steady_clock::now();
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
        ++processedCommands_;
        if (outcome->lane == CommandLane::External) {
          ++processedExternalCommands_;
        } else {
          ++processedControlCommands_;
        }
        lastProcessedOrdinal_ = outcome->admissionOrdinal;
      }
      submitPendingAppend();
      outcomeSink_(std::move(*outcome));
      ++processed;
      if (std::chrono::steady_clock::now() - startedAt >= budget_.maxWallTime) {
        break;
      }
    }

    std::lock_guard lock{mutex_};
    if (externalQueue_.empty() && controlQueue_.empty()) {
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
