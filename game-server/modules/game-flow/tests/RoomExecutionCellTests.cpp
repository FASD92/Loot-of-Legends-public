#include "execution/RoomExecutionCell.hpp"

#include <lol/battle/BattleProjections.hpp>
#include <lol/battle_continuity/BattleReplay.hpp>
#include <lol/battle_continuity/Durability.hpp>
#include <lol/battle_continuity/RecoveryEnvelopeCodec.hpp>
#include <lol/game_flow/GameplayTransportReadinessPort.hpp>
#include <lol/lobby_room/RoomApi.hpp>
#include <lol/runtime/DeadlineScheduler.hpp>
#include <lol/runtime/WorkerPool.hpp>
#include <lol/settlement/SettlementCapacityGate.hpp>
#include <lol/settlement/SettlementIntent.hpp>
#include <lol/settlement/SettlementPublication.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using lol::battle::BattleInputResultCode;
using lol::battle::BattleInstance;
using lol::battle::BattleLoadState;
using lol::battle::ClaimLootCommand;
using lol::battle::ClaimLootResultCode;
using lol::battle::CombatRuleset;
using lol::battle::CommandId;
using lol::battle::DropId;
using lol::battle_continuity::BattleIdentity;
using lol::battle_continuity::BattleRecording;
using lol::battle_continuity::CanonicalCommand;
using lol::battle_continuity::DurableTickCommitted;
using lol::battle_continuity::DurableTickSubmitResult;
using lol::battle_continuity::DurableTickWriteFailed;
using lol::battle_continuity::DurableTickWriteFailure;
using lol::battle_continuity::DurableTickWriteOutcome;
using lol::battle_continuity::DurableTickWritePort;
using lol::battle_continuity::DurableTickWriteRequest;
using lol::battle_continuity::RoomRecoveryPhase;
using lol::battle_continuity::RoomRecoveryState;
using lol::game_flow::BattleRecoveryReason;
using lol::game_flow::execution::CommandLane;
using lol::game_flow::execution::ConfirmedDisconnectCommand;
using lol::game_flow::execution::RecoveredRoomExecutionState;
using lol::game_flow::execution::RoomCellCommand;
using lol::game_flow::execution::RoomCommandAdmission;
using lol::game_flow::execution::RoomCommandEnvelope;
using lol::game_flow::execution::RoomCommandKind;
using lol::game_flow::execution::RoomCommandOutcome;
using lol::game_flow::execution::RoomControlCommand;
using lol::game_flow::execution::RoomControlEnvelope;
using lol::game_flow::execution::RoomExecutionCell;
using lol::game_flow::execution::WorkBudget;
using lol::lobby_room::CreateRoomCommand;
using lol::lobby_room::HostStartEligibilityCommand;
using lol::lobby_room::JoinRoomCommand;
using lol::lobby_room::KickRoomMemberCommand;
using lol::lobby_room::LeaveRoomCommand;
using lol::lobby_room::Room;
using lol::lobby_room::RoomLifecycle;
using lol::lobby_room::RoomMemberIdentity;
using lol::lobby_room::RoomResultCode;
using lol::lobby_room::SetReadyCommand;
using lol::runtime::DeadlineLease;
using lol::runtime::ThreadDeadlineScheduler;
using lol::runtime::ThreadDeadlineSchedulerConfig;
using lol::runtime::WorkerPool;
using lol::runtime::WorkerPoolConfig;
using lol::shared::AccountId;
using lol::shared::BattleInstanceId;
using lol::shared::RequestId;
using lol::shared::RoomId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;

using SettlementBattleOutcome = lol::settlement::BattleOutcome;
using lol::settlement::DurableAppendCompleted;
using lol::settlement::DurableAppendFailed;
using lol::settlement::DurableAppendOutcome;
using lol::settlement::DurableAppendRequest;
using lol::settlement::OutboxBacklogSnapshot;
using lol::settlement::SettlementCapacityGate;
using lol::settlement::SettlementStorageFailure;
using lol::settlement::SettlementStoragePort;
using lol::settlement::SubmitAppendResult;
using SettlementExitStatus = lol::settlement::ParticipantExitStatus;

AccountId account(std::uint64_t suffix) {
  AccountId::Bytes bytes{};
  bytes.back() = static_cast<std::uint8_t>(suffix);
  return AccountId{bytes};
}

std::optional<Room> makeRoom(std::uint64_t roomId) {
  auto created = Room::create(CreateRoomCommand{
      .roomId = RoomId{roomId},
      .title = "room-" + std::to_string(roomId),
      .capacity = 10,
      .creator =
          RoomMemberIdentity{
              .accountId = account(roomId),
              .sessionId = SessionId{roomId},
              .generation = SessionGeneration{1},
              .nickname = "host",
          },
  });
  return std::move(created.room);
}

std::optional<Room> makeRoomWithMember(std::uint64_t roomId,
                                       bool allReady = false) {
  auto room = makeRoom(roomId);
  if (!room.has_value() || room->join(JoinRoomCommand{RoomMemberIdentity{
                               .accountId = account(roomId + 1),
                               .sessionId = SessionId{roomId + 1},
                               .generation = SessionGeneration{1},
                               .nickname = "member",
                           }}) != RoomResultCode::Ok) {
    return std::nullopt;
  }
  if (allReady &&
      (room->setReady(SetReadyCommand{SessionId{roomId}, SessionGeneration{1},
                                      true}) != RoomResultCode::Ok ||
       room->setReady(SetReadyCommand{SessionId{roomId + 1},
                                      SessionGeneration{1}, true}) !=
           RoomResultCode::Ok)) {
    return std::nullopt;
  }
  return room;
}

RoomCommandEnvelope ready(std::uint64_t requestId, std::uint64_t sessionId,
                          bool value) {
  return RoomCommandEnvelope{
      .requestId = RequestId{requestId},
      .command = RoomCellCommand{SetReadyCommand{
          .sessionId = SessionId{sessionId},
          .generation = SessionGeneration{1},
          .ready = value,
      }},
  };
}

RoomCommandEnvelope join(std::uint64_t requestId, std::uint64_t sessionId) {
  return RoomCommandEnvelope{
      .requestId = RequestId{requestId},
      .command = RoomCellCommand{JoinRoomCommand{RoomMemberIdentity{
          .accountId = account(sessionId),
          .sessionId = SessionId{sessionId},
          .generation = SessionGeneration{1},
          .nickname = "joining-member",
      }}},
  };
}

RoomCommandEnvelope leave(std::uint64_t requestId, std::uint64_t sessionId) {
  return RoomCommandEnvelope{
      .requestId = RequestId{requestId},
      .command = RoomCellCommand{LeaveRoomCommand{
          .sessionId = SessionId{sessionId},
          .generation = SessionGeneration{1},
      }},
  };
}

RoomCommandEnvelope kick(std::uint64_t requestId, std::uint64_t hostId,
                         std::uint64_t targetId) {
  return RoomCommandEnvelope{
      .requestId = RequestId{requestId},
      .command = RoomCellCommand{KickRoomMemberCommand{
          .actorSessionId = SessionId{hostId},
          .actorGeneration = SessionGeneration{1},
          .targetSessionId = SessionId{targetId},
          .targetGeneration = SessionGeneration{1},
      }},
  };
}

RoomCommandEnvelope hostStart(std::uint64_t requestId, std::uint64_t hostId) {
  return RoomCommandEnvelope{
      .requestId = RequestId{requestId},
      .command = RoomCellCommand{HostStartEligibilityCommand{
          .actorSessionId = SessionId{hostId},
          .actorGeneration = SessionGeneration{1},
      }},
  };
}

RoomControlEnvelope disconnect(std::uint64_t sessionId) {
  return RoomControlEnvelope{
      .command = RoomControlCommand{ConfirmedDisconnectCommand{
          .sessionId = SessionId{sessionId},
          .generation = SessionGeneration{1},
      }},
  };
}

RoomControlEnvelope suspendBattleInput(std::uint64_t sessionId,
                                       std::uint64_t generation = 1) {
  return RoomControlEnvelope{
      .command = RoomControlCommand{lol::battle::SuspendBattleInputCommand{
          .sessionId = SessionId{sessionId},
          .generation = SessionGeneration{generation},
      }},
  };
}

RoomControlEnvelope resumeBattleInput(std::uint64_t sessionId,
                                      std::uint64_t generation = 1) {
  return RoomControlEnvelope{
      .command = RoomControlCommand{lol::battle::ResumeBattleInputCommand{
          .sessionId = SessionId{sessionId},
          .generation = SessionGeneration{generation},
      }},
  };
}

RoomCommandEnvelope loadComplete(std::uint64_t requestId,
                                 std::uint64_t sessionId,
                                 std::uint64_t roomId) {
  return RoomCommandEnvelope{
      .requestId = RequestId{requestId},
      .command = RoomCellCommand{lol::battle::ArenaLoadCompleteCommand{
          .sessionId = SessionId{sessionId},
          .generation = SessionGeneration{1},
          .roomId = RoomId{roomId},
          .battleId = BattleInstanceId{1},
      }},
  };
}

RoomCommandEnvelope attack(std::uint64_t requestId, std::uint64_t commandId,
                           std::uint64_t sessionId,
                           std::chrono::steady_clock::time_point receivedAt) {
  return RoomCommandEnvelope{
      .requestId = RequestId{requestId},
      .command = RoomCellCommand{lol::battle::AttackCommand{
          .commandId = CommandId{.high = 0, .low = commandId},
          .sessionId = SessionId{sessionId},
          .generation = SessionGeneration{1},
          .battleId = BattleInstanceId{1},
          .targetHint = CombatRuleset::monsterId,
      }},
      .receivedAt = receivedAt,
  };
}

RoomCommandEnvelope claimLoot(std::uint64_t requestId, std::uint64_t commandId,
                              std::uint64_t sessionId, std::uint64_t dropId,
                              std::chrono::steady_clock::time_point receivedAt,
                              std::uint64_t battleId = 1) {
  return RoomCommandEnvelope{
      .requestId = RequestId{requestId},
      .command = RoomCellCommand{ClaimLootCommand{
          .commandId = CommandId{.high = 0, .low = commandId},
          .sessionId = SessionId{sessionId},
          .generation = SessionGeneration{1},
          .battleId = BattleInstanceId{battleId},
          .dropId = DropId{dropId},
      }},
      .receivedAt = receivedAt,
  };
}

bool isAccepted(RoomCommandAdmission admission) {
  return admission == RoomCommandAdmission::Accepted;
}

class AlwaysReadyReadiness final
    : public lol::game_flow::GameplayTransportReadinessPort {
public:
  [[nodiscard]] bool isReady(SessionId,
                             SessionGeneration) const noexcept override {
    return true;
  }
};

class ManualDeadlineScheduler final : public lol::runtime::DeadlineScheduler {
public:
  explicit ManualDeadlineScheduler(
      std::size_t capacity = std::numeric_limits<std::size_t>::max())
      : capacity_(capacity) {}

  [[nodiscard]] bool scheduleAfter(std::chrono::milliseconds delay,
                                   Task task) override {
    if (delay.count() < 0 || !task ||
        unreservedPending_ + leases_.size() >= capacity_) {
      return false;
    }
    pending_.push_back(Pending{.due = now_ + delay,
                               .leaseId = 0,
                               .gate = nullptr,
                               .task = std::move(task)});
    ++unreservedPending_;
    sortAndMeasure();
    return true;
  }

  [[nodiscard]] std::unique_ptr<DeadlineLease> tryReserve() override {
    if (unreservedPending_ + leases_.size() >= capacity_) {
      return nullptr;
    }
    const auto id = nextLeaseId_++;
    leases_.push_back(LeaseRecord{.id = id, .gate = nullptr});
    return std::make_unique<Lease>(*this, id);
  }

  void advance(std::chrono::milliseconds elapsed) {
    now_ += elapsed;
    while (!pending_.empty() && pending_.front().due <= now_) {
      auto task = takeNextTask();
      if (task.has_value()) {
        (*task)();
      }
    }
  }

  [[nodiscard]] std::optional<Task> takeNextTask() {
    if (pending_.empty()) {
      return std::nullopt;
    }
    auto pending = std::move(pending_.front());
    pending_.erase(pending_.begin());
    if (pending.leaseId == 0) {
      --unreservedPending_;
      return std::move(pending.task);
    }
    const auto lease =
        std::ranges::find(leases_, pending.leaseId, &LeaseRecord::id);
    if (lease == leases_.end() || lease->gate != pending.gate) {
      return Task{[] {}};
    }
    lease->gate.reset();
    return Task{[gate = std::move(pending.gate),
                 task = std::move(pending.task)]() mutable {
      if (gate->exchange(false)) {
        task();
      }
    }};
  }

  [[nodiscard]] std::size_t pendingCount() const noexcept {
    return pending_.size();
  }

  [[nodiscard]] std::size_t maximumPending() const noexcept {
    return maximumPending_;
  }

private:
  class Lease final : public DeadlineLease {
  public:
    Lease(ManualDeadlineScheduler &owner, std::uint64_t id)
        : owner_(&owner), id_(id) {}

    ~Lease() override { owner_->releaseLease(id_); }

    [[nodiscard]] bool armAfter(std::chrono::milliseconds delay,
                                Task task) override {
      return owner_->armLease(id_, delay, std::move(task));
    }

    void cancel() noexcept override { owner_->cancelLease(id_); }

  private:
    ManualDeadlineScheduler *owner_;
    std::uint64_t id_;
  };

  struct Pending final {
    std::chrono::milliseconds due;
    std::uint64_t leaseId;
    std::shared_ptr<std::atomic_bool> gate;
    Task task;
  };

  struct LeaseRecord final {
    std::uint64_t id;
    std::shared_ptr<std::atomic_bool> gate;
  };

  [[nodiscard]] auto findLease(std::uint64_t id) {
    return std::ranges::find(leases_, id, &LeaseRecord::id);
  }

  [[nodiscard]] bool armLease(std::uint64_t id, std::chrono::milliseconds delay,
                              Task task) {
    if (delay.count() < 0 || !task) {
      return false;
    }
    const auto lease = findLease(id);
    if (lease == leases_.end()) {
      return false;
    }
    cancelLeaseTask(id);
    auto gate = std::make_shared<std::atomic_bool>(true);
    lease->gate = gate;
    pending_.push_back(Pending{.due = now_ + delay,
                               .leaseId = id,
                               .gate = std::move(gate),
                               .task = std::move(task)});
    sortAndMeasure();
    return true;
  }

  void cancelLease(std::uint64_t id) noexcept {
    const auto lease = findLease(id);
    if (lease == leases_.end()) {
      return;
    }
    cancelLeaseTask(id);
    lease->gate.reset();
  }

  void releaseLease(std::uint64_t id) noexcept {
    cancelLease(id);
    std::erase_if(leases_, [id](const auto &lease) { return lease.id == id; });
  }

  void cancelLeaseTask(std::uint64_t id) {
    for (const auto &task : pending_) {
      if (task.leaseId == id && task.gate) {
        task.gate->store(false);
      }
    }
    std::erase_if(pending_,
                  [id](const auto &task) { return task.leaseId == id; });
  }

  void sortAndMeasure() {
    maximumPending_ = std::max(maximumPending_, pending_.size());
    std::stable_sort(pending_.begin(), pending_.end(),
                     [](const Pending &left, const Pending &right) {
                       return left.due < right.due;
                     });
  }

  std::chrono::milliseconds now_{0};
  std::size_t capacity_;
  std::size_t maximumPending_{0};
  std::size_t unreservedPending_{0};
  std::uint64_t nextLeaseId_{1};
  std::vector<Pending> pending_;
  std::vector<LeaseRecord> leases_;
};

class Gate final {
public:
  void enterAndWait() {
    std::unique_lock lock{mutex_};
    ++entered_;
    changed_.notify_all();
    changed_.wait(lock, [this] { return open_; });
  }

  bool waitFor(std::size_t count, std::chrono::milliseconds timeout = 2s) {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout,
                             [this, count] { return entered_ >= count; });
  }

  void open() {
    std::lock_guard lock{mutex_};
    open_ = true;
    changed_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::size_t entered_{0};
  bool open_{false};
};

class OutcomeCollector final {
public:
  void add(RoomCommandOutcome outcome) {
    std::lock_guard lock{mutex_};
    outcomes_.push_back(std::move(outcome));
  }

  std::vector<RoomCommandOutcome> take() {
    std::lock_guard lock{mutex_};
    return std::exchange(outcomes_, {});
  }

private:
  std::mutex mutex_;
  std::vector<RoomCommandOutcome> outcomes_;
};

class ManualSettlementStorage final : public SettlementStoragePort {
public:
  SubmitAppendResult submit(DurableAppendRequest request,
                            CompletionSink completion) override {
    request_ = std::move(request);
    completion_ = std::move(completion);
    return submitResult_;
  }

  void setSubmitResult(SubmitAppendResult result) { submitResult_ = result; }

  const std::optional<DurableAppendRequest> &request() const noexcept {
    return request_;
  }

  bool complete(DurableAppendOutcome outcome) {
    if (!completion_) {
      return false;
    }
    completion_(std::move(outcome));
    return true;
  }

private:
  SubmitAppendResult submitResult_{SubmitAppendResult::Accepted};
  std::optional<DurableAppendRequest> request_;
  CompletionSink completion_;
};

class ManualContinuityStorage final : public DurableTickWritePort {
public:
  void setSubmitResult(DurableTickSubmitResult result) {
    submitResult_ = result;
  }

  DurableTickSubmitResult submit(DurableTickWriteRequest request,
                                 CompletionSink completion) override {
    std::lock_guard lock{mutex_};
    request_ = std::move(request);
    completion_ = std::move(completion);
    changed_.notify_all();
    return submitResult_;
  }

  bool waitForRequest(std::chrono::milliseconds timeout = 2s) {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout,
                             [this] { return request_.has_value(); });
  }

  std::optional<DurableTickWriteRequest> request() const {
    std::lock_guard lock{mutex_};
    return request_;
  }

  bool deliver(DurableTickWriteOutcome outcome) {
    CompletionSink completion;
    {
      std::lock_guard lock{mutex_};
      if (!request_.has_value() || !completion_) {
        return false;
      }
      completion = completion_;
    }
    completion(std::move(outcome));
    return true;
  }

  bool complete() {
    CompletionSink completion;
    std::optional<DurableTickCommitted> committed;
    {
      std::lock_guard lock{mutex_};
      if (!request_.has_value() || !completion_) {
        return false;
      }
      committed = DurableTickCommitted{
          .identity = request_->batch.identity,
          .writerRecoveryEpoch = request_->batch.writerRecoveryEpoch,
          .lastRecordSequence = request_->batch.lastRecordSequence,
      };
      request_.reset();
      completion = std::move(completion_);
    }
    completion(DurableTickWriteOutcome{*committed});
    return true;
  }

  bool fail(DurableTickWriteFailure failure) {
    CompletionSink completion;
    std::optional<DurableTickWriteFailed> failed;
    {
      std::lock_guard lock{mutex_};
      if (!request_.has_value() || !completion_) {
        return false;
      }
      failed = DurableTickWriteFailed{
          .identity = request_->batch.identity,
          .writerRecoveryEpoch = request_->batch.writerRecoveryEpoch,
          .lastRecordSequence = request_->batch.lastRecordSequence,
          .failure = failure,
      };
      request_.reset();
      completion = std::move(completion_);
    }
    completion(DurableTickWriteOutcome{*failed});
    return true;
  }

private:
  DurableTickSubmitResult submitResult_{DurableTickSubmitResult::Accepted};
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::optional<DurableTickWriteRequest> request_;
  CompletionSink completion_;
};

void markStorageHealthy(SettlementCapacityGate &gate) {
  gate.updateBacklog(OutboxBacklogSnapshot{
      .unretiredRecords = 0u,
      .unretiredBytes = 0u,
      .oldestPendingAge = 0ms,
      .storageHealthy = true,
  });
}

std::shared_ptr<RoomExecutionCell>
makeDurableCell(WorkerPool &pool, ManualDeadlineScheduler &deadlines,
                AlwaysReadyReadiness &readiness, SettlementCapacityGate &gate,
                ManualSettlementStorage &storage, std::optional<Room> room,
                OutcomeCollector &collector) {
  if (!room.has_value()) {
    return nullptr;
  }
  return RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 128, .maxWallTime = 20ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      &readiness, &gate, &storage);
}

bool startGameplay(RoomExecutionCell &cell, OutcomeCollector &collector,
                   std::uint64_t roomId) {
  if (!isAccepted(cell.enqueue(hostStart(1, roomId))) ||
      !cell.waitUntilIdle(2s) ||
      !isAccepted(cell.enqueue(loadComplete(2, roomId, roomId))) ||
      !isAccepted(cell.enqueue(loadComplete(3, roomId + 1, roomId))) ||
      !cell.waitUntilIdle(2s)) {
    return false;
  }
  const auto outcomes = collector.take();
  return outcomes.size() == 3 && outcomes.back().gameplayStartCommitted &&
         outcomes.back().detail.has_value() &&
         outcomes.back().detail->lifecycle == RoomLifecycle::InProgress;
}

bool continuityFailureFencesAlreadyAcceptedCommands() {
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 16}};
  AlwaysReadyReadiness readiness;
  Gate blocker;
  OutcomeCollector collector;
  auto room = makeRoomWithMember(260, true);
  if (!room.has_value() ||
      !pool.submit([&blocker] { blocker.enterAndWait(); }) ||
      !blocker.waitFor(1)) {
    blocker.open();
    return false;
  }
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 64, .maxWallTime = 10ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      &readiness, nullptr, nullptr, 1U);
  if (!isAccepted(cell->enqueue(hostStart(1, 260))) ||
      !isAccepted(cell->enqueue(leave(2, 260)))) {
    blocker.open();
    return false;
  }
  blocker.open();
  if (!cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto outcomes = collector.take();
  const auto detail = cell->detail();
  return outcomes.size() == 1U && outcomes.front().recoveryNotice.has_value() &&
         outcomes.front().recoveryNotice->reason ==
             BattleRecoveryReason::ContinuityRecordingFailed &&
         detail.has_value() && detail->members.size() == 2U &&
         cell->enqueue(ready(3, 260, false)) ==
             RoomCommandAdmission::RoomOverloaded;
}

bool authoritativeOutcomeWaitsForContinuityDurability() {
  constexpr std::uint64_t roomValue = (7ULL << 32U) | 1ULL;
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 16}};
  AlwaysReadyReadiness readiness;
  ManualContinuityStorage continuity;
  OutcomeCollector collector;
  auto room = makeRoomWithMember(roomValue, true);
  if (!room.has_value()) {
    return false;
  }
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 64, .maxWallTime = 10ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      &readiness, nullptr, nullptr, 7U, &continuity);
  if (!isAccepted(cell->enqueue(hostStart(1, roomValue))) ||
      !continuity.waitForRequest() || !collector.take().empty()) {
    return false;
  }
  const auto request = continuity.request();
  const auto journal =
      request.has_value()
          ? lol::battle_continuity::decodeJournal(request->batch.encodedRecords)
          : lol::battle_continuity::JournalDecodeResult{};
  const auto checkpoint =
      journal.ok()
          ? std::ranges::find_if(
                journal.records,
                [](const auto &record) {
                  return record.header.recordType ==
                         lol::battle_continuity::RecordType::Checkpoint;
                })
          : decltype(journal.records)::const_iterator{};
  const auto *checkpointPayload =
      checkpoint != journal.records.end()
          ? std::get_if<lol::battle_continuity::CheckpointPayload>(
                &checkpoint->payload)
          : nullptr;
  const auto initialState =
      checkpointPayload != nullptr
          ? lol::battle_continuity::decodeRecoveryState(
                checkpointPayload->canonicalStateBytes)
          : lol::battle_continuity::RecoveryStateDecodeResult{};
  const auto privateEnvelope =
      request.has_value() && request->privateEnvelopePlaintext.has_value()
          ? lol::battle_continuity::decodeBattleRecoveryEnvelope(
                *request->privateEnvelopePlaintext)
          : lol::battle_continuity::RecoveryEnvelopeDecodeResult{};
  if (!request.has_value() ||
      request->batch.identity.roomId != RoomId{roomValue} ||
      request->batch.writerRecoveryEpoch != 7U ||
      request->batch.firstRecordSequence != 1U ||
      request->batch.lastRecordSequence != 3U || request->batch.terminal ||
      request->batch.encodedRecords.empty() || !privateEnvelope.ok() ||
      !privateEnvelope.envelope.has_value() ||
      privateEnvelope.envelope->participants.size() != 2U ||
      privateEnvelope.envelope->participants.front().previousGeneration !=
          SessionGeneration{1} ||
      !initialState.ok() || !initialState.state.has_value() ||
      initialState.state->room.roomId != RoomId{roomValue} ||
      initialState.state->room.capacity != 10U ||
      initialState.state->room.hostParticipantSlot != 1U ||
      initialState.state->room.memberSlots !=
          std::vector<std::uint16_t>{1U, 2U} ||
      initialState.state->room.phase != RoomRecoveryPhase::Loading ||
      initialState.state->room.nextBattleOrdinal != 2U ||
      !continuity.complete() || !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto durable = collector.take();
  return durable.size() == 1U &&
         durable.front().kind == RoomCommandKind::HostStartEligibility &&
         durable.front().battle.has_value() &&
         durable.front().battle->state == BattleLoadState::LoadBarrierOpen;
}

bool roomMemberMutationChangesRecordedCompositeHash() {
  constexpr std::uint64_t roomValue = (12ULL << 32U) | 1ULL;
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 16}};
  AlwaysReadyReadiness readiness;
  ManualContinuityStorage continuity;
  OutcomeCollector collector;
  auto room = makeRoomWithMember(roomValue, true);
  if (!room.has_value()) {
    return false;
  }
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 64, .maxWallTime = 10ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      &readiness, nullptr, nullptr, 12U, &continuity);
  if (!isAccepted(cell->enqueue(hostStart(1, roomValue))) ||
      !continuity.waitForRequest()) {
    return false;
  }
  const auto initial = continuity.request();
  if (!initial.has_value() || !continuity.complete() ||
      !cell->waitUntilIdle(2s) || collector.take().size() != 1U ||
      !isAccepted(cell->enqueue(leave(2, roomValue))) ||
      !continuity.waitForRequest()) {
    return false;
  }
  const auto mutated = continuity.request();
  if (!mutated.has_value()) {
    return false;
  }
  auto committedJournal = initial->batch.encodedRecords;
  committedJournal.insert(committedJournal.end(),
                          mutated->batch.encodedRecords.begin(),
                          mutated->batch.encodedRecords.end());
  const auto journal = lol::battle_continuity::decodeJournal(committedJournal);
  if (!journal.ok()) {
    return false;
  }
  const auto decision =
      std::ranges::find_if(journal.records, [](const auto &record) {
        return record.header.recordType ==
               lol::battle_continuity::RecordType::CommandDecision;
      });
  if (decision == journal.records.end()) {
    return false;
  }
  const auto *payload =
      std::get_if<lol::battle_continuity::CommandDecisionPayload>(
          &decision->payload);
  const auto roomState =
      payload != nullptr
          ? lol::battle_continuity::decodeRoomRecoveryState(
                payload->roomRecoveryStateBytes)
          : lol::battle_continuity::RoomRecoveryStateDecodeResult{};
  const auto initialCheckpoint =
      std::ranges::find_if(journal.records, [](const auto &record) {
        return record.header.recordType ==
               lol::battle_continuity::RecordType::Checkpoint;
      });
  const auto *initialCheckpointPayload =
      initialCheckpoint != journal.records.end()
          ? std::get_if<lol::battle_continuity::CheckpointPayload>(
                &initialCheckpoint->payload)
          : nullptr;
  const auto initialState =
      initialCheckpointPayload != nullptr
          ? lol::battle_continuity::decodeRecoveryState(
                initialCheckpointPayload->canonicalStateBytes)
          : lol::battle_continuity::RecoveryStateDecodeResult{};
  return payload != nullptr && roomState.ok() && roomState.state.has_value() &&
         roomState.state->hostParticipantSlot == 2U &&
         roomState.state->memberSlots == std::vector<std::uint16_t>{2U} &&
         roomState.state->phase == RoomRecoveryPhase::Loading &&
         roomState.state->nextBattleOrdinal == 2U && initialState.ok() &&
         initialState.state.has_value() &&
         initialCheckpointPayload != nullptr &&
         payload->postDecisionStateHash != initialCheckpointPayload->stateHash;
}

bool loadCancellationIsDurableBeforeBattleRelease() {
  constexpr std::uint32_t epoch = 9U;
  constexpr std::uint64_t roomValue =
      (static_cast<std::uint64_t>(epoch) << 32U) | 1ULL;
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 32}};
  AlwaysReadyReadiness readiness;
  SettlementCapacityGate gate;
  markStorageHealthy(gate);
  ManualSettlementStorage settlement;
  ManualContinuityStorage continuity;
  OutcomeCollector collector;
  auto room = makeRoomWithMember(roomValue, true);
  if (!room.has_value()) {
    return false;
  }
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 64, .maxWallTime = 10ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      &readiness, &gate, &settlement, epoch, &continuity);
  if (!isAccepted(cell->enqueue(hostStart(1, roomValue))) ||
      !continuity.waitForRequest()) {
    return false;
  }
  const auto initial = continuity.request();
  if (!initial.has_value() || !continuity.complete() ||
      !cell->waitUntilIdle(2s) || collector.take().size() != 1U ||
      gate.metrics().reservedBatches != 1U) {
    return false;
  }

  deadlines.advance(10s);
  if (!continuity.waitForRequest()) {
    return false;
  }
  const auto cancelled = continuity.request();
  if (!cancelled.has_value() || !collector.take().empty() ||
      gate.metrics().reservedBatches != 1U) {
    return false;
  }
  auto journal = initial->batch.encodedRecords;
  journal.insert(journal.end(), cancelled->batch.encodedRecords.begin(),
                 cancelled->batch.encodedRecords.end());
  const auto replay =
      lol::battle_continuity::BattleReplayer::replayJournal(journal);
  if (!replay.ok() || !replay.battle.has_value() ||
      replay.battle->projection().state != BattleLoadState::LoadCancelled ||
      !continuity.complete() || !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto outcomes = collector.take();
  return outcomes.size() == 1U && outcomes.front().detail.has_value() &&
         outcomes.front().detail->lifecycle == RoomLifecycle::Open &&
         outcomes.front().battle.has_value() &&
         outcomes.front().battle->state == BattleLoadState::LoadCancelled &&
         !cell->battleJournal().has_value() &&
         gate.metrics().reservedBatches == 0U;
}

bool continuityWriteFailureSuppressesSuccessAndFencesQueuedWork() {
  constexpr std::uint64_t roomValue = (8ULL << 32U) | 1ULL;
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 16}};
  AlwaysReadyReadiness readiness;
  ManualContinuityStorage continuity;
  OutcomeCollector collector;
  auto room = makeRoomWithMember(roomValue, true);
  if (!room.has_value()) {
    return false;
  }
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 64, .maxWallTime = 10ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      &readiness, nullptr, nullptr, 8U, &continuity);
  if (!isAccepted(cell->enqueue(hostStart(1, roomValue))) ||
      !continuity.waitForRequest() ||
      !isAccepted(cell->enqueue(leave(2, roomValue))) ||
      !collector.take().empty() ||
      !continuity.fail(DurableTickWriteFailure::IoFailure) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto outcomes = collector.take();
  const auto detail = cell->detail();
  return outcomes.size() == 1U &&
         outcomes.front().kind == RoomCommandKind::ContinuityTickFailed &&
         outcomes.front().recoveryNotice.has_value() &&
         outcomes.front().recoveryNotice->reason ==
             BattleRecoveryReason::ContinuityRecordingFailed &&
         detail.has_value() && detail->members.size() == 2U &&
         cell->enqueue(ready(3, roomValue, false)) ==
             RoomCommandAdmission::RoomOverloaded;
}

bool continuityQueueFullSuppressesSuccessAndFencesQueuedWork() {
  constexpr std::uint64_t roomValue = (13ULL << 32U) | 1ULL;
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 16}};
  AlwaysReadyReadiness readiness;
  ManualContinuityStorage continuity;
  continuity.setSubmitResult(DurableTickSubmitResult::QueueFull);
  OutcomeCollector collector;
  auto room = makeRoomWithMember(roomValue, true);
  if (!room.has_value()) {
    return false;
  }
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 64, .maxWallTime = 10ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      &readiness, nullptr, nullptr, 13U, &continuity);
  if (!isAccepted(cell->enqueue(hostStart(1, roomValue))) ||
      !isAccepted(cell->enqueue(leave(2, roomValue))) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto outcomes = collector.take();
  const auto detail = cell->detail();
  const auto notices =
      std::ranges::count_if(outcomes, [](const RoomCommandOutcome &outcome) {
        return outcome.recoveryNotice.has_value() &&
               outcome.recoveryNotice->reason ==
                   BattleRecoveryReason::ContinuityRecordingFailed;
      });
  const auto hostStartSucceeded =
      std::ranges::any_of(outcomes, [](const RoomCommandOutcome &outcome) {
        return outcome.kind == RoomCommandKind::HostStartEligibility &&
               outcome.code == RoomResultCode::Ok;
      });
  return !hostStartSucceeded && outcomes.size() == 1U && notices == 1U &&
         outcomes.front().kind == RoomCommandKind::ContinuityTickFailed &&
         detail.has_value() && detail->members.size() == 2U &&
         cell->enqueue(ready(3, roomValue, false)) ==
             RoomCommandAdmission::RoomOverloaded;
}

bool staleContinuityCommitDoesNotFailCloseOrReleaseHeldOutcome() {
  constexpr std::uint64_t roomValue = (10ULL << 32U) | 1ULL;
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 16}};
  AlwaysReadyReadiness readiness;
  ManualContinuityStorage continuity;
  OutcomeCollector collector;
  auto room = makeRoomWithMember(roomValue, true);
  if (!room.has_value()) {
    return false;
  }
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 64, .maxWallTime = 10ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      &readiness, nullptr, nullptr, 10U, &continuity);
  if (!isAccepted(cell->enqueue(hostStart(1, roomValue))) ||
      !continuity.waitForRequest() || !collector.take().empty()) {
    return false;
  }
  const auto request = continuity.request();
  if (!request.has_value()) {
    return false;
  }
  auto staleIdentity = request->batch.identity;
  staleIdentity.battleInstanceId = BattleInstanceId{2};
  if (!continuity.deliver(DurableTickWriteOutcome{DurableTickCommitted{
          .identity = staleIdentity,
          .writerRecoveryEpoch = request->batch.writerRecoveryEpoch,
          .lastRecordSequence = request->batch.lastRecordSequence,
      }}) ||
      !pool.waitUntilIdle(2s)) {
    return false;
  }
  const auto stale = collector.take();
  const auto detail = cell->detail();
  if (stale.size() != 1U ||
      stale.front().kind != RoomCommandKind::ContinuityTickCommitted ||
      stale.front().code != RoomResultCode::InvalidArgument ||
      stale.front().recoveryNotice.has_value() ||
      !stale.front().battle.has_value() ||
      stale.front().battle->battleId != BattleInstanceId{1} ||
      stale.front().battle->state != BattleLoadState::LoadBarrierOpen ||
      !detail.has_value() || detail->lifecycle != RoomLifecycle::Loading ||
      cell->lifecycle() != RoomLifecycle::Loading ||
      cell->settlementBatch().has_value()) {
    return false;
  }
  if (!continuity.complete() || !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto released = collector.take();
  return released.size() == 1U &&
         released.front().kind == RoomCommandKind::HostStartEligibility &&
         released.front().code == RoomResultCode::Ok &&
         released.front().battle.has_value() &&
         released.front().battle->battleId == BattleInstanceId{1};
}

bool staleContinuityWriteFailureDoesNotFailCloseOrReleaseHeldOutcome() {
  constexpr std::uint64_t roomValue = (11ULL << 32U) | 1ULL;
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 16}};
  AlwaysReadyReadiness readiness;
  ManualContinuityStorage continuity;
  OutcomeCollector collector;
  auto room = makeRoomWithMember(roomValue, true);
  if (!room.has_value()) {
    return false;
  }
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 64, .maxWallTime = 10ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      &readiness, nullptr, nullptr, 11U, &continuity);
  if (!isAccepted(cell->enqueue(hostStart(1, roomValue))) ||
      !continuity.waitForRequest() || !collector.take().empty()) {
    return false;
  }
  const auto request = continuity.request();
  if (!request.has_value()) {
    return false;
  }
  auto staleIdentity = request->batch.identity;
  staleIdentity.battleInstanceId = BattleInstanceId{2};
  if (!continuity.deliver(DurableTickWriteOutcome{DurableTickWriteFailed{
          .identity = staleIdentity,
          .writerRecoveryEpoch = request->batch.writerRecoveryEpoch,
          .lastRecordSequence = request->batch.lastRecordSequence,
          .failure = DurableTickWriteFailure::IoFailure,
      }}) ||
      !pool.waitUntilIdle(2s)) {
    return false;
  }
  const auto stale = collector.take();
  const auto detail = cell->detail();
  if (stale.size() != 1U ||
      stale.front().kind != RoomCommandKind::ContinuityTickFailed ||
      stale.front().code != RoomResultCode::InvalidArgument ||
      stale.front().recoveryNotice.has_value() ||
      !stale.front().battle.has_value() ||
      stale.front().battle->battleId != BattleInstanceId{1} ||
      stale.front().battle->state != BattleLoadState::LoadBarrierOpen ||
      !detail.has_value() || detail->lifecycle != RoomLifecycle::Loading ||
      cell->lifecycle() != RoomLifecycle::Loading ||
      cell->settlementBatch().has_value()) {
    return false;
  }
  if (!continuity.complete() || !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto released = collector.take();
  return released.size() == 1U &&
         released.front().kind == RoomCommandKind::HostStartEligibility &&
         released.front().code == RoomResultCode::Ok &&
         released.front().battle.has_value() &&
         released.front().battle->battleId == BattleInstanceId{1};
}

bool recoveredCellContinuesSequenceAndRearmsLogicalDeadline() {
  constexpr std::uint64_t roomValue = (4ULL << 32U) | 1ULL;
  auto room = makeRoomWithMember(roomValue, true);
  if (!room.has_value()) {
    return false;
  }
  const auto eligibility = room->prepareHostStart(
      HostStartEligibilityCommand{SessionId{roomValue}, SessionGeneration{1}});
  if (!eligibility.admission.has_value() ||
      room->commitLoading(*eligibility.admission) != RoomResultCode::Ok) {
    return false;
  }
  std::vector<lol::battle::BattleStartCandidate> candidates;
  for (const auto &member : eligibility.admission->members) {
    candidates.push_back(lol::battle::BattleStartCandidate{
        .accountId = member.accountId,
        .sessionId = member.sessionId,
        .generation = member.sessionGeneration,
        .nickname = member.nickname,
    });
  }
  auto created = BattleInstance::create(lol::battle::BattleAdmissionSnapshot{
      .roomId = RoomId{roomValue},
      .battleId = BattleInstanceId{1},
      .candidates = std::move(candidates),
      .rulesetVersion = lol::battle::battleRulesetVersion,
      .seed = 91U,
  });
  if (!created.battle.has_value() ||
      created.battle->openLoadBarrier(lol::battle::BattleTime{}) !=
          lol::battle::BattleLoadResultCode::Ok) {
    return false;
  }
  auto battle = std::move(*created.battle);
  const RoomRecoveryState roomState{
      .roomId = RoomId{roomValue},
      .capacity = 10U,
      .hostParticipantSlot = 1U,
      .memberSlots = {1U, 2U},
      .phase = RoomRecoveryPhase::Loading,
      .nextBattleOrdinal = 2U,
  };
  auto initial = BattleRecording::start(
      battle,
      BattleIdentity{.originRecoveryEpoch = 4U,
                     .roomId = RoomId{roomValue},
                     .battleInstanceId = BattleInstanceId{1}},
      4U, roomState);
  if (!initial.has_value() || !initial->takePendingBatch().has_value()) {
    return false;
  }
  const std::vector<lol::battle_continuity::Record> records{
      initial->records().begin(), initial->records().end()};
  auto resumed = BattleRecording::resume(battle, records, 5U);
  if (!resumed.has_value()) {
    return false;
  }

  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 16}};
  AlwaysReadyReadiness readiness;
  SettlementCapacityGate gate;
  markStorageHealthy(gate);
  auto reservation = gate.tryReserve();
  ManualSettlementStorage settlement;
  ManualContinuityStorage continuity;
  OutcomeCollector collector;
  if (!reservation.reservation.has_value()) {
    return false;
  }
  auto cell = RoomExecutionCell::createRecovered(
      pool, deadlines,
      RecoveredRoomExecutionState{
          .room = std::move(*room),
          .battle = std::move(battle),
          .recording = std::move(*resumed),
          .nextBattleOrdinal = 2U,
          .settlementBatch = std::nullopt,
          .settlementReservation = std::move(reservation.reservation),
      },
      WorkBudget{.maxCommands = 64, .maxWallTime = 10ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      &readiness, &gate, &settlement, 5U, &continuity);
  if (!cell.has_value() || deadlines.pendingCount() != 0U ||
      !(*cell)->activateRecovered() || deadlines.pendingCount() != 1U ||
      !isAccepted((*cell)->enqueue(loadComplete(1, roomValue, roomValue))) ||
      !continuity.waitForRequest()) {
    return false;
  }
  const auto request = continuity.request();
  if (!request.has_value() || request->batch.firstRecordSequence != 4U ||
      request->batch.lastRecordSequence != 6U ||
      request->batch.writerRecoveryEpoch != 5U || !collector.take().empty() ||
      !continuity.complete() || !(*cell)->waitUntilIdle(2s)) {
    return false;
  }
  const auto outcomes = collector.take();
  if (outcomes.size() != 1U ||
      outcomes.front().kind != RoomCommandKind::ArenaLoadComplete ||
      !isAccepted(
          (*cell)->enqueue(loadComplete(2U, roomValue + 1U, roomValue))) ||
      !continuity.waitForRequest() || !collector.take().empty() ||
      !continuity.complete() || !(*cell)->waitUntilIdle(2s))
    return false;
  collector.take();
  const auto at = std::chrono::steady_clock::now() + 100ms;
  if (!isAccepted((*cell)->enqueue(RoomCommandEnvelope{
          .requestId = RequestId{3U},
          .command = RoomCellCommand{lol::battle::MoveCommand{
              SessionId{roomValue}, SessionGeneration{1U}, BattleInstanceId{1U},
              1U, lol::battle::DirectionIntent{1, 0, 0}}},
          .receivedAt = at})) ||
      !(*cell)->waitUntilIdle(2s) || continuity.request().has_value() ||
      collector.take().empty())
    return false;
  if (!isAccepted((*cell)->enqueueControl(RoomControlEnvelope{
          .command = RoomControlCommand{lol::battle::MovementTickCommand{
              BattleInstanceId{1U}, 2U}},
          .occurredAt = at})) ||
      !(*cell)->waitUntilIdle(2s) || continuity.request().has_value())
    return false;
  const auto tick = collector.take();
  if (tick.size() != 1U || !tick.front().snapshot.has_value() ||
      !isAccepted((*cell)->enqueueControl(suspendBattleInput(roomValue))) ||
      !continuity.waitForRequest() || !collector.take().empty())
    return false;
  return continuity.complete() && (*cell)->waitUntilIdle(2s) &&
         collector.take().size() == 1U;
}

bool recoveredActivationSerializesDeadlineAndRetirement() {
  enum class DeadlinePhase { Loading, Combat, Loot };

  const auto runCase = [](DeadlinePhase phase, bool retireBeforeActivation) {
    const auto epoch = static_cast<std::uint64_t>(
        phase == DeadlinePhase::Loading  ? (retireBeforeActivation ? 17U : 14U)
        : phase == DeadlinePhase::Combat ? 15U
                                         : 16U);
    const auto roomValue = (epoch << 32U) | 1U;
    auto room = makeRoomWithMember(roomValue, true);
    if (!room.has_value()) {
      return false;
    }
    const auto eligibility = room->prepareHostStart(HostStartEligibilityCommand{
        SessionId{roomValue}, SessionGeneration{1}});
    if (!eligibility.admission.has_value() ||
        room->commitLoading(*eligibility.admission) != RoomResultCode::Ok) {
      return false;
    }

    std::vector<lol::battle::BattleStartCandidate> candidates;
    candidates.reserve(eligibility.admission->members.size());
    for (const auto &member : eligibility.admission->members) {
      candidates.push_back(lol::battle::BattleStartCandidate{
          .accountId = member.accountId,
          .sessionId = member.sessionId,
          .generation = member.sessionGeneration,
          .nickname = member.nickname,
      });
    }
    auto created = BattleInstance::create(lol::battle::BattleAdmissionSnapshot{
        .roomId = RoomId{roomValue},
        .battleId = BattleInstanceId{1},
        .candidates = std::move(candidates),
        .rulesetVersion = lol::battle::battleRulesetVersion,
        .seed = 91U,
    });
    if (created.code != lol::battle::BattleLoadResultCode::Ok ||
        !created.battle.has_value() ||
        created.battle->openLoadBarrier(
            lol::battle::BattleTime::fromLogicalTick(0U)) !=
            lol::battle::BattleLoadResultCode::Ok) {
      return false;
    }
    auto battle = std::move(*created.battle);
    const BattleIdentity battleIdentity{
        .originRecoveryEpoch = static_cast<std::uint32_t>(epoch),
        .roomId = RoomId{roomValue},
        .battleInstanceId = BattleInstanceId{1}};
    RoomRecoveryState roomState{
        .roomId = RoomId{roomValue},
        .capacity = 10U,
        .hostParticipantSlot = 1U,
        .memberSlots = {1U, 2U},
        .phase = RoomRecoveryPhase::Loading,
        .nextBattleOrdinal = 2U,
    };
    auto recording = BattleRecording::start(
        battle, battleIdentity, static_cast<std::uint32_t>(epoch), roomState);
    if (!recording.has_value() || !recording->takePendingBatch().has_value()) {
      return false;
    }
    const auto recordDecision =
        [&](CanonicalCommand command, const auto &before,
            const RoomRecoveryState &beforeRoom,
            const RoomRecoveryState &afterRoom, std::uint16_t decisionCode) {
          const bool accepted =
              recording->recordDecision(command, before, beforeRoom, battle,
                                        afterRoom, decisionCode, std::nullopt);
          // This fixture explicitly supplies a fully persisted legacy journal
          // to restore/deadline tests. New runtime writes need not occur every
          // tick.
          (void)recording->takePendingBatch();
          return accepted;
        };
    const auto load = [&](const auto &member, std::uint64_t tick) {
      return battle.completeLoad(
                 lol::battle::ArenaLoadCompleteCommand{
                     .sessionId = member.sessionId,
                     .generation = member.sessionGeneration,
                     .roomId = RoomId{roomValue},
                     .battleId = BattleInstanceId{1},
                 },
                 true, lol::battle::BattleTime::fromLogicalTick(tick)) ==
             lol::battle::BattleLoadResultCode::Ok;
    };

    RoomCommandKind expectedKind = RoomCommandKind::LoadBarrierDeadline;
    RoomLifecycle expectedLifecycle = RoomLifecycle::Open;
    if (phase == DeadlinePhase::Loading) {
      const auto before = battle.exportDeterministicState();
      if (!load(eligibility.admission->members.front(), 200U) ||
          !recordDecision(CanonicalCommand::arenaLoadComplete(1U), before,
                          roomState, roomState,
                          static_cast<std::uint16_t>(
                              lol::battle::BattleLoadResultCode::Ok))) {
        return false;
      }
    } else {
      const auto firstBefore = battle.exportDeterministicState();
      if (!load(eligibility.admission->members[0], 0U) ||
          !recordDecision(CanonicalCommand::arenaLoadComplete(1U), firstBefore,
                          roomState, roomState,
                          static_cast<std::uint16_t>(
                              lol::battle::BattleLoadResultCode::Ok))) {
        return false;
      }
      const auto secondBefore = battle.exportDeterministicState();
      if (!load(eligibility.admission->members[1], 0U) ||
          room->commitInProgress() != RoomResultCode::Ok) {
        return false;
      }
      auto inProgressRoomState = roomState;
      inProgressRoomState.phase = RoomRecoveryPhase::InProgress;
      if (!recordDecision(CanonicalCommand::arenaLoadComplete(2U), secondBefore,
                          roomState, inProgressRoomState,
                          static_cast<std::uint16_t>(
                              lol::battle::BattleLoadResultCode::Ok))) {
        return false;
      }
      roomState = inProgressRoomState;
      expectedLifecycle = RoomLifecycle::AwaitingSettlementDurability;
      if (phase == DeadlinePhase::Combat) {
        const auto before = battle.exportDeterministicState();
        if (battle.integrateMovement(
                      lol::battle::MovementTickCommand{BattleInstanceId{1}, 1U},
                      lol::battle::BattleTime::fromLogicalTick(600U))
                    != lol::battle::MovementResultCode::Ok ||
            !recordDecision(CanonicalCommand::movementTick(1U), before,
                            roomState, roomState,
                            static_cast<std::uint16_t>(
                                lol::battle::MovementResultCode::Ok))) {
          return false;
        }
        expectedKind = RoomCommandKind::CombatDeadline;
      } else {
        constexpr auto attackCount =
            lol::battle::CombatRuleset::monsterHitPointsForParticipants(2) /
            lol::battle::CombatRuleset::attackDamage;
        for (std::uint64_t index = 0U; index < attackCount; ++index) {
          const auto before = battle.exportDeterministicState();
          const auto execution = battle.attackWithApplied(
              lol::battle::AttackCommand{
                  .commandId =
                      lol::battle::CommandId{.high = 0U, .low = index + 1U},
                  .sessionId = SessionId{roomValue},
                  .generation = SessionGeneration{1},
                  .battleId = BattleInstanceId{1},
                  .targetHint = lol::battle::CombatRuleset::monsterId,
              },
              lol::battle::BattleTime::fromLogicalTick(index * 16U));
          if (execution.result.code != lol::battle::AttackResultCode::Ok ||
              !execution.applied.has_value() ||
              !recordDecision(
                  CanonicalCommand::attack(
                      1U,
                      lol::battle_continuity::CommandId{.high = 0U,
                                                        .low = index + 1U},
                      lol::battle::CombatRuleset::monsterId),
                  before, roomState, roomState,
                  static_cast<std::uint16_t>(execution.result.code))) {
            return false;
          }
        }
        const auto state = battle.exportDeterministicState();
        const auto before = state;
        if (!state.lootDeadlineTick.has_value() ||
            battle.integrateMovement(
                      lol::battle::MovementTickCommand{BattleInstanceId{1}, 1U},
                      lol::battle::BattleTime::fromLogicalTick(
                          *state.lootDeadlineTick))
                    != lol::battle::MovementResultCode::Ok ||
            !recordDecision(CanonicalCommand::movementTick(1U), before,
                            roomState, roomState,
                            static_cast<std::uint16_t>(
                                lol::battle::MovementResultCode::Ok))) {
          return false;
        }
        expectedKind = RoomCommandKind::LootDeadline;
      }
    }

    ManualDeadlineScheduler deadlines;
    WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 16}};
    AlwaysReadyReadiness readiness;
    SettlementCapacityGate gate;
    markStorageHealthy(gate);
    auto reservation = gate.tryReserve();
    ManualSettlementStorage settlement;
    ManualContinuityStorage continuity;
    OutcomeCollector collector;
    if (!reservation.reservation.has_value()) {
      return false;
    }
    auto cell = RoomExecutionCell::createRecovered(
        pool, deadlines,
        RecoveredRoomExecutionState{
            .room = std::move(*room),
            .battle = std::move(battle),
            .recording = std::move(*recording),
            .nextBattleOrdinal = 2U,
            .settlementBatch = std::nullopt,
            .settlementReservation = std::move(reservation.reservation),
        },
        WorkBudget{.maxCommands = 64, .maxWallTime = 10ms},
        [&collector](RoomCommandOutcome outcome) {
          collector.add(std::move(outcome));
        },
        &readiness, &gate, &settlement, static_cast<std::uint32_t>(epoch),
        &continuity);
    if (!cell.has_value()) {
      return false;
    }
    auto recovered = *cell;
    const auto beforeJournal = recovered->battleJournal();
    if (!beforeJournal.has_value() || deadlines.pendingCount() != 0U ||
        continuity.request().has_value() || !collector.take().empty() ||
        recovered->enqueue(loadComplete(1U, roomValue, roomValue)) !=
            RoomCommandAdmission::RoomOverloaded ||
        recovered->battleJournal() != beforeJournal ||
        recovered->metrics().processedCommands != 0U) {
      return false;
    }
    if (retireBeforeActivation) {
      recovered->retire();
      const auto metrics = recovered->metrics();
      return !recovered->activateRecovered() &&
             deadlines.pendingCount() == 0U &&
             !continuity.request().has_value() && collector.take().empty() &&
             !settlement.request().has_value() &&
             metrics.processedCommands == 0U &&
             recovered->enqueue(loadComplete(2U, roomValue, roomValue)) ==
                 RoomCommandAdmission::RoomRetired &&
             recovered->enqueueControl(disconnect(roomValue)) ==
                 RoomCommandAdmission::RoomRetired;
    }

    if (!recovered->activateRecovered() || deadlines.pendingCount() != 1U) {
      return false;
    }
    deadlines.advance(0ms);
    if (!continuity.waitForRequest() || !collector.take().empty() ||
        deadlines.pendingCount() != 0U || !continuity.complete() ||
        !recovered->waitUntilIdle(2s)) {
      return false;
    }
    const auto outcomes = collector.take();
    return outcomes.size() == 1U && outcomes.front().kind == expectedKind &&
           outcomes.front().detail.has_value() &&
           outcomes.front().detail->lifecycle == expectedLifecycle &&
           deadlines.pendingCount() == 0U && !continuity.request().has_value();
  };

  const auto loading = runCase(DeadlinePhase::Loading, false);
  const auto combat = loading && runCase(DeadlinePhase::Combat, false);
  const auto loot = combat && runCase(DeadlinePhase::Loot, false);
  const auto retired = loot && runCase(DeadlinePhase::Loading, true);
  if (!retired)
    std::fprintf(stderr,
                 "activation phases: loading=%d combat=%d loot=%d retired=%d\n",
                 loading, combat, loot, retired);
  return retired;
}

bool suspendAndResumeBattleInputUsesTheCellMailbox() {
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 16}};
  AlwaysReadyReadiness readiness;
  OutcomeCollector collector;
  auto room = makeRoomWithMember(250, true);
  if (!room.has_value()) {
    return false;
  }
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 64, .maxWallTime = 10ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      &readiness);
  if (!cell || !startGameplay(*cell, collector, 250) ||
      !isAccepted(cell->enqueueControl(suspendBattleInput(250))) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto suspended = collector.take();
  if (suspended.size() != 1 ||
      suspended.front().kind != RoomCommandKind::SuspendBattleInput ||
      suspended.front().inputControlCode != BattleInputResultCode::Ok ||
      !suspended.front().battle.has_value() ||
      suspended.front().battle->state != BattleLoadState::GameplayCommitted) {
    return false;
  }
  if (!isAccepted(cell->enqueueControl(resumeBattleInput(250))) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto resumed = collector.take();
  return resumed.size() == 1 &&
         resumed.front().kind == RoomCommandKind::ResumeBattleInput &&
         resumed.front().inputControlCode == BattleInputResultCode::Ok &&
         resumed.front().resumeProjection.has_value() &&
         resumed.front().resumeProjection->roomId == RoomId{250} &&
         resumed.front().resumeProjection->battleId == BattleInstanceId{1} &&
         resumed.front().resumeProjection->sessionId == SessionId{250} &&
         resumed.front().resumeProjection->inputEnabled &&
         cell->lifecycle() == RoomLifecycle::InProgress;
}

bool hasTerminalHold(const RoomExecutionCell &cell, std::uint64_t roomId,
                     SettlementBattleOutcome outcome,
                     SettlementExitStatus exitStatus,
                     bool allowZeroBattleElapsed = false) {
  const auto batch = cell.settlementBatch();
  if (cell.lifecycle() != RoomLifecycle::AwaitingSettlementDurability ||
      !batch.has_value() || batch->roomId() != RoomId{roomId} ||
      batch->battleId() != BattleInstanceId{1} || batch->outcome() != outcome ||
      batch->catalogVersion() != 1 ||
      batch->committedAt().unixEpochMilliseconds == 0 ||
      (!allowZeroBattleElapsed &&
       batch->committedAt().monotonicNanoseconds == 0) ||
      batch->intents().size() != 2) {
    return false;
  }
  for (const auto &intent : batch->intents()) {
    if (intent.exitStatus() != exitStatus || !intent.itemDeltas().empty() ||
        intent.finalAssetValue() != 0) {
      return false;
    }
  }
  return true;
}

bool remainsHeldAfterLateCommand(RoomExecutionCell &cell,
                                 OutcomeCollector &collector,
                                 std::uint64_t requestId,
                                 std::uint64_t sessionId) {
  const auto before = cell.settlementBatch();
  if (!before.has_value() ||
      !isAccepted(cell.enqueue(ready(requestId, sessionId, false))) ||
      !cell.waitUntilIdle(2s)) {
    return false;
  }
  const auto outcomes = collector.take();
  return outcomes.size() == 1 &&
         outcomes.front().code == RoomResultCode::RoomClosed &&
         (!outcomes.front().detail.has_value() ||
          outcomes.front().detail->lifecycle != RoomLifecycle::Open) &&
         cell.lifecycle() == RoomLifecycle::AwaitingSettlementDurability &&
         cell.settlementBatch() == before;
}

std::optional<std::vector<RoomCommandOutcome>>
runCommands(std::optional<Room> room,
            std::vector<RoomCommandEnvelope> commands) {
  if (!room.has_value()) {
    return std::nullopt;
  }
  ThreadDeadlineScheduler deadlines{
      ThreadDeadlineSchedulerConfig{.queueCapacity = 16}};
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 8}};
  OutcomeCollector collector;
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 64, .maxWallTime = 2ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      });
  for (auto &command : commands) {
    if (!isAccepted(cell->enqueue(std::move(command)))) {
      return std::nullopt;
    }
  }
  if (!cell->waitUntilIdle(2s)) {
    return std::nullopt;
  }
  return collector.take();
}

bool budgetReschedulesAndPreservesOrder() {
  ThreadDeadlineScheduler deadlines{
      ThreadDeadlineSchedulerConfig{.queueCapacity = 16}};
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 16}};
  Gate blocker;
  if (!pool.submit([&blocker] { blocker.enterAndWait(); }) ||
      !blocker.waitFor(1)) {
    blocker.open();
    return false;
  }

  auto room = makeRoom(1);
  if (!room.has_value()) {
    blocker.open();
    return false;
  }
  OutcomeCollector collector;
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 1, .maxWallTime = 2ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      });
  if (!isAccepted(cell->enqueue(ready(1, 1, true))) ||
      !isAccepted(cell->enqueue(ready(2, 1, false))) ||
      !isAccepted(cell->enqueue(ready(3, 1, true)))) {
    blocker.open();
    return false;
  }

  blocker.open();
  if (!cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto outcomes = collector.take();
  const auto metrics = cell->metrics();
  const auto detail = cell->detail();
  return outcomes.size() == 3 && outcomes[0].requestId == RequestId{1} &&
         outcomes[1].requestId == RequestId{2} &&
         outcomes[2].requestId == RequestId{3} &&
         outcomes[0].code == RoomResultCode::Ok &&
         outcomes[1].code == RoomResultCode::Ok &&
         outcomes[2].code == RoomResultCode::Ok &&
         metrics.processedCommands == 3 && metrics.rescheduleCount == 2 &&
         metrics.maximumConcurrentRuns == 1 &&
         metrics.schedulingFailures == 0 && detail.has_value() &&
         detail->members.front().ready;
}

bool oneCellHasOnlyOneActiveWorker() {
  ThreadDeadlineScheduler deadlines{
      ThreadDeadlineSchedulerConfig{.queueCapacity = 16}};
  WorkerPool pool{WorkerPoolConfig{.threadCount = 4, .queueCapacity = 32}};
  auto room = makeRoom(10);
  if (!room.has_value()) {
    return false;
  }
  Gate sinkGate;
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 1, .maxWallTime = 2ms},
      [&sinkGate](RoomCommandOutcome) { sinkGate.enterAndWait(); });
  if (!isAccepted(cell->enqueue(ready(1, 10, true))) || !sinkGate.waitFor(1)) {
    sinkGate.open();
    return false;
  }

  std::atomic<std::uint64_t> nextRequest{2};
  std::atomic<bool> allAccepted{true};
  std::vector<std::thread> producers;
  for (std::size_t producer = 0; producer < 3; ++producer) {
    producers.emplace_back([&cell, &nextRequest, &allAccepted] {
      for (std::size_t command = 0; command < 10; ++command) {
        const auto request = nextRequest.fetch_add(1);
        if (!isAccepted(cell->enqueue(ready(request, 10, request % 2 == 0)))) {
          allAccepted.store(false);
        }
      }
    });
  }
  for (auto &producer : producers) {
    producer.join();
  }
  const auto blockedMetrics = cell->metrics();
  sinkGate.open();
  if (!allAccepted.load() || blockedMetrics.activeRuns != 1 ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto metrics = cell->metrics();
  return metrics.processedCommands == 31 &&
         metrics.maximumConcurrentRuns == 1 && metrics.schedulingFailures == 0;
}

bool twoCellsRunInParallel() {
  ThreadDeadlineScheduler deadlines{
      ThreadDeadlineSchedulerConfig{.queueCapacity = 16}};
  WorkerPool pool{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 8}};
  auto firstRoom = makeRoom(20);
  auto secondRoom = makeRoom(30);
  if (!firstRoom.has_value() || !secondRoom.has_value()) {
    return false;
  }

  Gate bothSinks;
  auto sink = [&bothSinks](RoomCommandOutcome) { bothSinks.enterAndWait(); };
  auto first = RoomExecutionCell::create(
      pool, deadlines, std::move(*firstRoom),
      WorkBudget{.maxCommands = 64, .maxWallTime = 2ms}, sink);
  auto second = RoomExecutionCell::create(
      pool, deadlines, std::move(*secondRoom),
      WorkBudget{.maxCommands = 64, .maxWallTime = 2ms}, sink);
  if (!isAccepted(first->enqueue(ready(1, 20, true))) ||
      !isAccepted(second->enqueue(ready(2, 30, true))) ||
      !bothSinks.waitFor(2)) {
    bothSinks.open();
    return false;
  }
  const auto poolMetrics = pool.metrics();
  bothSinks.open();
  if (poolMetrics.activeTasks != 2 || !first->waitUntilIdle(2s) ||
      !second->waitUntilIdle(2s)) {
    return false;
  }
  const auto firstDetail = first->detail();
  const auto secondDetail = second->detail();
  return first->metrics().maximumConcurrentRuns == 1 &&
         second->metrics().maximumConcurrentRuns == 1 &&
         firstDetail.has_value() && secondDetail.has_value() &&
         firstDetail->members.front().ready &&
         secondDetail->members.front().ready;
}

bool mailboxSaturationPreservesControlReserve() {
  ThreadDeadlineScheduler deadlines{
      ThreadDeadlineSchedulerConfig{.queueCapacity = 16}};
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 16}};
  Gate blocker;
  if (!pool.submit([&blocker] { blocker.enterAndWait(); }) ||
      !blocker.waitFor(1)) {
    blocker.open();
    return false;
  }
  auto room = makeRoom(40);
  if (!room.has_value()) {
    blocker.open();
    return false;
  }
  OutcomeCollector collector;
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 64, .maxWallTime = 2ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      });

  for (std::uint64_t request = 1; request <= 224; ++request) {
    if (!isAccepted(cell->enqueue(ready(request, 40, request % 2 == 0)))) {
      blocker.open();
      return false;
    }
  }
  if (cell->enqueue(ready(225, 40, true)) !=
      RoomCommandAdmission::RoomOverloaded) {
    blocker.open();
    return false;
  }
  for (std::uint64_t control = 1; control <= 32; ++control) {
    if (!isAccepted(cell->enqueueControl(disconnect(1'000 + control)))) {
      blocker.open();
      return false;
    }
  }
  if (cell->enqueueControl(disconnect(2'000)) !=
      RoomCommandAdmission::ControlReserveExhausted) {
    blocker.open();
    return false;
  }

  const auto saturated = cell->metrics();
  if (saturated.queueDepth != 256 || saturated.externalQueueDepth != 224 ||
      saturated.controlQueueDepth != 32 ||
      saturated.internalReserveHighWatermark != 32 ||
      saturated.externalRejections != 1 ||
      saturated.controlAdmissionFailures != 1) {
    blocker.open();
    return false;
  }

  blocker.open();
  if (!cell->waitUntilIdle(4s)) {
    return false;
  }
  const auto outcomes = collector.take();
  const auto metrics = cell->metrics();
  if (outcomes.size() != 256 || metrics.processedCommands != 256 ||
      metrics.processedExternalCommands != 224 ||
      metrics.processedControlCommands != 32 ||
      metrics.lastProcessedOrdinal != 256) {
    return false;
  }
  for (std::size_t index = 0; index < outcomes.size(); ++index) {
    if (outcomes[index].admissionOrdinal != index + 1 ||
        (index < 224 && outcomes[index].lane != CommandLane::External) ||
        (index >= 224 &&
         outcomes[index].lane != CommandLane::InternalControl)) {
      return false;
    }
  }
  return true;
}

bool mailboxLanesMergeByAdmissionOrdinal() {
  ThreadDeadlineScheduler deadlines{
      ThreadDeadlineSchedulerConfig{.queueCapacity = 16}};
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 16}};
  Gate blocker;
  if (!pool.submit([&blocker] { blocker.enterAndWait(); }) ||
      !blocker.waitFor(1)) {
    blocker.open();
    return false;
  }
  auto room = makeRoom(50);
  if (!room.has_value()) {
    blocker.open();
    return false;
  }
  OutcomeCollector collector;
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 64, .maxWallTime = 2ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      });
  if (!isAccepted(cell->enqueue(ready(1, 50, true))) ||
      !isAccepted(cell->enqueueControl(disconnect(1'001))) ||
      !isAccepted(cell->enqueue(ready(2, 50, false))) ||
      !isAccepted(cell->enqueueControl(disconnect(1'002)))) {
    blocker.open();
    return false;
  }
  blocker.open();
  if (!cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto outcomes = collector.take();
  const std::vector expectedLanes{
      CommandLane::External,
      CommandLane::InternalControl,
      CommandLane::External,
      CommandLane::InternalControl,
  };
  if (outcomes.size() != expectedLanes.size()) {
    return false;
  }
  for (std::size_t index = 0; index < outcomes.size(); ++index) {
    if (outcomes[index].admissionOrdinal != index + 1 ||
        outcomes[index].lane != expectedLanes[index]) {
      return false;
    }
  }
  return true;
}

bool roomCommandRacesFollowMailboxOrder() {
  const auto joinBeforeStart = runCommands(makeRoomWithMember(60, true),
                                           {join(1, 62), hostStart(2, 60)});
  if (!joinBeforeStart.has_value() || joinBeforeStart->size() != 2 ||
      (*joinBeforeStart)[0].code != RoomResultCode::Ok ||
      (*joinBeforeStart)[1].code != RoomResultCode::NotAllReady) {
    return false;
  }

  const auto startBeforeJoin = runCommands(makeRoomWithMember(70, true),
                                           {hostStart(3, 70), join(4, 72)});
  if (!startBeforeJoin.has_value() || startBeforeJoin->size() != 2 ||
      (*startBeforeJoin)[0].code != RoomResultCode::Ok ||
      !(*startBeforeJoin)[0].admission.has_value() ||
      (*startBeforeJoin)[0].admission->members.size() != 2 ||
      (*startBeforeJoin)[1].code != RoomResultCode::RoomClosed ||
      !(*startBeforeJoin)[1].detail.has_value() ||
      (*startBeforeJoin)[1].detail->members.size() != 2) {
    return false;
  }

  const auto readyBeforeLeave =
      runCommands(makeRoomWithMember(80), {ready(5, 81, true), leave(6, 81)});
  if (!readyBeforeLeave.has_value() || readyBeforeLeave->size() != 2 ||
      (*readyBeforeLeave)[0].code != RoomResultCode::Ok ||
      (*readyBeforeLeave)[1].code != RoomResultCode::Ok) {
    return false;
  }

  const auto leaveBeforeReady =
      runCommands(makeRoomWithMember(90), {leave(7, 91), ready(8, 91, true)});
  if (!leaveBeforeReady.has_value() || leaveBeforeReady->size() != 2 ||
      (*leaveBeforeReady)[0].code != RoomResultCode::Ok ||
      (*leaveBeforeReady)[1].code != RoomResultCode::NotInRoom) {
    return false;
  }

  const auto kickBeforeLeave =
      runCommands(makeRoomWithMember(100), {kick(9, 100, 101), leave(10, 101)});
  if (!kickBeforeLeave.has_value() || kickBeforeLeave->size() != 2 ||
      (*kickBeforeLeave)[0].code != RoomResultCode::Ok ||
      (*kickBeforeLeave)[1].code != RoomResultCode::NotInRoom) {
    return false;
  }

  const auto leaveBeforeKick = runCommands(
      makeRoomWithMember(110), {leave(11, 111), kick(12, 110, 111)});
  return leaveBeforeKick.has_value() && leaveBeforeKick->size() == 2 &&
         (*leaveBeforeKick)[0].code == RoomResultCode::Ok &&
         (*leaveBeforeKick)[1].code == RoomResultCode::InvalidTarget;
}

bool hostStartCommitsOneBattleAndRoomLoading() {
  const auto outcomes = runCommands(makeRoomWithMember(115, true),
                                    {hostStart(13, 115), hostStart(14, 115)});
  if (!outcomes.has_value() || outcomes->size() != 2) {
    return false;
  }
  const auto &first = outcomes->front();
  const auto &second = outcomes->back();
  return first.code == RoomResultCode::Ok && first.admission.has_value() &&
         first.admission->members.front().accountId == account(115) &&
         first.detail.has_value() &&
         first.detail->lifecycle == RoomLifecycle::Loading &&
         first.battle.has_value() &&
         first.battle->battleId == BattleInstanceId{1} &&
         first.battle->state == BattleLoadState::LoadBarrierOpen &&
         second.code == RoomResultCode::RoomClosed &&
         second.detail.has_value() &&
         second.detail->lifecycle == RoomLifecycle::Loading &&
         second.battle.has_value() &&
         second.battle->battleId == BattleInstanceId{1};
}

bool hostStartReservesCapacityAndLoadCancelReleasesIt() {
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 32}};
  SettlementCapacityGate gate;
  gate.updateBacklog(OutboxBacklogSnapshot{
      .unretiredRecords = 0u,
      .unretiredBytes = 0u,
      .oldestPendingAge = 0ms,
      .storageHealthy = true,
  });
  auto room = makeRoomWithMember(116, true);
  if (!room.has_value()) {
    return false;
  }
  OutcomeCollector collector;
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 64, .maxWallTime = 2ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      nullptr, &gate);
  if (!isAccepted(cell->enqueue(hostStart(1, 116))) ||
      !cell->waitUntilIdle(2s) || gate.metrics().reservedBatches != 1u) {
    return false;
  }
  const auto started = collector.take();
  if (started.size() != 1u || started.front().code != RoomResultCode::Ok) {
    return false;
  }

  deadlines.advance(10s);
  if (!cell->waitUntilIdle(2s) || gate.metrics().reservedBatches != 0u) {
    return false;
  }
  const auto cancelled = collector.take();
  return cancelled.size() == 1u && cancelled.front().detail.has_value() &&
         cancelled.front().detail->lifecycle == RoomLifecycle::Open;
}

bool hostStartRejectsUnhealthyStorageWithoutRoomMutation() {
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 32}};
  SettlementCapacityGate gate;
  gate.updateBacklog(OutboxBacklogSnapshot{
      .unretiredRecords = 0u,
      .unretiredBytes = 0u,
      .oldestPendingAge = 0ms,
      .storageHealthy = false,
  });
  auto room = makeRoomWithMember(117, true);
  if (!room.has_value()) {
    return false;
  }
  OutcomeCollector collector;
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 64, .maxWallTime = 2ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      nullptr, &gate);
  if (!isAccepted(cell->enqueue(hostStart(1, 117))) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto outcomes = collector.take();
  return outcomes.size() == 1u &&
         outcomes.front().code == RoomResultCode::RoomOverloaded &&
         outcomes.front().detail.has_value() &&
         outcomes.front().detail->lifecycle == RoomLifecycle::Open &&
         !outcomes.front().battle.has_value() &&
         gate.metrics().reservedBatches == 0u;
}

bool hostStartRejectsFullCriticalControlReservationWithoutMutation() {
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 64}};
  Gate blocker;
  if (!pool.submit([&blocker] { blocker.enterAndWait(); }) ||
      !blocker.waitFor(1)) {
    blocker.open();
    return false;
  }

  SettlementCapacityGate gate;
  markStorageHealthy(gate);
  auto room = makeRoomWithMember(118, true);
  if (!room.has_value()) {
    blocker.open();
    return false;
  }
  OutcomeCollector collector;
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 64, .maxWallTime = 20ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      nullptr, &gate);

  if (!isAccepted(cell->enqueue(hostStart(1, 118)))) {
    blocker.open();
    return false;
  }
  for (std::uint64_t index = 0; index < 32; ++index) {
    if (!isAccepted(cell->enqueueControl(disconnect(1000 + index)))) {
      blocker.open();
      return false;
    }
  }
  blocker.open();
  if (!cell->waitUntilIdle(2s) || !pool.waitUntilIdle(2s)) {
    return false;
  }

  const auto rejected = collector.take();
  const auto host = std::ranges::find_if(rejected, [](const auto &outcome) {
    return outcome.requestId == RequestId{1};
  });
  if (host == rejected.end() || host->code != RoomResultCode::RoomOverloaded ||
      !host->detail.has_value() ||
      host->detail->lifecycle != RoomLifecycle::Open ||
      host->battle.has_value() || gate.metrics().reservedBatches != 0u ||
      deadlines.pendingCount() != 0u) {
    return false;
  }

  if (!isAccepted(cell->enqueue(hostStart(2, 118))) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto retried = collector.take();
  return retried.size() == 1u && retried.front().code == RoomResultCode::Ok &&
         retried.front().battle.has_value() &&
         retried.front().battle->battleId == BattleInstanceId{1} &&
         gate.metrics().reservedBatches == 1u && deadlines.pendingCount() == 1u;
}

bool schedulerExhaustionRecoversAllHostStartReservations() {
  ManualDeadlineScheduler deadlines{1};
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 32}};
  if (!deadlines.scheduleAfter(1h, [] {})) {
    return false;
  }
  SettlementCapacityGate gate;
  markStorageHealthy(gate);
  auto room = makeRoomWithMember(119, true);
  if (!room.has_value()) {
    return false;
  }
  OutcomeCollector collector;
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 64, .maxWallTime = 2ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      nullptr, &gate);

  if (!isAccepted(cell->enqueue(hostStart(1, 119))) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto rejected = collector.take();
  if (rejected.size() != 1u ||
      rejected.front().code != RoomResultCode::RoomOverloaded ||
      !rejected.front().detail.has_value() ||
      rejected.front().detail->lifecycle != RoomLifecycle::Open ||
      rejected.front().battle.has_value() ||
      gate.metrics().reservedBatches != 0u || deadlines.pendingCount() != 1u) {
    return false;
  }

  deadlines.advance(1h);
  if (!isAccepted(cell->enqueue(hostStart(2, 119))) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto retried = collector.take();
  return retried.size() == 1u && retried.front().code == RoomResultCode::Ok &&
         retried.front().battle.has_value() &&
         retried.front().battle->battleId == BattleInstanceId{1} &&
         gate.metrics().reservedBatches == 1u && deadlines.pendingCount() == 1u;
}

bool canceledLoadCallbackCannotMutateOrEmitAfterCombatRearm() {
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 32}};
  AlwaysReadyReadiness readiness;
  auto room = makeRoomWithMember(121, true);
  if (!room.has_value()) {
    return false;
  }
  OutcomeCollector collector;
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 64, .maxWallTime = 2ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      &readiness);

  if (!isAccepted(cell->enqueue(hostStart(1, 121))) ||
      !cell->waitUntilIdle(2s) || deadlines.pendingCount() != 1u) {
    return false;
  }
  collector.take();
  auto canceledLoad = deadlines.takeNextTask();
  if (!canceledLoad.has_value() ||
      !isAccepted(cell->enqueue(loadComplete(2, 121, 121))) ||
      !isAccepted(cell->enqueue(loadComplete(3, 122, 121))) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto committed = collector.take();
  if (committed.size() != 2u || !committed.back().gameplayStartCommitted ||
      deadlines.pendingCount() != 1u) {
    return false;
  }

  (*canceledLoad)();
  if (!cell->waitUntilIdle(2s)) {
    return false;
  }
  return collector.take().empty() &&
         cell->lifecycle() == RoomLifecycle::InProgress &&
         deadlines.pendingCount() == 1u;
}

bool loadCombatLootRearmKeepsOneActiveDeadline() {
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 128}};
  AlwaysReadyReadiness readiness;
  auto room = makeRoomWithMember(122, true);
  if (!room.has_value()) {
    return false;
  }
  OutcomeCollector collector;
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 128, .maxWallTime = 20ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      &readiness);
  if (!startGameplay(*cell, collector, 122) || deadlines.pendingCount() != 1u ||
      deadlines.maximumPending() != 1u) {
    return false;
  }

  const auto startedAt =
      std::chrono::steady_clock::time_point{std::chrono::hours{1}};
  constexpr auto attackCount =
      CombatRuleset::monsterHitPointsForParticipants(2) /
      CombatRuleset::attackDamage;
  for (std::uint64_t index = 0; index < attackCount; ++index) {
    if (!isAccepted(cell->enqueue(
            attack(10 + index, index + 1, 122, startedAt + index * 800ms)))) {
      return false;
    }
  }
  if (!cell->waitUntilIdle(4s)) {
    return false;
  }
  const auto attacks = collector.take();
  return attacks.size() == attackCount && attacks.back().lootResolutionOpened &&
         deadlines.pendingCount() == 1u && deadlines.maximumPending() == 1u;
}

bool criticalDeadlineRetriesAfterWorkerCapacityReturns() {
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 1}};
  AlwaysReadyReadiness readiness;
  auto room = makeRoomWithMember(123, true);
  if (!room.has_value()) {
    return false;
  }
  OutcomeCollector collector;
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 128, .maxWallTime = 20ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      &readiness);
  if (!startGameplay(*cell, collector, 123)) {
    return false;
  }

  Gate blocker;
  if (!pool.submit([&blocker] { blocker.enterAndWait(); }) ||
      !blocker.waitFor(1) || !pool.submit([] {})) {
    blocker.open();
    return false;
  }
  deadlines.advance(30s);
  blocker.open();
  if (!pool.waitUntilIdle(2s)) {
    return false;
  }

  deadlines.advance(1ms);
  if (!cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto outcomes = collector.take();
  const auto deadlineCount =
      std::ranges::count_if(outcomes, [](const RoomCommandOutcome &outcome) {
        return outcome.kind == RoomCommandKind::CombatDeadline;
      });
  return deadlineCount == 1 && cell->metrics().schedulingFailures == 1u &&
         deadlines.pendingCount() == 0u;
}

bool rescheduleFailureDrainsAcceptedWork() {
  ThreadDeadlineScheduler deadlines{
      ThreadDeadlineSchedulerConfig{.queueCapacity = 16}};
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 1}};
  auto room = makeRoom(120);
  if (!room.has_value()) {
    return false;
  }
  Gate firstOutcome;
  OutcomeCollector collector;
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 1, .maxWallTime = 2ms},
      [&collector, &firstOutcome](RoomCommandOutcome outcome) {
        const bool first = outcome.requestId == RequestId{1};
        collector.add(std::move(outcome));
        if (first) {
          firstOutcome.enterAndWait();
        }
      });
  if (!isAccepted(cell->enqueue(ready(1, 120, true))) ||
      !isAccepted(cell->enqueue(ready(2, 120, false))) ||
      !firstOutcome.waitFor(1) || !pool.submit([] {})) {
    firstOutcome.open();
    return false;
  }
  firstOutcome.open();
  if (!cell->waitUntilIdle(2s) || !pool.waitUntilIdle(2s)) {
    return false;
  }
  const auto outcomes = collector.take();
  const auto metrics = cell->metrics();
  const auto detail = cell->detail();
  return outcomes.size() == 2 && outcomes[0].requestId == RequestId{1} &&
         outcomes[1].requestId == RequestId{2} &&
         metrics.processedCommands == 2 && metrics.rescheduleCount == 1 &&
         metrics.schedulingFailures == 1 && detail.has_value() &&
         !detail->members.front().ready;
}

bool workerPoolRejectsOverflowAndDrainsAcceptedWork() {
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 1}};
  Gate blocker;
  std::atomic<std::size_t> completed{0};
  if (!pool.submit([&blocker] { blocker.enterAndWait(); }) ||
      !blocker.waitFor(1) ||
      !pool.submit([&completed] { completed.fetch_add(1); }) ||
      pool.submit([&completed] { completed.fetch_add(1); })) {
    blocker.open();
    return false;
  }
  blocker.open();
  pool.stop();
  pool.stop();
  const auto metrics = pool.metrics();
  return completed.load() == 1 && metrics.acceptedTasks == 2 &&
         metrics.rejectedTasks == 1 && metrics.completedTasks == 2 &&
         metrics.pendingTasks == 0 && metrics.activeTasks == 0;
}

// The real host-start/load-complete path schedules the 30000 ms combat
// deadline. A logically retired cell (strong handle retained) rejects later
// admissions as RoomRetired, and firing the scheduler callbacks produces zero
// combat-deadline outcome and zero mutation.
bool retiredCellCancelsPendingCombatDeadline() {
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 16}};
  AlwaysReadyReadiness readiness;
  auto room = makeRoomWithMember(160, true);
  if (!room.has_value()) {
    return false;
  }
  OutcomeCollector collector;
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 64, .maxWallTime = 2ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      &readiness);
  if (!isAccepted(cell->enqueue(hostStart(1, 160))) ||
      !cell->waitUntilIdle(2s) ||
      !isAccepted(cell->enqueue(loadComplete(2, 160, 160))) ||
      !isAccepted(cell->enqueue(loadComplete(3, 161, 160))) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto committed = collector.take();
  if (committed.size() != 3 || !committed.back().gameplayStartCommitted) {
    return false;
  }

  cell->retire();
  if (cell->enqueue(ready(4, 160, false)) !=
          RoomCommandAdmission::RoomRetired ||
      cell->enqueueControl(disconnect(160)) !=
          RoomCommandAdmission::RoomRetired) {
    return false;
  }

  // Fire the pending 10000 ms load barrier and 30000 ms combat deadline
  // callbacks; both must be no-ops on the retired cell.
  deadlines.advance(30s);
  if (!cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto metrics = cell->metrics();
  return collector.take().empty() && metrics.processedCommands == 3 &&
         metrics.processedControlCommands == 0 &&
         metrics.lastProcessedOrdinal == 3;
}

bool combatTimeoutWaitsForSettlementDurability() {
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 32}};
  AlwaysReadyReadiness readiness;
  auto room = makeRoomWithMember(170, true);
  if (!room.has_value()) {
    return false;
  }
  OutcomeCollector collector;
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 128, .maxWallTime = 2ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      &readiness);
  if (!startGameplay(*cell, collector, 170)) {
    return false;
  }

  deadlines.advance(30s);
  if (!cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto terminal = collector.take();
  const auto timeout = std::find_if(
      terminal.begin(), terminal.end(), [](const RoomCommandOutcome &outcome) {
        return outcome.kind == RoomCommandKind::CombatDeadline;
      });
  if (timeout == terminal.end() || !timeout->detail.has_value() ||
      timeout->detail->lifecycle !=
          RoomLifecycle::AwaitingSettlementDurability ||
      !hasTerminalHold(*cell, 170, SettlementBattleOutcome::CombatTimeout,
                       SettlementExitStatus::TerminalPresent)) {
    return false;
  }
  return remainsHeldAfterLateCommand(*cell, collector, 4, 170);
}

bool cancellationWaitsForSettlementDurability() {
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 32}};
  AlwaysReadyReadiness readiness;
  auto room = makeRoomWithMember(180, true);
  if (!room.has_value()) {
    return false;
  }
  OutcomeCollector collector;
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 128, .maxWallTime = 2ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      &readiness);
  if (!startGameplay(*cell, collector, 180) ||
      !isAccepted(cell->enqueue(leave(4, 180))) ||
      !isAccepted(cell->enqueueControl(disconnect(181))) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto terminal = collector.take();
  if (terminal.size() != 2 ||
      terminal.back().kind != RoomCommandKind::ConfirmedDisconnect ||
      terminal.back().detail.has_value() ||
      !hasTerminalHold(*cell, 180,
                       SettlementBattleOutcome::CancelledNoActiveParticipants,
                       SettlementExitStatus::TerminalExited, true)) {
    return false;
  }
  return remainsHeldAfterLateCommand(*cell, collector, 5, 180);
}

bool monsterDefeatedWaitsForSettlementDurability() {
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 128}};
  AlwaysReadyReadiness readiness;
  auto room = makeRoomWithMember(190, true);
  if (!room.has_value()) {
    return false;
  }
  OutcomeCollector collector;
  auto cell = RoomExecutionCell::create(
      pool, deadlines, std::move(*room),
      WorkBudget{.maxCommands = 128, .maxWallTime = 20ms},
      [&collector](RoomCommandOutcome outcome) {
        collector.add(std::move(outcome));
      },
      &readiness);
  if (!startGameplay(*cell, collector, 190)) {
    return false;
  }

  const auto startedAt =
      std::chrono::steady_clock::time_point{std::chrono::hours{1}};
  constexpr auto attackCount =
      CombatRuleset::monsterHitPointsForParticipants(2) /
      CombatRuleset::attackDamage;
  for (std::uint64_t index = 0; index < attackCount; ++index) {
    if (!isAccepted(cell->enqueue(
            attack(10 + index, index + 1, 190, startedAt + index * 800ms)))) {
      return false;
    }
  }
  if (!cell->waitUntilIdle(4s)) {
    return false;
  }
  const auto attacks = collector.take();
  if (attacks.size() != attackCount ||
      !attacks.back().attackResult.has_value() ||
      attacks.back().attackResult->outcome !=
          lol::battle::CombatOutcome::MonsterDefeated ||
      cell->settlementBatch().has_value()) {
    return false;
  }

  deadlines.advance(15s);
  if (!cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto terminal = collector.take();
  const auto lootDeadline = std::find_if(
      terminal.begin(), terminal.end(), [](const RoomCommandOutcome &outcome) {
        return outcome.kind == RoomCommandKind::LootDeadline;
      });
  if (lootDeadline == terminal.end() || !lootDeadline->detail.has_value() ||
      lootDeadline->detail->lifecycle !=
          RoomLifecycle::AwaitingSettlementDurability ||
      !hasTerminalHold(*cell, 190, SettlementBattleOutcome::MonsterDefeated,
                       SettlementExitStatus::TerminalPresent)) {
    return false;
  }
  return remainsHeldAfterLateCommand(*cell, collector, 100, 190);
}

bool durableCompletionValidatesCorrelationAndReopens() {
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 64}};
  AlwaysReadyReadiness readiness;
  SettlementCapacityGate gate;
  markStorageHealthy(gate);
  ManualSettlementStorage storage;
  OutcomeCollector collector;
  auto cell = makeDurableCell(pool, deadlines, readiness, gate, storage,
                              makeRoomWithMember(200, true), collector);
  if (!cell || !startGameplay(*cell, collector, 200)) {
    return false;
  }
  deadlines.advance(30s);
  if (!cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto terminal = collector.take();
  const auto batch = cell->settlementBatch();
  const auto &request = storage.request();
  const auto timeout = std::ranges::find_if(terminal, [](const auto &outcome) {
    return outcome.kind == RoomCommandKind::CombatDeadline;
  });
  if (timeout == terminal.end() || !batch.has_value() || !request.has_value() ||
      request->batchId != batch->id() || request->roomId != RoomId{200} ||
      request->battleId != BattleInstanceId{1} ||
      request->canonicalIntents.size() != batch->intents().size()) {
    return false;
  }
  for (std::size_t index = 0; index < batch->intents().size(); ++index) {
    if (request->canonicalIntents[index] !=
        lol::settlement::canonicalPayload(batch->intents()[index])) {
      return false;
    }
  }

  auto wrongBytes = batch->id().bytes();
  wrongBytes.back() ^= 1u;
  if (!storage.complete(DurableAppendCompleted{
          .batchId = lol::settlement::SettlementBatchId{wrongBytes},
          .roomId = RoomId{200},
          .battleId = BattleInstanceId{1},
          .commitSequence = 3u,
      }) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto wrong = collector.take();
  if (wrong.size() != 1u ||
      wrong.front().kind != RoomCommandKind::DurableAppendCompleted ||
      wrong.front().code != RoomResultCode::InvalidArgument ||
      wrong.front().settlementDurable ||
      cell->lifecycle() != RoomLifecycle::AwaitingSettlementDurability ||
      cell->settlementBatch() != batch ||
      gate.metrics().reservedBatches != 1u) {
    return false;
  }

  if (!storage.complete(DurableAppendCompleted{
          .batchId = batch->id(),
          .roomId = RoomId{200},
          .battleId = BattleInstanceId{1},
          .commitSequence = 3u,
      }) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto completed = collector.take();
  if (completed.size() != 1u || !completed.front().settlementDurable ||
      !completed.front().finalResult.has_value() ||
      completed.front().finalResult->roomId != RoomId{200} ||
      completed.front().finalResult->battleId != BattleInstanceId{1} ||
      !completed.front().detail.has_value() ||
      completed.front().detail->lifecycle != RoomLifecycle::Open ||
      std::ranges::any_of(completed.front().detail->members,
                          [](const auto &member) { return member.ready; }) ||
      !completed.front().battle.has_value() ||
      completed.front().battle->battleId != BattleInstanceId{1} ||
      cell->settlementBatch().has_value() ||
      cell->lifecycle() != RoomLifecycle::Open ||
      gate.metrics().reservedBatches != 0u) {
    return false;
  }

  if (!storage.complete(DurableAppendCompleted{
          .batchId = batch->id(),
          .roomId = RoomId{200},
          .battleId = BattleInstanceId{1},
          .commitSequence = 3u,
      }) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto stale = collector.take();
  return stale.size() == 1u &&
         stale.front().kind == RoomCommandKind::DurableAppendCompleted &&
         stale.front().code == RoomResultCode::InvalidArgument &&
         !stale.front().settlementDurable &&
         cell->lifecycle() == RoomLifecycle::Open;
}

bool lootResultsRemainReplayableFor30SecondsAfterDetach() {
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 64}};
  AlwaysReadyReadiness readiness;
  SettlementCapacityGate gate;
  markStorageHealthy(gate);
  ManualSettlementStorage storage;
  OutcomeCollector collector;
  auto cell = makeDurableCell(pool, deadlines, readiness, gate, storage,
                              makeRoomWithMember(205, true), collector);
  if (!cell || !startGameplay(*cell, collector, 205)) {
    return false;
  }

  const auto completedAt =
      std::chrono::steady_clock::time_point{std::chrono::hours{5}};
  const auto original = claimLoot(4, 1, 205, 99, completedAt - 1ms);
  if (!isAccepted(cell->enqueue(original)) || !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto retained = collector.take();
  if (retained.size() != 1u || !retained.front().lootClaimResult.has_value() ||
      retained.front().lootClaimResult->code !=
          ClaimLootResultCode::NotEligible) {
    return false;
  }

  if (!isAccepted(cell->enqueueControl(RoomControlEnvelope{
          .command = RoomControlCommand{lol::battle::CombatDeadlineCommand{
              BattleInstanceId{1}}},
          .occurredAt = completedAt,
      })) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  collector.take();
  const auto batch = cell->settlementBatch();
  if (!batch.has_value() ||
      !isAccepted(cell->enqueueControl(RoomControlEnvelope{
          .command = RoomControlCommand{DurableAppendCompleted{
              .batchId = batch->id(),
              .roomId = RoomId{205},
              .battleId = BattleInstanceId{1},
              .commitSequence = 3u,
          }},
          .occurredAt = completedAt + 1ms,
      })) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto completed = collector.take();
  if (completed.size() != 1u || !completed.front().settlementDurable ||
      cell->lifecycle() != RoomLifecycle::Open) {
    return false;
  }

  if (!isAccepted(
          cell->enqueue(claimLoot(5, 1, 205, 99, completedAt + 29999ms))) ||
      !isAccepted(
          cell->enqueue(claimLoot(6, 1, 205, 100, completedAt + 29999ms))) ||
      !isAccepted(
          cell->enqueue(claimLoot(7, 2, 205, 99, completedAt + 29999ms))) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto beforeExpiry = collector.take();
  if (beforeExpiry.size() != 3u ||
      !beforeExpiry[0].lootClaimResult.has_value() ||
      beforeExpiry[0].lootClaimResult->code !=
          ClaimLootResultCode::NotEligible ||
      !beforeExpiry[1].lootClaimResult.has_value() ||
      beforeExpiry[1].lootClaimResult->code !=
          ClaimLootResultCode::CommandConflict ||
      !beforeExpiry[2].lootClaimResult.has_value() ||
      beforeExpiry[2].lootClaimResult->code !=
          ClaimLootResultCode::StaleBattle) {
    return false;
  }

  if (!isAccepted(
          cell->enqueue(claimLoot(8, 1, 205, 99, completedAt + 30000ms))) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto expired = collector.take();
  return expired.size() == 1u && expired.front().lootClaimResult.has_value() &&
         expired.front().lootClaimResult->code ==
             ClaimLootResultCode::StaleBattle;
}

bool appendFailureNeverReopens() {
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 64}};
  AlwaysReadyReadiness readiness;
  SettlementCapacityGate gate;
  markStorageHealthy(gate);
  ManualSettlementStorage storage;
  OutcomeCollector collector;
  auto cell = makeDurableCell(pool, deadlines, readiness, gate, storage,
                              makeRoomWithMember(210, true), collector);
  if (!cell || !startGameplay(*cell, collector, 210)) {
    return false;
  }
  deadlines.advance(30s);
  if (!cell->waitUntilIdle(2s)) {
    return false;
  }
  collector.take();
  const auto batch = cell->settlementBatch();
  if (!batch.has_value() ||
      !storage.complete(DurableAppendFailed{
          .batchId = batch->id(),
          .roomId = RoomId{210},
          .battleId = BattleInstanceId{1},
          .failure = SettlementStorageFailure::IoFailure,
      }) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto failed = collector.take();
  if (failed.size() != 1u ||
      failed.front().kind != RoomCommandKind::DurableAppendFailed ||
      failed.front().code != RoomResultCode::InvalidArgument ||
      failed.front().settlementDurable ||
      failed.front().finalResult.has_value() ||
      !failed.front().recoveryNotice.has_value() ||
      failed.front().recoveryNotice->reason !=
          BattleRecoveryReason::SettlementRecoveryPending ||
      cell->lifecycle() != RoomLifecycle::AwaitingSettlementDurability ||
      cell->settlementBatch() != batch ||
      gate.metrics().reservedBatches != 1u) {
    return false;
  }
  if (!storage.complete(DurableAppendFailed{
          .batchId = batch->id(),
          .roomId = RoomId{210},
          .battleId = BattleInstanceId{1},
          .failure = SettlementStorageFailure::IoFailure,
      }) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto duplicate = collector.take();
  return duplicate.size() == 1u &&
         !duplicate.front().recoveryNotice.has_value() &&
         !duplicate.front().finalResult.has_value() &&
         cell->lifecycle() == RoomLifecycle::AwaitingSettlementDurability &&
         cell->settlementBatch() == batch;
}

bool retrySuccessReleasesRecoveryOnlyWithFinalResult() {
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 64}};
  AlwaysReadyReadiness readiness;
  SettlementCapacityGate gate;
  markStorageHealthy(gate);
  ManualSettlementStorage storage;
  OutcomeCollector collector;
  auto cell = makeDurableCell(pool, deadlines, readiness, gate, storage,
                              makeRoomWithMember(211, true), collector);
  if (!cell || !startGameplay(*cell, collector, 211)) {
    return false;
  }
  deadlines.advance(30s);
  if (!cell->waitUntilIdle(2s)) {
    return false;
  }
  collector.take();
  const auto batch = cell->settlementBatch();
  if (!batch.has_value() ||
      !storage.complete(DurableAppendFailed{
          .batchId = batch->id(),
          .roomId = RoomId{211},
          .battleId = BattleInstanceId{1},
          .failure = SettlementStorageFailure::IoFailure,
      }) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto pending = collector.take();
  if (pending.size() != 1u || !pending.front().recoveryNotice.has_value() ||
      pending.front().finalResult.has_value() ||
      cell->lifecycle() != RoomLifecycle::AwaitingSettlementDurability) {
    return false;
  }
  if (!storage.complete(DurableAppendCompleted{
          .batchId = batch->id(),
          .roomId = RoomId{211},
          .battleId = BattleInstanceId{1},
          .commitSequence = 9u,
      }) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto completed = collector.take();
  return completed.size() == 1u && completed.front().settlementDurable &&
         completed.front().finalResult.has_value() &&
         !completed.front().recoveryNotice.has_value() &&
         completed.front().detail.has_value() &&
         completed.front().detail->lifecycle == RoomLifecycle::Open &&
         cell->lifecycle() == RoomLifecycle::Open;
}

bool staleAppendFailureHasNoRecoveryEffect() {
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 64}};
  AlwaysReadyReadiness readiness;
  SettlementCapacityGate gate;
  markStorageHealthy(gate);
  ManualSettlementStorage storage;
  OutcomeCollector collector;
  auto cell = makeDurableCell(pool, deadlines, readiness, gate, storage,
                              makeRoomWithMember(212, true), collector);
  if (!cell || !startGameplay(*cell, collector, 212)) {
    return false;
  }
  deadlines.advance(30s);
  if (!cell->waitUntilIdle(2s)) {
    return false;
  }
  collector.take();
  const auto batch = cell->settlementBatch();
  if (!batch.has_value() ||
      !storage.complete(DurableAppendFailed{
          .batchId = batch->id(),
          .roomId = RoomId{212},
          .battleId = BattleInstanceId{99},
          .failure = SettlementStorageFailure::IoFailure,
      }) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto stale = collector.take();
  return stale.size() == 1u && !stale.front().recoveryNotice.has_value() &&
         !stale.front().finalResult.has_value() &&
         cell->lifecycle() == RoomLifecycle::AwaitingSettlementDurability &&
         cell->settlementBatch() == batch;
}

bool emptyRoomClosesOnlyAfterDurability() {
  ManualDeadlineScheduler deadlines;
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 64}};
  AlwaysReadyReadiness readiness;
  SettlementCapacityGate gate;
  markStorageHealthy(gate);
  ManualSettlementStorage storage;
  OutcomeCollector collector;
  auto cell = makeDurableCell(pool, deadlines, readiness, gate, storage,
                              makeRoomWithMember(220, true), collector);
  if (!cell || !startGameplay(*cell, collector, 220) ||
      !isAccepted(cell->enqueue(leave(4, 220))) ||
      !isAccepted(cell->enqueue(leave(5, 221))) || !cell->waitUntilIdle(2s)) {
    return false;
  }
  collector.take();
  const auto batch = cell->settlementBatch();
  if (!batch.has_value() ||
      cell->lifecycle() != RoomLifecycle::AwaitingSettlementDurability ||
      !storage.complete(DurableAppendCompleted{
          .batchId = batch->id(),
          .roomId = RoomId{220},
          .battleId = BattleInstanceId{1},
          .commitSequence = 3u,
      }) ||
      !cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto completed = collector.take();
  return completed.size() == 1u && completed.front().settlementDurable &&
         completed.front().finalResult.has_value() &&
         !completed.front().summary.has_value() &&
         !completed.front().detail.has_value() &&
         completed.front().battle.has_value() &&
         cell->lifecycle() == RoomLifecycle::Open &&
         !cell->settlementBatch().has_value() &&
         gate.metrics().reservedBatches == 0u;
}

} // namespace

int main() {
  const auto check = [](const char *name, bool passed) {
    if (!passed)
      std::fprintf(stderr, "%s failed\n", name);
    return passed;
  };
  if (!budgetReschedulesAndPreservesOrder() ||
      !oneCellHasOnlyOneActiveWorker() || !twoCellsRunInParallel() ||
      !workerPoolRejectsOverflowAndDrainsAcceptedWork() ||
      !mailboxSaturationPreservesControlReserve() ||
      !mailboxLanesMergeByAdmissionOrdinal() ||
      !roomCommandRacesFollowMailboxOrder() ||
      !continuityFailureFencesAlreadyAcceptedCommands() ||
      !authoritativeOutcomeWaitsForContinuityDurability() ||
      !roomMemberMutationChangesRecordedCompositeHash() ||
      !loadCancellationIsDurableBeforeBattleRelease() ||
      !continuityWriteFailureSuppressesSuccessAndFencesQueuedWork() ||
      !continuityQueueFullSuppressesSuccessAndFencesQueuedWork() ||
      !staleContinuityCommitDoesNotFailCloseOrReleaseHeldOutcome() ||
      !staleContinuityWriteFailureDoesNotFailCloseOrReleaseHeldOutcome() ||
      !check("recovered selective cell",
             recoveredCellContinuesSequenceAndRearmsLogicalDeadline()) ||
      !check("recovered activation",
             recoveredActivationSerializesDeadlineAndRetirement()) ||
      !suspendAndResumeBattleInputUsesTheCellMailbox() ||
      !hostStartCommitsOneBattleAndRoomLoading() ||
      !hostStartReservesCapacityAndLoadCancelReleasesIt() ||
      !hostStartRejectsUnhealthyStorageWithoutRoomMutation() ||
      !hostStartRejectsFullCriticalControlReservationWithoutMutation() ||
      !schedulerExhaustionRecoversAllHostStartReservations() ||
      !canceledLoadCallbackCannotMutateOrEmitAfterCombatRearm() ||
      !loadCombatLootRearmKeepsOneActiveDeadline() ||
      !criticalDeadlineRetriesAfterWorkerCapacityReturns() ||
      !rescheduleFailureDrainsAcceptedWork() ||
      !retiredCellCancelsPendingCombatDeadline() ||
      !combatTimeoutWaitsForSettlementDurability() ||
      !cancellationWaitsForSettlementDurability() ||
      !monsterDefeatedWaitsForSettlementDurability() ||
      !durableCompletionValidatesCorrelationAndReopens() ||
      !lootResultsRemainReplayableFor30SecondsAfterDetach() ||
      !appendFailureNeverReopens() ||
      !retrySuccessReleasesRecoveryOnlyWithFinalResult() ||
      !staleAppendFailureHasNoRecoveryEffect() ||
      !emptyRoomClosesOnlyAfterDurability()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
