#include <lol/game_flow/RoomCommandGateway.hpp>
#include <lol/runtime/DeadlineScheduler.hpp>
#include <lol/runtime/WorkerPool.hpp>
#include <lol/settlement/SettlementCapacityGate.hpp>
#include <lol/settlement/SettlementPublication.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using lol::battle::BattleFinalResult;
using lol::game_flow::ArenaLoadCompleteRequest;
using lol::game_flow::ArenaLoadEntry;
using lol::game_flow::AuthenticatedRoomSession;
using lol::game_flow::CreateRoomRequest;
using lol::game_flow::GameplayTransportReadinessPort;
using lol::game_flow::HostStartRequest;
using lol::game_flow::JoinRoomRequest;
using lol::game_flow::LobbyRoomOutboundIntent;
using lol::game_flow::RoomCommandEnvelope;
using lol::game_flow::RoomCommandGateway;
using lol::game_flow::RoomSubmitResult;
using lol::game_flow::SetReadyRequest;
using lol::lobby_room::RoomDetailProjection;
using lol::lobby_room::RoomLifecycle;
using lol::runtime::DeadlineScheduler;
using lol::runtime::WorkerPool;
using lol::runtime::WorkerPoolConfig;
using lol::settlement::DurableAppendCompleted;
using lol::settlement::DurableAppendOutcome;
using lol::settlement::DurableAppendRequest;
using lol::settlement::OutboxBacklogSnapshot;
using lol::settlement::SettlementCapacityGate;
using lol::settlement::SettlementStoragePort;
using lol::settlement::SubmitAppendResult;
using lol::shared::AccountId;
using lol::shared::BattleInstanceId;
using lol::shared::RequestId;
using lol::shared::RoomId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;

AccountId account(std::uint8_t suffix) {
  AccountId::Bytes bytes{};
  bytes.back() = suffix;
  return AccountId{bytes};
}

AuthenticatedRoomSession session(std::uint64_t id, const char *nickname) {
  return {
      .accountId = account(static_cast<std::uint8_t>(id)),
      .sessionId = SessionId{id},
      .generation = SessionGeneration{1},
      .nickname = nickname,
  };
}

class AlwaysReady final : public GameplayTransportReadinessPort {
public:
  bool isReady(SessionId, SessionGeneration) const noexcept override {
    return true;
  }
};

class ManualDeadlines final : public DeadlineScheduler {
public:
  bool scheduleAfter(std::chrono::milliseconds delay, Task task) override {
    if (delay.count() < 0 || !task) {
      return false;
    }
    pending_.push_back(Pending{.due = now_ + delay, .task = std::move(task)});
    std::ranges::stable_sort(pending_, {}, &Pending::due);
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

private:
  class Lease final : public lol::runtime::DeadlineLease {
  public:
    Lease(ManualDeadlines &owner, std::uint64_t id) : owner_(&owner), id_(id) {}
    ~Lease() override { owner_->release(id_); }
    [[nodiscard]] bool armAfter(std::chrono::milliseconds delay,
                                Task task) override {
      return owner_->arm(id_, delay, std::move(task));
    }
    void cancel() noexcept override { owner_->cancel(id_); }

  private:
    ManualDeadlines *owner_;
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
    std::ranges::stable_sort(pending_, {}, &Pending::due);
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

  std::chrono::milliseconds now_{};
  std::uint64_t nextLeaseId_{1};
  std::vector<Pending> pending_;
  std::vector<std::uint64_t> leases_;
};

class ManualStorage final : public SettlementStoragePort {
public:
  SubmitAppendResult submit(DurableAppendRequest request,
                            CompletionSink completion) override {
    std::lock_guard lock{mutex_};
    request_ = std::move(request);
    completion_ = std::move(completion);
    return SubmitAppendResult::Accepted;
  }

  std::optional<DurableAppendRequest> request() const {
    std::lock_guard lock{mutex_};
    return request_;
  }

  bool complete(std::uint64_t sequence) {
    CompletionSink completion;
    DurableAppendRequest request = [&] {
      std::lock_guard lock{mutex_};
      completion = completion_;
      return *request_;
    }();
    if (!completion) {
      return false;
    }
    completion(DurableAppendOutcome{DurableAppendCompleted{
        .batchId = request.batchId,
        .roomId = request.roomId,
        .battleId = request.battleId,
        .commitSequence = sequence,
    }});
    return true;
  }

private:
  mutable std::mutex mutex_;
  std::optional<DurableAppendRequest> request_;
  CompletionSink completion_;
};

class OutboundCollector final {
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

bool accepted(RoomCommandGateway &gateway,
              const AuthenticatedRoomSession &actor,
              lol::game_flow::LobbyRoomClientMessage message,
              WorkerPool &workers) {
  return gateway.submit(RoomCommandEnvelope{.session = actor,
                                            .command = std::move(message)}) ==
             RoomSubmitResult::Accepted &&
         workers.waitUntilIdle(2s);
}

bool resultPrecedesOpenAndSameRoomStartsSecondBattle() {
  ManualDeadlines deadlines;
  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  AlwaysReady readiness;
  SettlementCapacityGate capacity;
  capacity.updateBacklog(OutboxBacklogSnapshot{
      .unretiredRecords = 0,
      .unretiredBytes = 0,
      .oldestPendingAge = 0ms,
      .storageHealthy = true,
  });
  ManualStorage storage;
  OutboundCollector outbound;
  RoomCommandGateway gateway{
      workers,
      readiness,
      [&outbound](LobbyRoomOutboundIntent intent) {
        outbound.add(std::move(intent));
      },
      {},
      {},
      deadlines,
      capacity,
      storage,
  };
  const auto host = session(1, "neo");
  const auto member = session(2, "trinity");
  if (!gateway.enterLobby(host) || !gateway.enterLobby(member) ||
      !accepted(gateway, host,
                CreateRoomRequest{
                    .requestId = RequestId{1}, .title = "room", .capacity = 2},
                workers) ||
      !accepted(gateway, member,
                JoinRoomRequest{.requestId = RequestId{2}, .roomId = RoomId{1}},
                workers) ||
      !accepted(gateway, host,
                SetReadyRequest{.requestId = RequestId{3}, .ready = true},
                workers) ||
      !accepted(gateway, member,
                SetReadyRequest{.requestId = RequestId{4}, .ready = true},
                workers) ||
      !accepted(gateway, host, HostStartRequest{.requestId = RequestId{5}},
                workers) ||
      !accepted(gateway, host,
                ArenaLoadCompleteRequest{.requestId = RequestId{6},
                                         .roomId = RoomId{1},
                                         .battleId = BattleInstanceId{1}},
                workers) ||
      !accepted(gateway, member,
                ArenaLoadCompleteRequest{.requestId = RequestId{7},
                                         .roomId = RoomId{1},
                                         .battleId = BattleInstanceId{1}},
                workers)) {
    return false;
  }
  outbound.take();
  deadlines.advance(30s);
  if (!workers.waitUntilIdle(2s) || !storage.request().has_value()) {
    return false;
  }
  const auto beforeDurability = outbound.take();
  if (std::ranges::any_of(beforeDurability, [](const auto &intent) {
        return std::holds_alternative<BattleFinalResult>(intent.message) ||
               (std::get_if<RoomDetailProjection>(&intent.message) != nullptr &&
                std::get<RoomDetailProjection>(intent.message).lifecycle ==
                    RoomLifecycle::Open);
      })) {
    return false;
  }

  if (!storage.complete(3u) || !workers.waitUntilIdle(2s)) {
    return false;
  }
  const auto reopened = outbound.take();
  const auto result = std::ranges::find_if(reopened, [](const auto &intent) {
    return std::holds_alternative<BattleFinalResult>(intent.message);
  });
  const auto detail = std::ranges::find_if(reopened, [](const auto &intent) {
    const auto *projection = std::get_if<RoomDetailProjection>(&intent.message);
    return projection != nullptr &&
           projection->lifecycle == RoomLifecycle::Open;
  });
  if (result == reopened.end() || detail == reopened.end() ||
      result >= detail ||
      std::ranges::any_of(
          std::get<RoomDetailProjection>(detail->message).members,
          [](const auto &value) { return value.ready; })) {
    return false;
  }

  if (!accepted(gateway, host,
                SetReadyRequest{.requestId = RequestId{8}, .ready = true},
                workers) ||
      !accepted(gateway, member,
                SetReadyRequest{.requestId = RequestId{9}, .ready = true},
                workers) ||
      !accepted(gateway, host, HostStartRequest{.requestId = RequestId{10}},
                workers)) {
    return false;
  }
  const auto second = outbound.take();
  if (!std::ranges::any_of(second,
                           [](const auto &intent) {
                             const auto *entry =
                                 std::get_if<ArenaLoadEntry>(&intent.message);
                             return entry != nullptr &&
                                    entry->roomId == RoomId{1} &&
                                    entry->battleId == BattleInstanceId{2};
                           }) ||
      !accepted(gateway, host,
                ArenaLoadCompleteRequest{.requestId = RequestId{11},
                                         .roomId = RoomId{1},
                                         .battleId = BattleInstanceId{2}},
                workers) ||
      !accepted(gateway, member,
                ArenaLoadCompleteRequest{.requestId = RequestId{12},
                                         .roomId = RoomId{1},
                                         .battleId = BattleInstanceId{2}},
                workers)) {
    return false;
  }
  outbound.take();
  deadlines.advance(30s);
  if (!workers.waitUntilIdle(2s)) {
    return false;
  }
  const auto secondRequest = storage.request();
  if (!secondRequest.has_value() ||
      secondRequest->battleId != BattleInstanceId{2} || !storage.complete(4u) ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }
  const auto reopenedAgain = outbound.take();
  const auto secondResult =
      std::ranges::find_if(reopenedAgain, [](const auto &intent) {
        const auto *value = std::get_if<BattleFinalResult>(&intent.message);
        return value != nullptr && value->battleId == BattleInstanceId{2};
      });
  const auto secondDetail =
      std::ranges::find_if(reopenedAgain, [](const auto &intent) {
        const auto *value = std::get_if<RoomDetailProjection>(&intent.message);
        return value != nullptr && value->lifecycle == RoomLifecycle::Open;
      });
  return secondResult != reopenedAgain.end() &&
         secondDetail != reopenedAgain.end() && secondResult < secondDetail;
}

} // namespace

int main() {
  return resultPrecedesOpenAndSameRoomStartsSecondBattle() ? EXIT_SUCCESS
                                                           : EXIT_FAILURE;
}
