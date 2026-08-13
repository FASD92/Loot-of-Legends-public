#include "execution/RoomExecutionDirectory.hpp"
#include "execution/SessionRouteIndex.hpp"

#include <lol/lobby_room/RoomApi.hpp>
#include <lol/runtime/DeadlineScheduler.hpp>
#include <lol/runtime/WorkerPool.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using lol::game_flow::execution::CellHandle;
using lol::game_flow::execution::ConfirmedDisconnectCommand;
using lol::game_flow::execution::RoomCellCommand;
using lol::game_flow::execution::RoomCommandAdmission;
using lol::game_flow::execution::RoomCommandEnvelope;
using lol::game_flow::execution::RoomCommandKind;
using lol::game_flow::execution::RoomCommandOutcome;
using lol::game_flow::execution::RoomControlCommand;
using lol::game_flow::execution::RoomControlEnvelope;
using lol::game_flow::execution::RoomDirectoryEntry;
using lol::game_flow::execution::RoomExecutionDirectory;
using lol::game_flow::execution::RouteBindResult;
using lol::game_flow::execution::SessionRoomRoute;
using lol::game_flow::execution::SessionRouteIndex;
using lol::game_flow::execution::WorkBudget;
using lol::lobby_room::CreateRoomCommand;
using lol::lobby_room::JoinRoomCommand;
using lol::lobby_room::LeaveRoomCommand;
using lol::lobby_room::Room;
using lol::lobby_room::RoomMemberIdentity;
using lol::lobby_room::RoomResultCode;
using lol::lobby_room::RoomSummary;
using lol::lobby_room::SetReadyCommand;
using lol::runtime::ThreadDeadlineScheduler;
using lol::runtime::ThreadDeadlineSchedulerConfig;
using lol::runtime::WorkerPool;
using lol::runtime::WorkerPoolConfig;
using lol::shared::AccountId;
using lol::shared::RequestId;
using lol::shared::RoomId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;

AccountId account(std::uint64_t suffix) {
  AccountId::Bytes bytes{};
  bytes.back() = static_cast<std::uint8_t>(suffix);
  return AccountId{bytes};
}

std::optional<Room> makeRoom(std::uint64_t roomId, std::uint64_t hostId) {
  auto created = Room::create(CreateRoomCommand{
      .roomId = RoomId{roomId},
      .title = "room-" + std::to_string(roomId),
      .capacity = 10,
      .creator =
          RoomMemberIdentity{
              .accountId = account(hostId),
              .sessionId = SessionId{hostId},
              .generation = SessionGeneration{1},
              .nickname = "host",
          },
  });
  return std::move(created.room);
}

std::optional<Room> makeRoomWithMember(std::uint64_t roomId,
                                       std::uint64_t hostId,
                                       std::uint64_t memberId) {
  auto room = makeRoom(roomId, hostId);
  if (!room.has_value() || room->join(JoinRoomCommand{
                               RoomMemberIdentity{
                                   .accountId = account(memberId),
                                   .sessionId = SessionId{memberId},
                                   .generation = SessionGeneration{1},
                                   .nickname = "member",
                               },
                           }) != RoomResultCode::Ok) {
    return std::nullopt;
  }
  return room;
}

RoomCommandEnvelope ready(std::uint64_t requestId, std::uint64_t sessionId) {
  return RoomCommandEnvelope{
      .requestId = RequestId{requestId},
      .command = RoomCellCommand{SetReadyCommand{
          .sessionId = SessionId{sessionId},
          .generation = SessionGeneration{1},
          .ready = true,
      }},
      .receivedAt = std::chrono::steady_clock::now(),
  };
}

RoomCommandEnvelope leave(std::uint64_t requestId, std::uint64_t sessionId) {
  return RoomCommandEnvelope{
      .requestId = RequestId{requestId},
      .command = RoomCellCommand{LeaveRoomCommand{
          .sessionId = SessionId{sessionId},
          .generation = SessionGeneration{1},
      }},
      .receivedAt = std::chrono::steady_clock::now(),
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

bool isAccepted(RoomCommandAdmission admission) {
  return admission == RoomCommandAdmission::Accepted;
}

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

class OutcomeWaiter final {
public:
  void add(RoomCommandOutcome outcome) {
    std::lock_guard lock{mutex_};
    outcomes_.push_back(std::move(outcome));
    changed_.notify_all();
  }

  bool waitFor(std::size_t count) {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(
        lock, 2s, [this, count] { return outcomes_.size() >= count; });
  }

  std::vector<RoomCommandOutcome> take() {
    std::lock_guard lock{mutex_};
    return std::exchange(outcomes_, {});
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<RoomCommandOutcome> outcomes_;
};

bool directoryOwnsOnlyHandleAndSummary() {
  ThreadDeadlineScheduler deadlines{
      ThreadDeadlineSchedulerConfig{.queueCapacity = 16}};
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 8}};
  RoomExecutionDirectory directory;
  auto room = makeRoom(101, 10);
  if (!room.has_value()) {
    return false;
  }
  const auto expectedSummary = room->summary();
  auto created =
      directory.create(pool, deadlines, std::move(*room),
                       WorkBudget{.maxCommands = 64, .maxWallTime = 2ms},
                       [](RoomCommandOutcome) {});
  if (!created.has_value() || !expectedSummary.has_value()) {
    return false;
  }

  const auto &[cell, summary] = *created;
  static_assert(
      std::is_same_v<std::remove_cvref_t<decltype(cell)>, CellHandle>);
  static_assert(
      std::is_same_v<std::remove_cvref_t<decltype(summary)>, RoomSummary>);
  if (!cell || summary != *expectedSummary || directory.size() != 1) {
    return false;
  }

  auto duplicateRoom = makeRoom(101, 11);
  if (!duplicateRoom.has_value() ||
      directory
          .create(pool, deadlines, std::move(*duplicateRoom),
                  WorkBudget{.maxCommands = 64, .maxWallTime = 2ms},
                  [](RoomCommandOutcome) {})
          .has_value()) {
    return false;
  }

  auto snapshot = directory.lookup(RoomId{101});
  if (!snapshot.has_value() || snapshot->cell != cell ||
      snapshot->summary != summary || directory.summaries().size() != 1) {
    return false;
  }
  snapshot->summary.title = "caller-local-change";
  const auto unchanged = directory.lookup(RoomId{101});
  if (!unchanged.has_value() || unchanged->summary != summary) {
    return false;
  }

  auto updatedSummary = summary;
  updatedSummary.memberCount = 2;
  if (!directory.updateSummary(updatedSummary) ||
      directory.updateSummary(RoomSummary{
          .roomId = RoomId{999},
          .title = "missing",
          .memberCount = 1,
          .capacity = 2,
      })) {
    return false;
  }
  const auto updated = directory.lookup(RoomId{101});
  if (!updated.has_value() || updated->summary != updatedSummary) {
    return false;
  }

  return directory.remove(RoomId{101}) && !directory.remove(RoomId{101}) &&
         !directory.lookup(RoomId{101}).has_value() && directory.size() == 0;
}

bool routeCannotAuthorizeMembershipAndRejectsStaleGeneration() {
  ThreadDeadlineScheduler deadlines{
      ThreadDeadlineSchedulerConfig{.queueCapacity = 16}};
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 8}};
  RoomExecutionDirectory directory;
  OutcomeWaiter outcomes;
  auto room = makeRoom(202, 20);
  if (!room.has_value()) {
    return false;
  }
  auto created =
      directory.create(pool, deadlines, std::move(*room),
                       WorkBudget{.maxCommands = 64, .maxWallTime = 2ms},
                       [&outcomes](RoomCommandOutcome outcome) {
                         outcomes.add(std::move(outcome));
                       });
  if (!created.has_value()) {
    return false;
  }

  SessionRouteIndex routes;
  const SessionRoomRoute outsiderRoute{
      .sessionId = SessionId{999},
      .generation = SessionGeneration{7},
      .roomId = RoomId{202},
  };
  if (routes.bind(outsiderRoute) != RouteBindResult::Inserted ||
      routes.bind(outsiderRoute) != RouteBindResult::AlreadyBound ||
      routes.lookup(SessionId{999}, SessionGeneration{7}) != RoomId{202} ||
      routes.lookup(SessionId{999}, SessionGeneration{6}).has_value() ||
      routes.clear(SessionId{999}, SessionGeneration{6}, RoomId{202}) ||
      routes.bind(SessionRoomRoute{
          .sessionId = SessionId{999},
          .generation = SessionGeneration{7},
          .roomId = RoomId{303},
      }) != RouteBindResult::Conflict) {
    return false;
  }

  const auto admission = created->cell->enqueue(RoomCommandEnvelope{
      .requestId = RequestId{1},
      .command = RoomCellCommand{SetReadyCommand{
          .sessionId = SessionId{999},
          .generation = SessionGeneration{7},
          .ready = true,
      }},
  });
  if (admission != RoomCommandAdmission::Accepted || !outcomes.waitFor(1) ||
      !created->cell->waitUntilIdle(2s)) {
    return false;
  }
  const auto result = outcomes.take();
  if (result.size() != 1 || result.front().code != RoomResultCode::NotInRoom ||
      routes.lookup(SessionId{999}, SessionGeneration{7}) != RoomId{202}) {
    return false;
  }

  return routes.clear(SessionId{999}, SessionGeneration{7}, RoomId{202}) &&
         !routes.lookup(SessionId{999}, SessionGeneration{7}).has_value() &&
         routes.size() == 0;
}

// A strong CellHandle that outlives directory removal must not authorize
// gameplay: every later admission is RoomRetired with zero mutation and zero
// outcome, regardless of physical shared_ptr survival.
bool retiredDirectoryEntryRejectsAdmissionsWithoutMutation() {
  ThreadDeadlineScheduler deadlines{
      ThreadDeadlineSchedulerConfig{.queueCapacity = 16}};
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 8}};
  RoomExecutionDirectory directory;
  OutcomeCollector collector;
  auto room = makeRoomWithMember(301, 30, 31);
  if (!room.has_value()) {
    return false;
  }
  auto created =
      directory.create(pool, deadlines, std::move(*room),
                       WorkBudget{.maxCommands = 64, .maxWallTime = 2ms},
                       [&collector](RoomCommandOutcome outcome) {
                         collector.add(std::move(outcome));
                       });
  if (!created.has_value()) {
    return false;
  }
  const auto handle = created->cell; // strong handle survives removal
  if (!handle || !directory.remove(RoomId{301}) || directory.size() != 0) {
    return false;
  }
  if (handle->enqueue(ready(1, 30)) != RoomCommandAdmission::RoomRetired ||
      handle->enqueue(leave(2, 31)) != RoomCommandAdmission::RoomRetired ||
      handle->enqueueControl(disconnect(30)) !=
          RoomCommandAdmission::RoomRetired ||
      directory.remove(RoomId{301}) ||
      directory.lookup(RoomId{301}).has_value()) {
    return false;
  }
  if (!handle->waitUntilIdle(2s)) {
    return false;
  }
  const auto detail = handle->detail();
  const auto metrics = handle->metrics();
  return collector.take().empty() && detail.has_value() &&
         detail->members.size() == 2 && !detail->members.front().ready &&
         metrics.processedCommands == 0 &&
         metrics.processedControlCommands == 0;
}

// Block the worker, admit a one-member room's actual last Leave (host) followed
// by later external/control commands for that host, and make the Leave outcome
// sink call the real directory removal. Removal must retire the cell and cancel
// the later queued ordinals; only the Leave outcome is observed, and the empty
// room projects no detail.
bool leaveOutcomeRemovalCancelsQueuedOrdinals() {
  ThreadDeadlineScheduler deadlines{
      ThreadDeadlineSchedulerConfig{.queueCapacity = 16}};
  WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 16}};
  Gate blocker;
  if (!pool.submit([&blocker] { blocker.enterAndWait(); }) ||
      !blocker.waitFor(1)) {
    blocker.open();
    return false;
  }
  RoomExecutionDirectory directory;
  OutcomeCollector collector;
  auto room = makeRoom(302, 30);
  if (!room.has_value()) {
    blocker.open();
    return false;
  }
  auto created =
      directory.create(pool, deadlines, std::move(*room),
                       WorkBudget{.maxCommands = 64, .maxWallTime = 2ms},
                       [&collector, &directory](RoomCommandOutcome outcome) {
                         if (outcome.kind == RoomCommandKind::Leave &&
                             outcome.code == RoomResultCode::Ok) {
                           static_cast<void>(directory.remove(RoomId{302}));
                         }
                         collector.add(std::move(outcome));
                       });
  if (!created.has_value()) {
    blocker.open();
    return false;
  }
  const auto handle = created->cell;
  if (!isAccepted(handle->enqueue(leave(1, 30))) ||
      !isAccepted(handle->enqueue(ready(2, 30))) ||
      !isAccepted(handle->enqueueControl(disconnect(30)))) {
    blocker.open();
    return false;
  }
  blocker.open();
  if (!handle->waitUntilIdle(2s) || !pool.waitUntilIdle(2s)) {
    return false;
  }
  const auto outcomes = collector.take();
  const auto metrics = handle->metrics();
  const auto detail = handle->detail();
  return outcomes.size() == 1 &&
         outcomes.front().kind == RoomCommandKind::Leave &&
         outcomes.front().code == RoomResultCode::Ok &&
         outcomes.front().queueDelay > 0ns &&
         outcomes.front().processingDuration >= 0ns &&
         !outcomes.front().criticalTerminalLatency.has_value() &&
         metrics.processedCommands == 1 &&
         metrics.processedExternalCommands == 1 &&
         metrics.processedControlCommands == 0 &&
         metrics.lastProcessedOrdinal == 1 && directory.size() == 0 &&
         !detail.has_value();
}

// Deterministic race at the Cell-mutex retirement boundary. An admission
// linearized before retirement is Accepted and then cancelled by retire with
// zero outcome/mutation; an admission after retirement is rejected as
// RoomRetired. Both branches never mutate or produce post-retirement output.
bool admissionAtRetirementBoundaryNeverMutates() {
  // Branch A: admitted while Active, then cancelled by retirement.
  {
    ThreadDeadlineScheduler deadlines{
        ThreadDeadlineSchedulerConfig{.queueCapacity = 16}};
    WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 16}};
    Gate blocker;
    if (!pool.submit([&blocker] { blocker.enterAndWait(); }) ||
        !blocker.waitFor(1)) {
      blocker.open();
      return false;
    }
    RoomExecutionDirectory directory;
    Gate sinkGate;
    OutcomeCollector collector;
    auto room = makeRoom(303, 30);
    if (!room.has_value()) {
      blocker.open();
      return false;
    }
    auto created =
        directory.create(pool, deadlines, std::move(*room),
                         WorkBudget{.maxCommands = 64, .maxWallTime = 2ms},
                         [&sinkGate, &collector](RoomCommandOutcome outcome) {
                           if (outcome.kind == RoomCommandKind::Leave) {
                             sinkGate.enterAndWait();
                           }
                           collector.add(std::move(outcome));
                         });
    if (!created.has_value()) {
      blocker.open();
      return false;
    }
    const auto handle = created->cell;
    if (!isAccepted(handle->enqueue(leave(1, 30))) ||
        !isAccepted(handle->enqueue(ready(2, 30)))) {
      blocker.open();
      return false;
    }
    blocker.open();
    if (!sinkGate.waitFor(1)) {
      blocker.open();
      return false;
    }
    // The Leave outcome sink holds the worker, so the Cell mutex is free while
    // the cell is still Active: this admission deterministically linearizes
    // BEFORE retirement.
    if (handle->enqueue(ready(3, 30)) != RoomCommandAdmission::Accepted) {
      sinkGate.open();
      return false;
    }
    // Retirement cancels the just-admitted command and the earlier queued one.
    if (!directory.remove(RoomId{303})) {
      sinkGate.open();
      return false;
    }
    sinkGate.open();
    if (!handle->waitUntilIdle(2s) || !pool.waitUntilIdle(2s)) {
      return false;
    }
    const auto outcomes = collector.take();
    const auto metrics = handle->metrics();
    const auto detail = handle->detail();
    const bool okA = outcomes.size() == 1 &&
                     outcomes.front().kind == RoomCommandKind::Leave &&
                     outcomes.front().code == RoomResultCode::Ok &&
                     metrics.processedCommands == 1 &&
                     metrics.lastProcessedOrdinal == 1 && !detail.has_value();
    if (!okA) {
      return false;
    }
  }
  // Branch B: admission after retirement is rejected as RoomRetired.
  {
    ThreadDeadlineScheduler deadlines{
        ThreadDeadlineSchedulerConfig{.queueCapacity = 16}};
    WorkerPool pool{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 8}};
    RoomExecutionDirectory directory;
    OutcomeCollector collector;
    auto room = makeRoomWithMember(304, 30, 31);
    if (!room.has_value()) {
      return false;
    }
    auto created =
        directory.create(pool, deadlines, std::move(*room),
                         WorkBudget{.maxCommands = 64, .maxWallTime = 2ms},
                         [&collector](RoomCommandOutcome outcome) {
                           collector.add(std::move(outcome));
                         });
    if (!created.has_value() || !directory.remove(RoomId{304})) {
      return false;
    }
    const auto handle = created->cell;
    const bool okB =
        handle->enqueue(ready(1, 30)) == RoomCommandAdmission::RoomRetired &&
        handle->enqueueControl(disconnect(31)) ==
            RoomCommandAdmission::RoomRetired;
    if (!okB) {
      return false;
    }
    const bool okB2 = handle->waitUntilIdle(2s) && collector.take().empty() &&
                      handle->metrics().processedCommands == 0;
    return okB2;
  }
}

} // namespace

int main() {
  if (!directoryOwnsOnlyHandleAndSummary() ||
      !routeCannotAuthorizeMembershipAndRejectsStaleGeneration() ||
      !retiredDirectoryEntryRejectsAdmissionsWithoutMutation() ||
      !leaveOutcomeRemovalCancelsQueuedOrdinals() ||
      !admissionAtRetirementBoundaryNeverMutates()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
