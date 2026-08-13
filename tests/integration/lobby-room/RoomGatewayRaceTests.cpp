#include <lol/game_flow/RoomCommandGateway.hpp>
#include <lol/runtime/WorkerPool.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using lol::game_flow::AuthenticatedRoomSession;
using lol::game_flow::CreateRoomRequest;
using lol::game_flow::JoinRoomRequest;
using lol::game_flow::LeaveRoomRequest;
using lol::game_flow::LobbyRoomOutboundIntent;
using lol::game_flow::RoomCommandEnvelope;
using lol::game_flow::RoomCommandGateway;
using lol::game_flow::RoomCommandResponse;
using lol::game_flow::RoomSubmitResult;
using lol::game_flow::SetReadyRequest;
using lol::lobby_room::RoomDetailProjection;
using lol::lobby_room::RoomResultCode;
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

AuthenticatedRoomSession session(std::uint64_t id,
                                 std::uint64_t generation = 1) {
  return AuthenticatedRoomSession{
      .accountId = account(id),
      .sessionId = SessionId{id},
      .generation = SessionGeneration{generation},
      .nickname = "player-" + std::to_string(id),
  };
}

class IntentCollector final {
public:
  void add(LobbyRoomOutboundIntent intent) {
    std::lock_guard lock{mutex_};
    intents_.push_back(std::move(intent));
  }

  std::vector<LobbyRoomOutboundIntent> take() {
    std::lock_guard lock{mutex_};
    return std::exchange(intents_, {});
  }

private:
  std::mutex mutex_;
  std::vector<LobbyRoomOutboundIntent> intents_;
};

class Gate final {
public:
  void enterAndWait() {
    std::unique_lock lock{mutex_};
    ++entered_;
    changed_.notify_all();
    changed_.wait(lock, [this] { return open_; });
  }

  bool waitFor(std::size_t count) {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, 2s,
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

RoomSubmitResult create(RoomCommandGateway &gateway,
                        const AuthenticatedRoomSession &actor,
                        std::uint64_t requestId, std::uint8_t capacity = 2) {
  return gateway.submit(RoomCommandEnvelope{
      .session = actor,
      .command =
          CreateRoomRequest{
              .requestId = RequestId{requestId},
              .title = "room-" + std::to_string(requestId),
              .capacity = capacity,
          },
  });
}

RoomSubmitResult join(RoomCommandGateway &gateway,
                      const AuthenticatedRoomSession &actor,
                      std::uint64_t requestId, std::uint64_t roomId) {
  return gateway.submit(RoomCommandEnvelope{
      .session = actor,
      .command =
          JoinRoomRequest{
              .requestId = RequestId{requestId},
              .roomId = RoomId{roomId},
          },
  });
}

RoomSubmitResult leave(RoomCommandGateway &gateway,
                       const AuthenticatedRoomSession &actor,
                       std::uint64_t requestId) {
  return gateway.submit(RoomCommandEnvelope{
      .session = actor,
      .command = LeaveRoomRequest{.requestId = RequestId{requestId}},
  });
}

RoomSubmitResult ready(RoomCommandGateway &gateway,
                       const AuthenticatedRoomSession &actor,
                       std::uint64_t requestId, bool value) {
  return gateway.submit(RoomCommandEnvelope{
      .session = actor,
      .command =
          SetReadyRequest{
              .requestId = RequestId{requestId},
              .ready = value,
          },
  });
}

std::optional<RoomResultCode>
response(const std::vector<LobbyRoomOutboundIntent> &intents,
         std::uint64_t requestId) {
  for (const auto &intent : intents) {
    const auto *message = std::get_if<RoomCommandResponse>(&intent.message);
    if (message != nullptr && message->requestId == RequestId{requestId}) {
      return message->result;
    }
  }
  return std::nullopt;
}

std::optional<RoomDetailProjection>
latestDetail(const std::vector<LobbyRoomOutboundIntent> &intents,
             std::uint64_t roomId) {
  std::optional<RoomDetailProjection> result;
  for (const auto &intent : intents) {
    const auto *detail = std::get_if<RoomDetailProjection>(&intent.message);
    if (detail != nullptr && detail->roomId == RoomId{roomId}) {
      result = *detail;
    }
  }
  return result;
}

bool hasMember(const RoomDetailProjection &detail, std::uint64_t sessionId,
               std::optional<bool> readyValue = std::nullopt) {
  for (const auto &member : detail.members) {
    if (member.sessionId == SessionId{sessionId} &&
        (!readyValue.has_value() || member.ready == *readyValue)) {
      return true;
    }
  }
  return false;
}

bool createJoinFullLeaveReleasesRejectedRoute() {
  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 32}};
  IntentCollector collector;
  RoomCommandGateway gateway{workers,
                             [&collector](LobbyRoomOutboundIntent intent) {
                               collector.add(std::move(intent));
                             }};
  const auto host = session(1);
  const auto member = session(2);
  const auto overflow = session(3);
  if (create(gateway, host, 1) != RoomSubmitResult::Accepted ||
      join(gateway, member, 2, 1) != RoomSubmitResult::Accepted ||
      join(gateway, overflow, 3, 1) != RoomSubmitResult::Accepted ||
      leave(gateway, member, 4) != RoomSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s) ||
      create(gateway, overflow, 5) != RoomSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }

  const auto intents = collector.take();
  const auto firstRoom = latestDetail(intents, 1);
  const auto secondRoom = latestDetail(intents, 2);
  return response(intents, 1) == RoomResultCode::Ok &&
         response(intents, 2) == RoomResultCode::Ok &&
         response(intents, 3) == RoomResultCode::RoomFull &&
         response(intents, 4) == RoomResultCode::Ok &&
         response(intents, 5) == RoomResultCode::Ok && firstRoom.has_value() &&
         firstRoom->members.size() == 1 && hasMember(*firstRoom, 1) &&
         secondRoom.has_value() && secondRoom->members.size() == 1 &&
         hasMember(*secondRoom, 3);
}

bool leaveThenQueuedRejoinKeepsRoute() {
  WorkerPool workers{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 32}};
  IntentCollector collector;
  Gate leaveResponse;
  RoomCommandGateway gateway{
      workers, [&collector, &leaveResponse](LobbyRoomOutboundIntent intent) {
        const auto *message = std::get_if<RoomCommandResponse>(&intent.message);
        const bool block =
            message != nullptr && message->requestId == RequestId{12};
        collector.add(std::move(intent));
        if (block) {
          leaveResponse.enterAndWait();
        }
      }};
  const auto host = session(10);
  const auto member = session(11);
  if (create(gateway, host, 10) != RoomSubmitResult::Accepted ||
      join(gateway, member, 11, 1) != RoomSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }
  static_cast<void>(collector.take());

  Gate blocker;
  if (!workers.submit([&blocker] { blocker.enterAndWait(); }) ||
      !blocker.waitFor(1) ||
      leave(gateway, member, 12) != RoomSubmitResult::Accepted ||
      join(gateway, member, 13, 1) != RoomSubmitResult::Accepted) {
    blocker.open();
    return false;
  }
  blocker.open();
  if (!leaveResponse.waitFor(1) ||
      ready(gateway, member, 14, true) != RoomSubmitResult::Accepted) {
    leaveResponse.open();
    return false;
  }
  leaveResponse.open();
  if (!workers.waitUntilIdle(2s)) {
    return false;
  }

  const auto intents = collector.take();
  const auto detail = latestDetail(intents, 1);
  return response(intents, 12) == RoomResultCode::Ok &&
         response(intents, 13) == RoomResultCode::Ok &&
         response(intents, 14) == RoomResultCode::Ok && detail.has_value() &&
         hasMember(*detail, 11, true);
}

bool concurrentJoinChoosesOneLiveRoom() {
  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 32}};
  IntentCollector collector;
  RoomCommandGateway gateway{workers,
                             [&collector](LobbyRoomOutboundIntent intent) {
                               collector.add(std::move(intent));
                             }};
  if (create(gateway, session(20), 20, 3) != RoomSubmitResult::Accepted ||
      create(gateway, session(30), 21, 3) != RoomSubmitResult::Accepted) {
    return false;
  }
  Gate start;
  const auto contender = session(40);
  std::atomic<bool> accepted{true};
  std::thread first{[&] {
    start.enterAndWait();
    if (join(gateway, contender, 22, 1) != RoomSubmitResult::Accepted) {
      accepted.store(false, std::memory_order_relaxed);
    }
  }};
  std::thread second{[&] {
    start.enterAndWait();
    if (join(gateway, contender, 23, 2) != RoomSubmitResult::Accepted) {
      accepted.store(false, std::memory_order_relaxed);
    }
  }};
  if (!start.waitFor(2)) {
    start.open();
    first.join();
    second.join();
    return false;
  }
  start.open();
  first.join();
  second.join();
  if (!accepted.load(std::memory_order_relaxed) || !workers.waitUntilIdle(2s)) {
    return false;
  }

  const auto intents = collector.take();
  const auto firstCode = response(intents, 22);
  const auto secondCode = response(intents, 23);
  const auto firstRoom = latestDetail(intents, 1);
  const auto secondRoom = latestDetail(intents, 2);
  const bool exactlyOneAccepted =
      (firstCode == RoomResultCode::Ok &&
       secondCode == RoomResultCode::AlreadyInRoom) ||
      (firstCode == RoomResultCode::AlreadyInRoom &&
       secondCode == RoomResultCode::Ok);
  return exactlyOneAccepted && firstRoom.has_value() &&
         secondRoom.has_value() &&
         (hasMember(*firstRoom, 40) != hasMember(*secondRoom, 40));
}

bool disconnectTransfersHostAndStaleGenerationCannotMutate() {
  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 32}};
  IntentCollector collector;
  RoomCommandGateway gateway{workers,
                             [&collector](LobbyRoomOutboundIntent intent) {
                               collector.add(std::move(intent));
                             }};
  const auto host = session(50);
  const auto member = session(51);
  if (create(gateway, host, 30) != RoomSubmitResult::Accepted ||
      join(gateway, member, 31, 1) != RoomSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }
  static_cast<void>(collector.take());

  if (gateway.disconnect(host.sessionId, SessionGeneration{99}) ||
      !gateway.disconnect(host.sessionId, host.generation) ||
      !workers.waitUntilIdle(2s) ||
      ready(gateway, session(51, 99), 32, true) != RoomSubmitResult::Accepted ||
      create(gateway, host, 33) != RoomSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }

  const auto intents = collector.take();
  const auto transferred = latestDetail(intents, 1);
  const auto newRoom = latestDetail(intents, 2);
  return response(intents, 32) == RoomResultCode::NotInRoom &&
         response(intents, 33) == RoomResultCode::Ok &&
         transferred.has_value() &&
         transferred->hostSessionId == member.sessionId &&
         hasMember(*transferred, 51, false) && newRoom.has_value() &&
         newRoom->hostSessionId == host.sessionId;
}

} // namespace

int main() {
  if (!createJoinFullLeaveReleasesRejectedRoute()) {
    return 1;
  }
  if (!leaveThenQueuedRejoinKeepsRoute()) {
    return 2;
  }
  if (!concurrentJoinChoosesOneLiveRoom()) {
    return 3;
  }
  if (!disconnectTransfersHostAndStaleGenerationCannotMutate()) {
    return 4;
  }
  return EXIT_SUCCESS;
}
