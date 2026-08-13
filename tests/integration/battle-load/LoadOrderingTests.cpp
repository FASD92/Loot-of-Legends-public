#include "execution/RoomExecutionCell.hpp"

#include <lol/battle/BattleLoadApi.hpp>
#include <lol/game_flow/GameplayTransportReadinessPort.hpp>
#include <lol/lobby_room/RoomApi.hpp>
#include <lol/runtime/DeadlineScheduler.hpp>
#include <lol/runtime/WorkerPool.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using lol::battle::ArenaLoadCompleteCommand;
using lol::battle::BattleLoadResultCode;
using lol::battle::BattleLoadState;
using lol::battle::LoadCandidateState;
using lol::game_flow::GameplayTransportReadinessPort;
using lol::game_flow::execution::ConfirmedDisconnectCommand;
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
using lol::lobby_room::Room;
using lol::lobby_room::RoomLifecycle;
using lol::lobby_room::RoomMemberIdentity;
using lol::lobby_room::RoomResultCode;
using lol::lobby_room::SetReadyCommand;
using lol::runtime::DeadlineScheduler;
using lol::runtime::WorkerPool;
using lol::runtime::WorkerPoolConfig;
using lol::shared::AccountId;
using lol::shared::BattleInstanceId;
using lol::shared::RequestId;
using lol::shared::RoomId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;

AccountId account(std::uint64_t suffix) {
  AccountId::Bytes bytes{};
  bytes.back() = static_cast<std::uint8_t>(suffix);
  return AccountId{bytes};
}

RoomMemberIdentity member(std::uint64_t sessionId) {
  return RoomMemberIdentity{
      .accountId = account(sessionId),
      .sessionId = SessionId{sessionId},
      .generation = SessionGeneration{1},
      .nickname = "player-" + std::to_string(sessionId),
  };
}

std::optional<Room> makeReadyRoom(std::size_t count) {
  auto created = Room::create(CreateRoomCommand{
      .roomId = RoomId{7},
      .title = "room",
      .capacity = 10,
      .creator = member(1),
  });
  if (!created.room.has_value()) {
    return std::nullopt;
  }
  for (std::size_t index = 2; index <= count; ++index) {
    if (created.room->join(JoinRoomCommand{member(index)}) !=
        RoomResultCode::Ok) {
      return std::nullopt;
    }
  }
  for (std::size_t index = 1; index <= count; ++index) {
    if (created.room->setReady(SetReadyCommand{
            .sessionId = SessionId{index},
            .generation = SessionGeneration{1},
            .ready = true,
        }) != RoomResultCode::Ok) {
      return std::nullopt;
    }
  }
  return std::move(created.room);
}

class ManualDeadlineScheduler final : public DeadlineScheduler {
public:
  bool scheduleAfter(std::chrono::milliseconds delay, Task task) override {
    if (delay.count() < 0 || !task) {
      return false;
    }
    pending_.push_back(Pending{.due = now_ + delay, .task = std::move(task)});
    std::stable_sort(pending_.begin(), pending_.end(),
                     [](const Pending &left, const Pending &right) {
                       return left.due < right.due;
                     });
    return true;
  }

  [[nodiscard]] std::unique_ptr<lol::runtime::DeadlineLease>
  tryReserve() override {
    const auto id = nextLeaseId_++;
    leases_.push_back(id);
    return std::make_unique<Lease>(*this, id);
  }

  void advance(std::chrono::milliseconds elapsed) {
    now_ += elapsed;
    while (!pending_.empty() && pending_.front().due <= now_) {
      auto task = std::move(pending_.front().task);
      pending_.erase(pending_.begin());
      task();
    }
  }

  [[nodiscard]] std::optional<std::chrono::milliseconds> nextDelay() const {
    if (pending_.empty()) {
      return std::nullopt;
    }
    return pending_.front().due - now_;
  }

private:
  class Lease final : public lol::runtime::DeadlineLease {
  public:
    Lease(ManualDeadlineScheduler &owner, std::uint64_t id)
        : owner_(&owner), id_(id) {}
    ~Lease() override { owner_->release(id_); }
    [[nodiscard]] bool armAfter(std::chrono::milliseconds delay,
                                Task task) override {
      return owner_->arm(id_, delay, std::move(task));
    }
    void cancel() noexcept override { owner_->cancel(id_); }

  private:
    ManualDeadlineScheduler *owner_;
    std::uint64_t id_;
  };

  struct Pending final {
    std::chrono::milliseconds due;
    std::uint64_t leaseId{0};
    Task task;
  };

  bool arm(std::uint64_t id, std::chrono::milliseconds delay, Task task) {
    if (std::ranges::find(leases_, id) == leases_.end()) {
      return false;
    }
    cancel(id);
    if (delay.count() < 0 || !task) {
      return false;
    }
    pending_.push_back(
        Pending{.due = now_ + delay, .leaseId = id, .task = std::move(task)});
    std::stable_sort(pending_.begin(), pending_.end(),
                     [](const Pending &left, const Pending &right) {
                       return left.due < right.due;
                     });
    return true;
  }

  void cancel(std::uint64_t id) noexcept {
    std::erase_if(pending_, [id](const Pending &pending) {
      return pending.leaseId == id;
    });
  }

  void release(std::uint64_t id) noexcept {
    cancel(id);
    std::erase(leases_, id);
  }

  std::chrono::milliseconds now_{0};
  std::uint64_t nextLeaseId_{1};
  std::vector<Pending> pending_;
  std::vector<std::uint64_t> leases_;
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

class FakeGameplayTransportReadiness final
    : public GameplayTransportReadinessPort {
public:
  explicit FakeGameplayTransportReadiness(
      std::vector<std::uint64_t> notReadySessions)
      : notReadySessions_(std::move(notReadySessions)) {}

  [[nodiscard]] bool
  isReady(SessionId sessionId,
          SessionGeneration generation) const noexcept override {
    return generation == SessionGeneration{1} &&
           std::ranges::find(notReadySessions_, sessionId.value()) ==
               notReadySessions_.end();
  }

private:
  std::vector<std::uint64_t> notReadySessions_;
};

class Scenario final {
public:
  explicit Scenario(std::size_t candidateCount,
                    std::vector<std::uint64_t> notReadySessions = {},
                    bool attachReadiness = true)
      : workers_(WorkerPoolConfig{.threadCount = 1, .queueCapacity = 32}),
        readiness_(std::move(notReadySessions)) {
    auto room = makeReadyRoom(candidateCount);
    if (!room.has_value()) {
      std::abort();
    }
    cell_ = RoomExecutionCell::create(
        workers_, deadlines_, std::move(*room),
        WorkBudget{.maxCommands = 64, .maxWallTime = 2ms},
        [this](RoomCommandOutcome outcome) {
          outcomes_.add(std::move(outcome));
        },
        attachReadiness ? &readiness_ : nullptr);
  }

  bool start() {
    if (cell_->enqueue(RoomCommandEnvelope{
            .requestId = RequestId{1},
            .command = RoomCellCommand{HostStartEligibilityCommand{
                .actorSessionId = SessionId{1},
                .actorGeneration = SessionGeneration{1},
            }},
        }) != RoomCommandAdmission::Accepted ||
        !cell_->waitUntilIdle(2s)) {
      return false;
    }
    const auto started = outcomes_.take();
    return started.size() == 1 && started.front().battle.has_value() &&
           started.front().battle->state == BattleLoadState::LoadBarrierOpen &&
           deadlines_.nextDelay() == 10s;
  }

  bool complete(std::uint64_t sessionId, std::uint64_t battleId = 1) {
    return cell_->enqueue(RoomCommandEnvelope{
               .requestId = RequestId{10 + sessionId},
               .command = RoomCellCommand{ArenaLoadCompleteCommand{
                   .sessionId = SessionId{sessionId},
                   .generation = SessionGeneration{1},
                   .roomId = RoomId{7},
                   .battleId = BattleInstanceId{battleId},
               }},
           }) == RoomCommandAdmission::Accepted;
  }

  bool disconnect(std::uint64_t sessionId) {
    return cell_->enqueueControl(RoomControlEnvelope{
               .command = RoomControlCommand{ConfirmedDisconnectCommand{
                   .sessionId = SessionId{sessionId},
                   .generation = SessionGeneration{1},
               }},
           }) == RoomCommandAdmission::Accepted;
  }

  std::optional<std::vector<RoomCommandOutcome>> waitAndTake() {
    if (!cell_->waitUntilIdle(2s)) {
      return std::nullopt;
    }
    return outcomes_.take();
  }

  void advance(std::chrono::milliseconds elapsed) {
    deadlines_.advance(elapsed);
  }

private:
  ManualDeadlineScheduler deadlines_;
  WorkerPool workers_;
  OutcomeCollector outcomes_;
  FakeGameplayTransportReadiness readiness_;
  std::shared_ptr<RoomExecutionCell> cell_;
};

LoadCandidateState candidateState(const RoomCommandOutcome &outcome,
                                  std::uint64_t sessionId) {
  if (!outcome.battle.has_value()) {
    std::abort();
  }
  for (const auto &candidate : outcome.battle->candidates) {
    if (candidate.sessionId == SessionId{sessionId}) {
      return candidate.state;
    }
  }
  std::abort();
}

bool completeAndDisconnectUseMailboxOrder() {
  Scenario completeFirst{2};
  if (!completeFirst.start() || !completeFirst.complete(2) ||
      !completeFirst.disconnect(2)) {
    return false;
  }
  const auto firstOrder = completeFirst.waitAndTake();
  if (!firstOrder.has_value() || firstOrder->size() != 2 ||
      firstOrder->back().battleCode != BattleLoadResultCode::Ok ||
      candidateState(firstOrder->back(), 2) != LoadCandidateState::Ready ||
      !firstOrder->back().detail.has_value() ||
      firstOrder->back().detail->members.size() != 1) {
    return false;
  }

  Scenario disconnectFirst{2};
  if (!disconnectFirst.start() || !disconnectFirst.disconnect(2) ||
      !disconnectFirst.complete(2)) {
    return false;
  }
  const auto secondOrder = disconnectFirst.waitAndTake();
  return secondOrder.has_value() && secondOrder->size() == 2 &&
         secondOrder->back().battleCode == BattleLoadResultCode::NotEligible &&
         candidateState(secondOrder->back(), 2) ==
             LoadCandidateState::Disconnected;
}

bool deadlineUsesBoundCommandAndCommitsReadySubset() {
  Scenario scenario{3};
  if (!scenario.start() || !scenario.complete(1) || !scenario.complete(2) ||
      !scenario.waitAndTake().has_value()) {
    return false;
  }
  scenario.advance(9'999ms);
  const auto beforeDeadline = scenario.waitAndTake();
  if (!beforeDeadline.has_value() || !beforeDeadline->empty()) {
    return false;
  }
  scenario.advance(1ms);
  const auto outcomes = scenario.waitAndTake();
  if (!outcomes.has_value() || outcomes->size() != 1) {
    return false;
  }
  const auto &deadline = outcomes->front();
  return deadline.kind == RoomCommandKind::LoadBarrierDeadline &&
         deadline.battleCode == BattleLoadResultCode::Ok &&
         deadline.battle.has_value() &&
         deadline.battle->state == BattleLoadState::GameplayCommitted &&
         deadline.battle->capturedParticipants.size() == 2 &&
         candidateState(deadline, 3) == LoadCandidateState::TimedOut &&
         deadline.detail.has_value() &&
         deadline.detail->lifecycle == RoomLifecycle::InProgress;
}

bool readinessGateExcludesNonReadyCandidate() {
  Scenario scenario{3, {3}};
  if (!scenario.start() || !scenario.complete(1) || !scenario.complete(2) ||
      !scenario.complete(3)) {
    return false;
  }
  const auto completions = scenario.waitAndTake();
  if (!completions.has_value() || completions->size() != 3) {
    return false;
  }
  const auto &beforeDeadline = completions->back();
  if (!beforeDeadline.battle.has_value() ||
      beforeDeadline.battle->state != BattleLoadState::LoadBarrierOpen ||
      candidateState(beforeDeadline, 1) != LoadCandidateState::Ready ||
      candidateState(beforeDeadline, 2) != LoadCandidateState::Ready ||
      candidateState(beforeDeadline, 3) != LoadCandidateState::PendingLoad ||
      !beforeDeadline.battle->capturedParticipants.empty() ||
      !beforeDeadline.detail.has_value() ||
      beforeDeadline.detail->lifecycle != RoomLifecycle::Loading) {
    return false;
  }

  scenario.advance(10s);
  const auto resolved = scenario.waitAndTake();
  return resolved.has_value() && resolved->size() == 1 &&
         resolved->front().battle.has_value() &&
         resolved->front().battle->state ==
             BattleLoadState::GameplayCommitted &&
         resolved->front().battle->capturedParticipants.size() == 2 &&
         candidateState(resolved->front(), 3) == LoadCandidateState::TimedOut &&
         resolved->front().detail.has_value() &&
         resolved->front().detail->lifecycle == RoomLifecycle::InProgress;
}

bool releaseCompositionWithoutAdapterCannotCommitGameplay() {
  Scenario scenario{2, {}, false};
  if (!scenario.start() || !scenario.complete(1) || !scenario.complete(2)) {
    return false;
  }
  const auto completions = scenario.waitAndTake();
  if (!completions.has_value() || completions->size() != 2 ||
      !completions->back().battle.has_value() ||
      completions->back().battle->state != BattleLoadState::LoadBarrierOpen ||
      candidateState(completions->back(), 1) !=
          LoadCandidateState::PendingLoad ||
      candidateState(completions->back(), 2) !=
          LoadCandidateState::PendingLoad) {
    return false;
  }

  scenario.advance(10s);
  const auto cancelled = scenario.waitAndTake();
  return cancelled.has_value() && cancelled->size() == 1 &&
         cancelled->front().battle.has_value() &&
         cancelled->front().battle->state == BattleLoadState::LoadCancelled &&
         cancelled->front().battle->capturedParticipants.empty() &&
         cancelled->front().detail.has_value() &&
         cancelled->front().detail->lifecycle == RoomLifecycle::Open;
}

bool staleCompletionAndCanceledTimerDoNotMutateReopenedRoom() {
  Scenario scenario{2};
  if (!scenario.start() || !scenario.disconnect(2) || !scenario.complete(1)) {
    return false;
  }
  const auto cancelled = scenario.waitAndTake();
  if (!cancelled.has_value() || cancelled->size() != 2 ||
      !cancelled->back().battle.has_value() ||
      cancelled->back().battle->state != BattleLoadState::LoadCancelled ||
      !cancelled->back().detail.has_value() ||
      cancelled->back().detail->lifecycle != RoomLifecycle::Open ||
      cancelled->back().detail->members.front().ready ||
      !scenario.complete(1)) {
    return false;
  }
  const auto staleComplete = scenario.waitAndTake();
  if (!staleComplete.has_value() || staleComplete->size() != 1 ||
      staleComplete->front().battleCode != BattleLoadResultCode::StaleBattle ||
      !staleComplete->front().detail.has_value() ||
      staleComplete->front().detail->lifecycle != RoomLifecycle::Open ||
      staleComplete->front().detail->members.front().ready) {
    return false;
  }

  scenario.advance(10s);
  const auto afterCanceledTimer = scenario.waitAndTake();
  return afterCanceledTimer.has_value() && afterCanceledTimer->empty();
}

} // namespace

int main() {
  if (!completeAndDisconnectUseMailboxOrder() ||
      !deadlineUsesBoundCommandAndCommitsReadySubset() ||
      !readinessGateExcludesNonReadyCandidate() ||
      !releaseCompositionWithoutAdapterCannotCommitGameplay() ||
      !staleCompletionAndCanceledTimerDoNotMutateReopenedRoom()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
