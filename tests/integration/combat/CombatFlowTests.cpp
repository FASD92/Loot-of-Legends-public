#include "RudpCombatFlow.hpp"

#include "AuthClaimCoordinator.hpp"

#include <lol/game_flow/RoomCommandGateway.hpp>
#include <lol/meta/MetaClaimClient.hpp>
#include <lol/runtime/DeadlineScheduler.hpp>
#include <lol/runtime/WorkerPool.hpp>
#include <lol/session/SessionRegistry.hpp>
#include <lol/transport/rudp/RudpBindingRegistry.hpp>
#include <lol/transport/rudp/RudpCombatCodec.hpp>
#include <lol/transport/rudp/RudpLootCodec.hpp>
#include <lol/transport/rudp/RudpPeer.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
using lol::app::AppliedClaimKind;
using lol::app::AuthClaimCoordinator;
using lol::app::EncodedRudpDatagram;
using lol::app::RudpCombatFlow;
using lol::app::RudpCombatPollResult;
using lol::app::RudpCombatSubmitResult;
using lol::app::RudpGameplayReadiness;
using lol::app::RudpPeerFailure;
using lol::app::RudpPeerFailureReason;
using lol::battle::AttackResultCode;
using lol::battle::AttackTerminalResult;
using lol::battle::ClaimLootResultCode;
using lol::battle::CombatOutcome;
using lol::battle::CombatProjection;
using lol::battle::CombatTerminalRecord;
using lol::battle::CommandId;
using lol::battle::EventId;
using lol::battle::MonsterState;
using lol::battle::StateSnapshotProjection;
using lol::game_flow::ArenaGameplayStart;
using lol::game_flow::AuthenticatedRoomSession;
using lol::game_flow::BattleParticipantProjection;
using lol::game_flow::CombatAttackResultOutbound;
using lol::game_flow::CombatBattleRetiredOutbound;
using lol::game_flow::CombatMonsterSpawnedOutbound;
using lol::game_flow::CombatMonsterStateOutbound;
using lol::game_flow::CombatOutboundIntent;
using lol::game_flow::CombatTerminalEventOutbound;
using lol::game_flow::CreateRoomRequest;
using lol::game_flow::HostStartRequest;
using lol::game_flow::JoinRoomRequest;
using lol::game_flow::LeaveRoomRequest;
using lol::game_flow::LobbyRoomOutboundIntent;
using lol::game_flow::LootClaimResultOutbound;
using lol::game_flow::RoomCommandEnvelope;
using lol::game_flow::RoomCommandGateway;
using lol::game_flow::RoomSubmitResult;
using lol::game_flow::SetReadyRequest;
using lol::meta::ClaimCompletion;
using lol::meta::ClaimedIdentity;
using lol::meta::ClaimOutcome;
using lol::runtime::WorkerPool;
using lol::runtime::WorkerPoolConfig;
using lol::session::SessionRegistry;
using lol::shared::AccountId;
using lol::shared::BattleInstanceId;
using lol::shared::RequestId;
using lol::shared::RoomId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;
using lol::transport::rudp::ReliableQueueAdmission;
using lol::transport::rudp::RudpAttackIntent;
using lol::transport::rudp::RudpAttackResultCode;
using lol::transport::rudp::RudpAttackTerminalResult;
using lol::transport::rudp::RudpBindHello;
using lol::transport::rudp::RudpBindingRegistry;
using lol::transport::rudp::RudpBindStatus;
using lol::transport::rudp::RudpClaimLootIntent;
using lol::transport::rudp::RudpClaimLootTerminalResult;
using lol::transport::rudp::RudpCombatCodec;
using lol::transport::rudp::RudpCombatMessage;
using lol::transport::rudp::RudpCombatOutcome;
using lol::transport::rudp::RudpCombatTerminalEvent;
using lol::transport::rudp::RudpCommandId;
using lol::transport::rudp::RudpDropSpawned;
using lol::transport::rudp::RudpDropStateSnapshot;
using lol::transport::rudp::RudpEndpoint;
using lol::transport::rudp::RudpEventId;
using lol::transport::rudp::RudpEventStreamKind;
using lol::transport::rudp::RudpFlag;
using lol::transport::rudp::RudpHeader;
using lol::transport::rudp::RudpLootCodec;
using lol::transport::rudp::RudpLootMessage;
using lol::transport::rudp::RudpMonsterSpawned;
using lol::transport::rudp::RudpMonsterState;
using lol::transport::rudp::RudpMonsterStateSnapshot;

constexpr auto kStart = std::chrono::steady_clock::time_point{};

AccountId account(std::uint8_t suffix) {
  AccountId::Bytes bytes{};
  bytes.back() = suffix;
  return AccountId{bytes};
}

AuthenticatedRoomSession session(std::uint64_t id) {
  return {.accountId = account(static_cast<std::uint8_t>(id)),
          .sessionId = SessionId{id},
          .generation = SessionGeneration{1},
          .nickname = "player-" + std::to_string(id)};
}

RudpEndpoint endpoint(std::uint8_t suffix) {
  RudpEndpoint value{.address = {},
                     .port = static_cast<std::uint16_t>(4000 + suffix),
                     .scopeId = 0};
  value.address.back() = static_cast<std::byte>(suffix);
  return value;
}

std::optional<std::uint32_t> bind(RudpBindingRegistry &bindings,
                                  std::uint64_t sessionId) {
  const auto capability = bindings.requestCapability(sessionId, 1, kStart);
  if (!capability.has_value()) {
    return std::nullopt;
  }
  const auto result =
      bindings.bind(RudpHeader{.flag = RudpFlag::Reliable,
                               .sessionId = sessionId,
                               .sessionGeneration = 1,
                               .transportEpoch = 0,
                               .sequence = 1,
                               .ack = 0,
                               .ackBits = 0,
                               .messageId = 22},
                    RudpBindHello{*capability},
                    endpoint(static_cast<std::uint8_t>(sessionId)), kStart);
  return result.status == RudpBindStatus::Accepted
             ? std::optional{result.transportEpoch}
             : std::nullopt;
}

std::optional<std::uint32_t> bindGeneration(RudpBindingRegistry &bindings,
                                            std::uint64_t sessionId,
                                            std::uint64_t generation) {
  const auto capability =
      bindings.requestCapability(sessionId, generation, kStart);
  if (!capability.has_value()) {
    return std::nullopt;
  }
  const auto result =
      bindings.bind(RudpHeader{.flag = RudpFlag::Reliable,
                               .sessionId = sessionId,
                               .sessionGeneration = generation,
                               .transportEpoch = 0,
                               .sequence = 1,
                               .ack = 0,
                               .ackBits = 0,
                               .messageId = 22},
                    RudpBindHello{*capability},
                    endpoint(static_cast<std::uint8_t>(sessionId)), kStart);
  return result.status == RudpBindStatus::Accepted
             ? std::optional{result.transportEpoch}
             : std::nullopt;
}

std::optional<std::vector<std::byte>> attackDatagram(std::uint64_t sessionId,
                                                     std::uint32_t epoch,
                                                     std::uint32_t transportSeq,
                                                     std::uint64_t commandIdLow,
                                                     std::uint64_t target) {
  return RudpCombatCodec::encode(
      RudpHeader{.flag = RudpFlag::Reliable,
                 .sessionId = sessionId,
                 .sessionGeneration = 1,
                 .transportEpoch = epoch,
                 .sequence = transportSeq,
                 .ack = 1,
                 .ackBits = 0,
                 .messageId = 27},
      RudpCombatMessage{RudpAttackIntent{
          .commandId = RudpCommandId{.high = 0, .low = commandIdLow},
          .battleInstanceId = 1,
          .targetHint = target,
      }});
}

std::optional<std::vector<std::byte>>
claimLootDatagram(std::uint64_t sessionId, std::uint32_t epoch,
                  std::uint32_t transportSeq, std::uint64_t commandIdLow,
                  std::uint64_t dropId) {
  return RudpLootCodec::encode(
      RudpHeader{.flag = RudpFlag::Reliable,
                 .sessionId = sessionId,
                 .sessionGeneration = 1,
                 .transportEpoch = epoch,
                 .sequence = transportSeq,
                 .ack = 1,
                 .ackBits = 0,
                 .messageId = 32},
      RudpLootMessage{RudpClaimLootIntent{
          .commandId = RudpCommandId{.high = 0, .low = commandIdLow},
          .battleInstanceId = 1,
          .dropId = dropId,
      }});
}

bool matchesFailure(const RudpPeerFailure &failure, std::uint64_t sessionId,
                    std::uint64_t generation, std::uint32_t transportEpoch,
                    std::uint32_t sequence, RudpPeerFailureReason reason) {
  return failure.sessionId == SessionId{sessionId} &&
         failure.generation == SessionGeneration{generation} &&
         failure.transportEpoch == transportEpoch &&
         failure.sequence == sequence && failure.reason == reason;
}

class IntentCollector final {
public:
  void add(LobbyRoomOutboundIntent intent) {
    std::lock_guard lock{mutex_};
    intents_.push_back(std::move(intent));
  }

  std::size_t gameplayStarts() const {
    std::lock_guard lock{mutex_};
    return static_cast<std::size_t>(
        std::count_if(intents_.begin(), intents_.end(), [](const auto &intent) {
          return std::holds_alternative<ArenaGameplayStart>(intent.message);
        }));
  }

  std::vector<LobbyRoomOutboundIntent> take() {
    std::lock_guard lock{mutex_};
    return std::exchange(intents_, {});
  }

private:
  mutable std::mutex mutex_;
  std::vector<LobbyRoomOutboundIntent> intents_;
};

class CombatIntentCollector final {
public:
  void add(CombatOutboundIntent intent) {
    std::lock_guard lock{mutex_};
    intents_.push_back(std::move(intent));
  }

  std::vector<CombatOutboundIntent> take() {
    std::lock_guard lock{mutex_};
    return std::exchange(intents_, {});
  }

private:
  mutable std::mutex mutex_;
  std::vector<CombatOutboundIntent> intents_;
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

void feedCombat(RudpCombatFlow &flow, CombatIntentCollector &collector) {
  for (auto intent : collector.take()) {
    static_cast<void>(flow.handleCombatOutbound(std::move(intent)));
  }
}

class ManualDeadlineScheduler final : public lol::runtime::DeadlineScheduler {
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

bool createCommittedRoom(RoomCommandGateway &gateway, WorkerPool &workers) {
  const auto host = session(1);
  const auto member = session(2);
  return gateway.submit(RoomCommandEnvelope{
             .session = host,
             .command = CreateRoomRequest{.requestId = RequestId{1},
                                          .title = "room",
                                          .capacity = 2}}) ==
             RoomSubmitResult::Accepted &&
         gateway.submit(RoomCommandEnvelope{
             .session = member,
             .command = JoinRoomRequest{.requestId = RequestId{2},
                                        .roomId = RoomId{1}}}) ==
             RoomSubmitResult::Accepted &&
         gateway.submit(RoomCommandEnvelope{
             .session = host,
             .command =
                 SetReadyRequest{.requestId = RequestId{3}, .ready = true}}) ==
             RoomSubmitResult::Accepted &&
         gateway.submit(RoomCommandEnvelope{
             .session = member,
             .command =
                 SetReadyRequest{.requestId = RequestId{4}, .ready = true}}) ==
             RoomSubmitResult::Accepted &&
         workers.waitUntilIdle(2s) &&
         gateway.submit(RoomCommandEnvelope{
             .session = host,
             .command = HostStartRequest{.requestId = RequestId{5}}}) ==
             RoomSubmitResult::Accepted &&
         workers.waitUntilIdle(2s) &&
         gateway.submit(
             RoomCommandEnvelope{.session = host,
                                 .command =
                                     lol::game_flow::ArenaLoadCompleteRequest{
                                         .requestId = RequestId{6},
                                         .roomId = RoomId{1},
                                         .battleId = BattleInstanceId{1}}}) ==
             RoomSubmitResult::Accepted &&
         gateway.submit(
             RoomCommandEnvelope{.session = member,
                                 .command =
                                     lol::game_flow::ArenaLoadCompleteRequest{
                                         .requestId = RequestId{7},
                                         .roomId = RoomId{1},
                                         .battleId = BattleInstanceId{1}}}) ==
             RoomSubmitResult::Accepted &&
         workers.waitUntilIdle(2s);
}

// Parameterized committed-room helper used by the repeated-lifecycle test so
// the same composition flow can run several real room lifecycles back to back.
bool createCommittedRoomAt(RoomCommandGateway &gateway, WorkerPool &workers,
                           std::uint64_t roomId, std::uint64_t requestBase) {
  const auto host = session(1);
  const auto member = session(2);
  return gateway.submit(RoomCommandEnvelope{
             .session = host,
             .command =
                 CreateRoomRequest{.requestId = RequestId{requestBase + 1},
                                   .title = "room",
                                   .capacity = 2}}) ==
             RoomSubmitResult::Accepted &&
         gateway.submit(RoomCommandEnvelope{
             .session = member,
             .command = JoinRoomRequest{.requestId = RequestId{requestBase + 2},
                                        .roomId = RoomId{roomId}}}) ==
             RoomSubmitResult::Accepted &&
         gateway.submit(RoomCommandEnvelope{
             .session = host,
             .command = SetReadyRequest{.requestId = RequestId{requestBase + 3},
                                        .ready = true}}) ==
             RoomSubmitResult::Accepted &&
         gateway.submit(RoomCommandEnvelope{
             .session = member,
             .command = SetReadyRequest{.requestId = RequestId{requestBase + 4},
                                        .ready = true}}) ==
             RoomSubmitResult::Accepted &&
         workers.waitUntilIdle(2s) &&
         gateway.submit(RoomCommandEnvelope{
             .session = host,
             .command =
                 HostStartRequest{.requestId = RequestId{requestBase + 5}}}) ==
             RoomSubmitResult::Accepted &&
         workers.waitUntilIdle(2s) &&
         gateway.submit(RoomCommandEnvelope{
             .session = host,
             .command =
                 lol::game_flow::ArenaLoadCompleteRequest{
                     .requestId = RequestId{requestBase + 6},
                     .roomId = RoomId{roomId},
                     .battleId = BattleInstanceId{1}}}) ==
             RoomSubmitResult::Accepted &&
         gateway.submit(RoomCommandEnvelope{
             .session = member,
             .command =
                 lol::game_flow::ArenaLoadCompleteRequest{
                     .requestId = RequestId{requestBase + 7},
                     .roomId = RoomId{roomId},
                     .battleId = BattleInstanceId{1}}}) ==
             RoomSubmitResult::Accepted &&
         workers.waitUntilIdle(2s);
}

bool reorderedReliableCommandsReachApplicationExactlyOnce() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  ManualDeadlineScheduler deadlines;
  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{
      workers,
      readiness,
      [&intents](LobbyRoomOutboundIntent intent) {
        intents.add(std::move(intent));
      },
      [](StateSnapshotProjection) {},
      [&combat](CombatOutboundIntent intent) { combat.add(std::move(intent)); },
      deadlines};
  RudpCombatFlow flow{bindings, gateway};
  if (!createCommittedRoom(gateway, workers)) {
    return false;
  }
  feedCombat(flow, combat);

  const auto newestAttack = attackDatagram(1, *hostEpoch, 3, 101, 1);
  const auto reorderedAttack = attackDatagram(1, *hostEpoch, 2, 102, 1);
  const auto newestClaim = claimLootDatagram(1, *hostEpoch, 5, 201, 99);
  const auto reorderedClaim = claimLootDatagram(1, *hostEpoch, 4, 202, 100);
  if (!newestAttack.has_value() || !reorderedAttack.has_value() ||
      !newestClaim.has_value() || !reorderedClaim.has_value()) {
    return false;
  }

  const auto newestAttackResult =
      flow.submitAttack(*newestAttack, endpoint(1), kStart);
  const auto reorderedAttackResult =
      flow.submitAttack(*reorderedAttack, endpoint(1), kStart + 750ms);
  const auto newestClaimResult =
      flow.submitClaimLoot(*newestClaim, endpoint(1), kStart + 750ms);
  const auto reorderedClaimResult =
      flow.submitClaimLoot(*reorderedClaim, endpoint(1), kStart + 750ms);
  const auto commandsDrained = workers.waitUntilIdle(2s);
  if (newestAttackResult != RudpCombatSubmitResult::Accepted ||
      reorderedAttackResult != RudpCombatSubmitResult::Accepted ||
      newestClaimResult != RudpCombatSubmitResult::Accepted ||
      reorderedClaimResult != RudpCombatSubmitResult::Accepted ||
      !commandsDrained) {
    return false;
  }
  std::size_t acceptedAttackResults = 0;
  std::size_t rejectedClaimResults = 0;
  for (const auto &intent : combat.take()) {
    if (const auto *result =
            std::get_if<CombatAttackResultOutbound>(&intent.message);
        result != nullptr) {
      if ((result->result.commandId == CommandId{.high = 0, .low = 101} &&
           result->result.remainingHitPoints == 1580) ||
          (result->result.commandId == CommandId{.high = 0, .low = 102} &&
           result->result.remainingHitPoints == 1560)) {
        ++acceptedAttackResults;
      }
    }
    if (const auto *result =
            std::get_if<LootClaimResultOutbound>(&intent.message);
        result != nullptr &&
        (result->result.commandId == CommandId{.high = 0, .low = 201} ||
         result->result.commandId == CommandId{.high = 0, .low = 202}) &&
        result->result.code == ClaimLootResultCode::NotEligible) {
      ++rejectedClaimResults;
    }
  }
  if (acceptedAttackResults != 2 || rejectedClaimResults != 2) {
    return false;
  }

  return flow.submitAttack(*reorderedAttack, endpoint(1), kStart + 1s) ==
             RudpCombatSubmitResult::StaleTransport &&
         flow.submitClaimLoot(*reorderedClaim, endpoint(1), kStart + 1s) ==
             RudpCombatSubmitResult::StaleTransport &&
         workers.waitUntilIdle(2s) && combat.take().empty();
}

bool roomAdmissionFailurePreservesPeerIdentityWithoutMutation() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  WorkerPool workers{WorkerPoolConfig{.threadCount = 1, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  Gate blockedSink;
  std::atomic<bool> blockHostResult{false};
  RoomCommandGateway gateway{
      workers, readiness,
      [&intents](LobbyRoomOutboundIntent intent) {
        intents.add(std::move(intent));
      },
      [](StateSnapshotProjection) {},
      [&combat, &blockedSink, &blockHostResult](CombatOutboundIntent intent) {
        if (blockHostResult.load(std::memory_order_acquire) &&
            intent.actorSessionId == SessionId{1} &&
            std::holds_alternative<CombatAttackResultOutbound>(
                intent.message)) {
          blockedSink.enterAndWait();
        }
        combat.add(std::move(intent));
      }};
  RudpCombatFlow flow{bindings, gateway};
  if (!createCommittedRoom(gateway, workers)) {
    return false;
  }
  feedCombat(flow, combat);

  blockHostResult.store(true, std::memory_order_release);
  const auto blocker = attackDatagram(1, *hostEpoch, 2, 1, 1);
  if (!blocker.has_value() ||
      flow.submitAttack(*blocker, endpoint(1), kStart) !=
          RudpCombatSubmitResult::Accepted ||
      !blockedSink.waitFor(1)) {
    blockedSink.open();
    return false;
  }

  RudpCombatSubmitResult rejected = RudpCombatSubmitResult::Accepted;
  std::uint32_t rejectedSequence = 0;
  std::uint64_t rejectedCommandId = 0;
  for (std::uint32_t sequence = 3; sequence < 303; ++sequence) {
    const std::uint64_t commandId = 1000u + sequence;
    const auto datagram = attackDatagram(1, *hostEpoch, sequence, commandId, 1);
    if (!datagram.has_value()) {
      blockedSink.open();
      return false;
    }
    const auto result =
        flow.submitAttack(*datagram, endpoint(1), kStart + sequence * 1ms);
    if (result == RudpCombatSubmitResult::RoomRejected) {
      rejected = result;
      rejectedSequence = sequence;
      rejectedCommandId = commandId;
      break;
    }
    if (result != RudpCombatSubmitResult::Accepted) {
      blockedSink.open();
      return false;
    }
  }

  const auto polled = flow.pollReliable(std::chrono::steady_clock::now());
  blockedSink.open();
  if (!workers.waitUntilIdle(2s)) {
    return false;
  }
  const bool rejectedCommandMutated = std::ranges::any_of(
      combat.take(), [rejectedCommandId](const auto &intent) {
        const auto *result =
            std::get_if<CombatAttackResultOutbound>(&intent.message);
        return result != nullptr &&
               result->result.commandId.low == rejectedCommandId;
      });
  return rejected == RudpCombatSubmitResult::RoomRejected &&
         rejectedSequence != 0 && !rejectedCommandMutated &&
         std::ranges::any_of(polled.failures, [&](const auto &failure) {
           return matchesFailure(failure, 1, 1, *hostEpoch, rejectedSequence,
                                 RudpPeerFailureReason::RoomAdmissionRejected);
         });
}

bool realBindingDrivesAttackTerminalAndSnapshot() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{workers, readiness,
                             [&intents](LobbyRoomOutboundIntent intent) {
                               intents.add(std::move(intent));
                             },
                             [](StateSnapshotProjection) {},
                             [&combat](CombatOutboundIntent intent) {
                               combat.add(std::move(intent));
                             }};
  RudpCombatFlow flow{bindings, gateway};

  if (!createCommittedRoom(gateway, workers) || intents.gameplayStarts() != 1) {
    return false;
  }
  bool sawSpawnIntent = false;
  for (auto intent : combat.take()) {
    if (std::holds_alternative<CombatMonsterSpawnedOutbound>(intent.message)) {
      sawSpawnIntent = true;
    }
    static_cast<void>(flow.handleCombatOutbound(std::move(intent)));
  }
  if (!sawSpawnIntent) {
    return false;
  }

  auto polled = flow.pollReliable(std::chrono::steady_clock::now());
  std::size_t spawnTransmissions = 0;
  for (const auto &transmission : polled.transmissions) {
    const auto decoded = RudpCombatCodec::decode(transmission.datagram);
    const auto *spawn = decoded.message.has_value()
                            ? std::get_if<RudpMonsterSpawned>(&*decoded.message)
                            : nullptr;
    if (spawn != nullptr && spawn->eventSequence == 1 &&
        spawn->eventStreamKind == RudpEventStreamKind::CombatLifecycle &&
        spawn->battleInstanceId == 1 && spawn->maximumHitPoints == 1600) {
      ++spawnTransmissions;
    }
  }
  if (spawnTransmissions != 2) {
    return false;
  }

  const auto first = attackDatagram(1, *hostEpoch, 2, 1, 1);
  if (!first.has_value() || flow.submitAttack(*first, endpoint(1), kStart) !=
                                RudpCombatSubmitResult::Accepted) {
    return false;
  }
  if (!workers.waitUntilIdle(2s)) {
    return false;
  }
  feedCombat(flow, combat);

  bool sawAcceptedResult = false;
  polled = flow.pollReliable(std::chrono::steady_clock::now());
  for (const auto &transmission : polled.transmissions) {
    const auto decoded = RudpCombatCodec::decode(transmission.datagram);
    const auto *result =
        decoded.message.has_value()
            ? std::get_if<RudpAttackTerminalResult>(&*decoded.message)
            : nullptr;
    if (result != nullptr && result->commandId.low == 1 &&
        result->resultCode == RudpAttackResultCode::Ok &&
        result->remainingHitPoints == 1580) {
      sawAcceptedResult = true;
    }
  }
  bool sawHpSnapshot = false;
  for (const auto &snapshot : flow.takeUnreliableSnapshots()) {
    const auto decoded = RudpCombatCodec::decode(snapshot.datagram);
    const auto *state =
        decoded.message.has_value()
            ? std::get_if<RudpMonsterStateSnapshot>(&*decoded.message)
            : nullptr;
    if (state != nullptr && state->hitPoints == 1580 &&
        state->monsterState == RudpMonsterState::Alive) {
      sawHpSnapshot = true;
    }
  }
  if (!sawAcceptedResult || !sawHpSnapshot) {
    return false;
  }

  if (flow.submitAttack(*first, endpoint(1), kStart) !=
          RudpCombatSubmitResult::StaleTransport ||
      !workers.waitUntilIdle(2s) || !combat.take().empty()) {
    return false;
  }

  const auto retry = attackDatagram(1, *hostEpoch, 3, 1, 1);
  if (!retry.has_value() ||
      flow.submitAttack(*retry, endpoint(1), kStart) !=
          RudpCombatSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }
  bool sawRetainedResult = false;
  for (auto intent : combat.take()) {
    const auto *result =
        std::get_if<CombatAttackResultOutbound>(&intent.message);
    if (result != nullptr && result->result.remainingHitPoints == 1580) {
      sawRetainedResult = true;
    }
    static_cast<void>(flow.handleCombatOutbound(std::move(intent)));
  }
  if (!sawRetainedResult) {
    return false;
  }
  for (const auto &snapshot : flow.takeUnreliableSnapshots()) {
    const auto decoded = RudpCombatCodec::decode(snapshot.datagram);
    const auto *state =
        decoded.message.has_value()
            ? std::get_if<RudpMonsterStateSnapshot>(&*decoded.message)
            : nullptr;
    if (state == nullptr || state->hitPoints != 1580) {
      return false;
    }
  }
  return true;
}

bool combatFlowEmitsStableLifecycleEvents() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{workers, readiness,
                             [&intents](LobbyRoomOutboundIntent intent) {
                               intents.add(std::move(intent));
                             },
                             [](StateSnapshotProjection) {},
                             [&combat](CombatOutboundIntent intent) {
                               combat.add(std::move(intent));
                             }};
  RudpCombatFlow flow{bindings, gateway};

  if (!createCommittedRoom(gateway, workers) || intents.gameplayStarts() != 1) {
    return false;
  }
  feedCombat(flow, combat);

  const std::array<std::uint32_t, 3> epochs{0, *hostEpoch, *memberEpoch};
  std::array<std::uint32_t, 3> nextTransportSequence{0, 2, 2};
  std::uint64_t commandId = 1;
  for (std::uint32_t round = 0; round < 40; ++round) {
    for (std::uint64_t sessionId = 1; sessionId <= 2; ++sessionId) {
      const auto datagram =
          attackDatagram(sessionId, epochs[sessionId],
                         nextTransportSequence[sessionId]++, commandId++, 1);
      if (!datagram.has_value() ||
          flow.submitAttack(
              *datagram, endpoint(static_cast<std::uint8_t>(sessionId)),
              kStart + round * 750ms) != RudpCombatSubmitResult::Accepted) {
        return false;
      }
    }
  }
  if (!workers.waitUntilIdle(2s)) {
    return false;
  }
  feedCombat(flow, combat);

  bool sawSpawnEvent = false;
  bool sawTerminalEvent = false;
  std::size_t terminalEventCount = 0;
  const auto polled = flow.pollReliable(std::chrono::steady_clock::now());
  for (const auto &transmission : polled.transmissions) {
    const auto decoded = RudpCombatCodec::decode(transmission.datagram);
    if (const auto *spawn =
            decoded.message.has_value()
                ? std::get_if<RudpMonsterSpawned>(&*decoded.message)
                : nullptr;
        spawn != nullptr && spawn->eventSequence == 1 &&
        spawn->eventId == RudpEventId{.high = 1, .low = 1} &&
        spawn->eventStreamKind == RudpEventStreamKind::CombatLifecycle) {
      sawSpawnEvent = true;
    }
    if (const auto *terminal =
            decoded.message.has_value()
                ? std::get_if<RudpCombatTerminalEvent>(&*decoded.message)
                : nullptr;
        terminal != nullptr && terminal->eventSequence == 2 &&
        terminal->eventId == RudpEventId{.high = 1, .low = 2} &&
        terminal->eventStreamKind == RudpEventStreamKind::CombatLifecycle &&
        terminal->combatOutcome == RudpCombatOutcome::MonsterDefeated &&
        terminal->monsterId == 1) {
      ++terminalEventCount;
      sawTerminalEvent = true;
    }
  }
  if (!sawSpawnEvent || !sawTerminalEvent || terminalEventCount != 2) {
    return false;
  }

  const auto snapshots = flow.takeUnreliableSnapshots();
  if (snapshots.size() != 162) {
    return false;
  }
  std::size_t combatSnapshotCount = 0;
  std::size_t lootSnapshotCount = 0;
  std::optional<RudpMonsterStateSnapshot> terminalState;
  for (const auto &snapshot : snapshots) {
    const auto decoded = RudpCombatCodec::decode(snapshot.datagram);
    const auto *state =
        decoded.message.has_value()
            ? std::get_if<RudpMonsterStateSnapshot>(&*decoded.message)
            : nullptr;
    if (state != nullptr) {
      ++combatSnapshotCount;
      terminalState = *state;
      continue;
    }
    const auto loot = RudpLootCodec::decode(snapshot.datagram);
    if (loot.message.has_value() &&
        std::holds_alternative<RudpDropStateSnapshot>(*loot.message)) {
      ++lootSnapshotCount;
    } else {
      return false;
    }
  }
  return combatSnapshotCount == 160 && lootSnapshotCount == 2 &&
         terminalState.has_value() && terminalState->hitPoints == 0 &&
         terminalState->monsterState == RudpMonsterState::Dead;
}

bool combatFlowReliableQueueAdmissionAckAndExpiry() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{workers, readiness,
                             [&intents](LobbyRoomOutboundIntent intent) {
                               intents.add(std::move(intent));
                             },
                             [](StateSnapshotProjection) {},
                             [&combat](CombatOutboundIntent intent) {
                               combat.add(std::move(intent));
                             }};
  RudpCombatFlow flow{bindings, gateway};

  if (!createCommittedRoom(gateway, workers) || intents.gameplayStarts() != 1) {
    return false;
  }
  feedCombat(flow, combat);

  const auto first = attackDatagram(1, *hostEpoch, 2, 1, 1);
  if (!first.has_value() ||
      flow.submitAttack(*first, endpoint(1), kStart) !=
          RudpCombatSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }
  feedCombat(flow, combat);

  const auto now = std::chrono::steady_clock::now();
  auto polled = flow.pollReliable(now);
  std::optional<std::uint32_t> resultSequence;
  for (const auto &transmission : polled.transmissions) {
    const auto decoded = RudpCombatCodec::decode(transmission.datagram);
    const auto *result =
        decoded.message.has_value()
            ? std::get_if<RudpAttackTerminalResult>(&*decoded.message)
            : nullptr;
    if (result != nullptr && result->commandId.low == 1) {
      resultSequence = decoded.header->sequence;
    }
  }
  if (!resultSequence.has_value() ||
      flow.discardAcknowledged(1, 1, *hostEpoch, *resultSequence, 0) != 1) {
    return false;
  }

  polled = flow.pollReliable(now + 1ms);
  for (const auto &transmission : polled.transmissions) {
    const auto decoded = RudpCombatCodec::decode(transmission.datagram);
    if (decoded.header.has_value() &&
        decoded.header->sequence == *resultSequence) {
      return false;
    }
  }

  const auto afterExpiry = flow.pollReliable(now + 5000ms);
  if (std::ranges::none_of(afterExpiry.failures, [&](const auto &failure) {
        return matchesFailure(failure, 1, 1, *hostEpoch, 2,
                              RudpPeerFailureReason::ReliableExpired);
      })) {
    return false;
  }

  const auto retry = attackDatagram(1, *hostEpoch, 3, 1, 1);
  if (!retry.has_value() ||
      flow.submitAttack(*retry, endpoint(1), kStart) !=
          RudpCombatSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }
  bool recovered = false;
  for (auto intent : combat.take()) {
    const auto *result =
        std::get_if<CombatAttackResultOutbound>(&intent.message);
    if (result != nullptr && result->result.remainingHitPoints == 1580) {
      recovered = true;
    }
    static_cast<void>(flow.handleCombatOutbound(std::move(intent)));
  }
  if (!recovered) {
    return false;
  }
  for (const auto &snapshot : flow.takeUnreliableSnapshots()) {
    const auto decoded = RudpCombatCodec::decode(snapshot.datagram);
    const auto *state =
        decoded.message.has_value()
            ? std::get_if<RudpMonsterStateSnapshot>(&*decoded.message)
            : nullptr;
    if (state == nullptr || state->hitPoints != 1580) {
      return false;
    }
  }
  return true;
}

bool combatFlowSnapshotServerTickAndSharedSequence() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{workers, readiness,
                             [&intents](LobbyRoomOutboundIntent intent) {
                               intents.add(std::move(intent));
                             },
                             [](StateSnapshotProjection) {},
                             [&combat](CombatOutboundIntent intent) {
                               combat.add(std::move(intent));
                             }};
  RudpCombatFlow flow{bindings, gateway};

  if (!createCommittedRoom(gateway, workers) || intents.gameplayStarts() != 1) {
    return false;
  }
  feedCombat(flow, combat);

  if (gateway.submitMovementTick(RoomId{1}, BattleInstanceId{1}, 7) !=
          RoomSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }

  const auto attack = attackDatagram(1, *hostEpoch, 2, 1, 1);
  if (!attack.has_value() ||
      flow.submitAttack(*attack, endpoint(1), kStart) !=
          RudpCombatSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }
  feedCombat(flow, combat);

  const auto snapshots = flow.takeUnreliableSnapshots();
  if (snapshots.size() != 2) {
    return false;
  }
  std::optional<std::uint32_t> sharedSequence;
  for (const auto &snapshot : snapshots) {
    const auto decoded = RudpCombatCodec::decode(snapshot.datagram);
    const auto *state =
        decoded.message.has_value()
            ? std::get_if<RudpMonsterStateSnapshot>(&*decoded.message)
            : nullptr;
    if (state == nullptr || state->serverTick != 7 ||
        state->hitPoints != 1580 ||
        state->monsterState != RudpMonsterState::Alive) {
      return false;
    }
    if (sharedSequence.has_value()) {
      if (*sharedSequence != state->snapshotSequence) {
        return false;
      }
    } else {
      sharedSequence = state->snapshotSequence;
    }
  }
  return sharedSequence.has_value() && *sharedSequence != 0;
}

bool combatFlowRebindCannotDeliverOldGenerationOutput() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{workers, readiness,
                             [&intents](LobbyRoomOutboundIntent intent) {
                               intents.add(std::move(intent));
                             },
                             [](StateSnapshotProjection) {},
                             [&combat](CombatOutboundIntent intent) {
                               combat.add(std::move(intent));
                             }};
  RudpCombatFlow flow{bindings, gateway};

  if (!createCommittedRoom(gateway, workers) || intents.gameplayStarts() != 1) {
    return false;
  }
  feedCombat(flow, combat);

  // The spawn is enqueued for both gen-1 participants; mark transmitted so the
  // old-epoch queue entry is ACK-discardable.
  static_cast<void>(flow.pollReliable(std::chrono::steady_clock::now()));

  // Rebind session 1 to a NEWER generation. The captured battle still refers to
  // generation 1, so combat output must not be routed to the new binding.
  const auto reboundEpoch = bindGeneration(bindings, 1, 2);
  if (!reboundEpoch.has_value() || *reboundEpoch == *hostEpoch) {
    return false;
  }

  // A newer generation/epoch ACK must not find the old-epoch queue, and an old
  // epoch ACK still discards the old-epoch spawn entry (sequence 2).
  if (flow.discardAcknowledged(1, 2, *reboundEpoch, 2, 0) != 0) {
    return false;
  }
  if (flow.discardAcknowledged(1, 1, *hostEpoch, 2, 0) != 1) {
    return false;
  }

  CombatOutboundIntent stateIntent{
      .actorSessionId = std::nullopt,
      .actorGeneration = std::nullopt,
      .message =
          CombatMonsterStateOutbound{
              .projection =
                  CombatProjection{
                      .battleId = BattleInstanceId{1},
                      .monsterId = 1,
                      .hitPoints = 1580,
                      .monsterState = MonsterState::Alive,
                      .outcome = CombatOutcome::None,
                      .terminal = std::nullopt,
                      .serverTick = 7,
                  },
              .participants =
                  std::vector<BattleParticipantProjection>{
                      {.sessionId = SessionId{1},
                       .generation = SessionGeneration{1},
                       .nickname = "player-1"},
                      {.sessionId = SessionId{2},
                       .generation = SessionGeneration{1},
                       .nickname = "player-2"},
                  },
          },
  };
  static_cast<void>(flow.handleCombatOutbound(std::move(stateIntent)));

  const auto snapshots = flow.takeUnreliableSnapshots();
  if (snapshots.size() != 1) {
    return false;
  }
  const auto decoded = RudpCombatCodec::decode(snapshots.front().datagram);
  return decoded.header.has_value() && decoded.header->sessionId == 2;
}

bool combatFlowReliableAdmissionIsExplicitAndRetryRecovers() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{workers, readiness,
                             [&intents](LobbyRoomOutboundIntent intent) {
                               intents.add(std::move(intent));
                             },
                             [](StateSnapshotProjection) {},
                             [&combat](CombatOutboundIntent intent) {
                               combat.add(std::move(intent));
                             }};
  RudpCombatFlow flow{bindings, gateway};

  if (!createCommittedRoom(gateway, workers) || intents.gameplayStarts() != 1) {
    return false;
  }
  feedCombat(flow, combat);

  // First accepted attack commandId=1 (retained result).
  const auto first = attackDatagram(1, *hostEpoch, 2, 1, 1);
  if (!first.has_value() ||
      flow.submitAttack(*first, endpoint(1), kStart) !=
          RudpCombatSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }
  feedCombat(flow, combat);

  // Fill the application reliable limit for session 1 with distinct results.
  std::uint64_t commandId = 1000;
  for (std::size_t i = 0; i < 300; ++i) {
    CombatOutboundIntent intent{
        .actorSessionId = SessionId{1},
        .actorGeneration = SessionGeneration{1},
        .message =
            CombatAttackResultOutbound{
                .result =
                    AttackTerminalResult{
                        .commandId = CommandId{.high = 0, .low = commandId++},
                        .battleId = BattleInstanceId{1},
                        .code = AttackResultCode::Ok,
                        .monsterId = 1,
                        .remainingHitPoints = 1580,
                        .rulesetVersion = 1,
                        .outcome = CombatOutcome::None,
                    }},
    };
    static_cast<void>(flow.handleCombatOutbound(std::move(intent)));
  }

  // Release capacity by discarding acknowledged transmissions for session 1.
  const auto now = std::chrono::steady_clock::now();
  auto polled = flow.pollReliable(now);
  if (std::ranges::none_of(
          polled.failures,
          [&](const auto &failure) {
            return failure.sessionId == SessionId{1} &&
                   failure.generation == SessionGeneration{1} &&
                   failure.transportEpoch == *hostEpoch &&
                   failure.sequence != 0 &&
                   failure.reason ==
                       RudpPeerFailureReason::ReliableAdmissionRejected;
          }) ||
      std::ranges::any_of(polled.failures, [](const auto &failure) {
        return failure.sessionId == SessionId{2};
      })) {
    return false;
  }
  std::optional<std::uint32_t> ackSequence;
  for (const auto &transmission : polled.transmissions) {
    const auto decoded = RudpCombatCodec::decode(transmission.datagram);
    if (decoded.header.has_value() && decoded.header->sessionId == 1) {
      ackSequence = decoded.header->sequence;
      break;
    }
  }
  if (!ackSequence.has_value() ||
      flow.discardAcknowledged(1, 1, *hostEpoch, *ackSequence, 0) == 0) {
    return false;
  }

  // Same-CommandId retry recovers the retained result after capacity is freed.
  const auto retry = attackDatagram(1, *hostEpoch, 3, 1, 1);
  if (!retry.has_value() ||
      flow.submitAttack(*retry, endpoint(1), kStart) !=
          RudpCombatSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }
  bool sawRetainedResult = false;
  for (auto intent : combat.take()) {
    const auto *result =
        std::get_if<CombatAttackResultOutbound>(&intent.message);
    if (result != nullptr && result->result.commandId.low == 1 &&
        result->result.remainingHitPoints == 1580) {
      sawRetainedResult = true;
    }
    static_cast<void>(flow.handleCombatOutbound(std::move(intent)));
  }
  if (!sawRetainedResult) {
    return false;
  }

  polled = flow.pollReliable(std::chrono::steady_clock::now());
  bool sawRecoveredResult = false;
  for (const auto &transmission : polled.transmissions) {
    const auto decoded = RudpCombatCodec::decode(transmission.datagram);
    const auto *result =
        decoded.message.has_value()
            ? std::get_if<RudpAttackTerminalResult>(&*decoded.message)
            : nullptr;
    if (result != nullptr && result->commandId.low == 1 &&
        result->remainingHitPoints == 1580) {
      sawRecoveredResult = true;
    }
  }
  return sawRecoveredResult;
}

bool combatFlowTimeoutProjectionEncodesTimedOut() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{workers, readiness,
                             [&intents](LobbyRoomOutboundIntent intent) {
                               intents.add(std::move(intent));
                             },
                             [](StateSnapshotProjection) {},
                             [&combat](CombatOutboundIntent intent) {
                               combat.add(std::move(intent));
                             }};
  RudpCombatFlow flow{bindings, gateway};

  if (!createCommittedRoom(gateway, workers) || intents.gameplayStarts() != 1) {
    return false;
  }
  feedCombat(flow, combat);

  const CombatProjection timedOut{
      .battleId = BattleInstanceId{1},
      .monsterId = 1,
      .hitPoints = 1600,
      .monsterState = MonsterState::TimedOut,
      .outcome = CombatOutcome::CombatTimeout,
      .terminal =
          CombatTerminalRecord{
              .eventId = EventId{.high = 1, .low = 2},
              .battleId = BattleInstanceId{1},
              .eventSequence = 2,
              .outcome = CombatOutcome::CombatTimeout,
              .monsterId = 1,
              .serverTick = 7,
              .rulesetVersion = 1,
          },
      .serverTick = 7,
  };
  CombatOutboundIntent stateIntent{
      .actorSessionId = std::nullopt,
      .actorGeneration = std::nullopt,
      .message =
          CombatMonsterStateOutbound{
              .projection = timedOut,
              .participants =
                  std::vector<BattleParticipantProjection>{
                      {.sessionId = SessionId{1},
                       .generation = SessionGeneration{1},
                       .nickname = "player-1"},
                      {.sessionId = SessionId{2},
                       .generation = SessionGeneration{1},
                       .nickname = "player-2"},
                  },
          },
  };
  static_cast<void>(flow.handleCombatOutbound(std::move(stateIntent)));

  const auto snapshots = flow.takeUnreliableSnapshots();
  if (snapshots.size() != 2) {
    return false;
  }
  for (const auto &snapshot : snapshots) {
    const auto decoded = RudpCombatCodec::decode(snapshot.datagram);
    const auto *state =
        decoded.message.has_value()
            ? std::get_if<RudpMonsterStateSnapshot>(&*decoded.message)
            : nullptr;
    if (state == nullptr || state->monsterState != RudpMonsterState::TimedOut ||
        state->monsterState == RudpMonsterState::Dead ||
        state->serverTick != 7) {
      return false;
    }
  }

  CombatOutboundIntent terminalIntent{
      .actorSessionId = std::nullopt,
      .actorGeneration = std::nullopt,
      .message =
          CombatTerminalEventOutbound{
              .terminal = *timedOut.terminal,
              .participants =
                  std::vector<BattleParticipantProjection>{
                      {.sessionId = SessionId{1},
                       .generation = SessionGeneration{1},
                       .nickname = "player-1"},
                      {.sessionId = SessionId{2},
                       .generation = SessionGeneration{1},
                       .nickname = "player-2"},
                  },
          },
  };
  static_cast<void>(flow.handleCombatOutbound(std::move(terminalIntent)));

  const auto polled = flow.pollReliable(std::chrono::steady_clock::now());
  std::size_t timeoutEvents = 0;
  for (const auto &transmission : polled.transmissions) {
    const auto decoded = RudpCombatCodec::decode(transmission.datagram);
    const auto *terminal =
        decoded.message.has_value()
            ? std::get_if<RudpCombatTerminalEvent>(&*decoded.message)
            : nullptr;
    if (terminal != nullptr) {
      if (terminal->combatOutcome == RudpCombatOutcome::MonsterDefeated ||
          terminal->combatOutcome != RudpCombatOutcome::CombatTimeout) {
        return false;
      }
      ++timeoutEvents;
    }
  }
  return timeoutEvents == 2;
}

// Finding 1 evidence: TransportEpoch/generation rebinds, ACK completion, and
// reliable expiry each return reliable-state cardinality to a bounded value,
// while an active identity keeps retransmitting until ACK or expiry.
bool combatFlowReliableStateCountsBoundByLifecycle() {
  // Rebind storm: old TransportEpoch/generation identities are retired as soon
  // as a newer binding produces outbound traffic for the same session.
  {
    RudpBindingRegistry bindings;
    RudpGameplayReadiness readiness{bindings};
    const auto hostEpoch = bind(bindings, 1);
    const auto memberEpoch = bind(bindings, 2);
    if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
      return false;
    }

    WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
    IntentCollector intents;
    CombatIntentCollector combat;
    RoomCommandGateway gateway{workers, readiness,
                               [&intents](LobbyRoomOutboundIntent intent) {
                                 intents.add(std::move(intent));
                               },
                               [](StateSnapshotProjection) {},
                               [&combat](CombatOutboundIntent intent) {
                                 combat.add(std::move(intent));
                               }};
    RudpCombatFlow flow{bindings, gateway};

    if (!createCommittedRoom(gateway, workers) ||
        intents.gameplayStarts() != 1) {
      return false;
    }
    feedCombat(flow, combat);
    if (flow.reliableStateCount() != 2) {
      return false;
    }

    std::uint64_t commandId = 1;
    for (std::uint64_t generation = 2; generation <= 6; ++generation) {
      const auto reboundEpoch = bindGeneration(bindings, 1, generation);
      if (!reboundEpoch.has_value() || *reboundEpoch == *hostEpoch) {
        return false;
      }
      CombatOutboundIntent intent{
          .actorSessionId = SessionId{1},
          .actorGeneration = SessionGeneration{generation},
          .message =
              CombatAttackResultOutbound{
                  .result =
                      AttackTerminalResult{
                          .commandId = CommandId{.high = 0, .low = commandId++},
                          .battleId = BattleInstanceId{1},
                          .code = AttackResultCode::Ok,
                          .monsterId = 1,
                          .remainingHitPoints = 1580,
                          .rulesetVersion = 1,
                          .outcome = CombatOutcome::None,
                      }},
      };
      static_cast<void>(flow.handleCombatOutbound(std::move(intent)));
      if (flow.reliableStateCount() != 2) {
        return false;
      }
    }
    // The original generation-1 identity must be gone after the rebinds.
    if (flow.discardAcknowledged(1, 1, *hostEpoch, 0, 0) != 0) {
      return false;
    }
  }

  // ACK completion drains the queue and retires the identity to baseline.
  {
    RudpBindingRegistry bindings;
    RudpGameplayReadiness readiness{bindings};
    const auto hostEpoch = bind(bindings, 1);
    const auto memberEpoch = bind(bindings, 2);
    if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
      return false;
    }

    WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
    IntentCollector intents;
    CombatIntentCollector combat;
    RoomCommandGateway gateway{workers, readiness,
                               [&intents](LobbyRoomOutboundIntent intent) {
                                 intents.add(std::move(intent));
                               },
                               [](StateSnapshotProjection) {},
                               [&combat](CombatOutboundIntent intent) {
                                 combat.add(std::move(intent));
                               }};
    RudpCombatFlow flow{bindings, gateway};

    if (!createCommittedRoom(gateway, workers) ||
        intents.gameplayStarts() != 1) {
      return false;
    }
    feedCombat(flow, combat);
    if (flow.reliableStateCount() != 2) {
      return false;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto polled = flow.pollReliable(now);
    for (const auto &transmission : polled.transmissions) {
      const auto decoded = RudpCombatCodec::decode(transmission.datagram);
      if (!decoded.header.has_value()) {
        return false;
      }
      static_cast<void>(flow.discardAcknowledged(
          decoded.header->sessionId, decoded.header->sessionGeneration,
          decoded.header->transportEpoch, decoded.header->sequence, 0));
    }
    if (flow.reliableStateCount() != 0) {
      return false;
    }
  }

  // Reliable expiry retires the identity to baseline.
  {
    RudpBindingRegistry bindings;
    RudpGameplayReadiness readiness{bindings};
    const auto hostEpoch = bind(bindings, 1);
    const auto memberEpoch = bind(bindings, 2);
    if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
      return false;
    }

    WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
    IntentCollector intents;
    CombatIntentCollector combat;
    RoomCommandGateway gateway{workers, readiness,
                               [&intents](LobbyRoomOutboundIntent intent) {
                                 intents.add(std::move(intent));
                               },
                               [](StateSnapshotProjection) {},
                               [&combat](CombatOutboundIntent intent) {
                                 combat.add(std::move(intent));
                               }};
    RudpCombatFlow flow{bindings, gateway};

    if (!createCommittedRoom(gateway, workers) ||
        intents.gameplayStarts() != 1) {
      return false;
    }
    feedCombat(flow, combat);
    if (flow.reliableStateCount() != 2) {
      return false;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto polled = flow.pollReliable(now + 5000ms);
    if (polled.failures.size() != 2 ||
        std::ranges::none_of(polled.failures,
                             [&](const auto &failure) {
                               return matchesFailure(
                                   failure, 1, 1, *hostEpoch, 2,
                                   RudpPeerFailureReason::ReliableExpired);
                             }) ||
        std::ranges::none_of(polled.failures, [&](const auto &failure) {
          return matchesFailure(failure, 2, 1, *memberEpoch, 2,
                                RudpPeerFailureReason::ReliableExpired);
        })) {
      return false;
    }
    if (flow.reliableStateCount() != 0) {
      return false;
    }
  }

  // An active identity is retained and retransmitted until ACK or expiry.
  {
    RudpBindingRegistry bindings;
    RudpGameplayReadiness readiness{bindings};
    const auto hostEpoch = bind(bindings, 1);
    const auto memberEpoch = bind(bindings, 2);
    if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
      return false;
    }

    WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
    IntentCollector intents;
    CombatIntentCollector combat;
    RoomCommandGateway gateway{workers, readiness,
                               [&intents](LobbyRoomOutboundIntent intent) {
                                 intents.add(std::move(intent));
                               },
                               [](StateSnapshotProjection) {},
                               [&combat](CombatOutboundIntent intent) {
                                 combat.add(std::move(intent));
                               }};
    RudpCombatFlow flow{bindings, gateway};

    if (!createCommittedRoom(gateway, workers) ||
        intents.gameplayStarts() != 1) {
      return false;
    }
    feedCombat(flow, combat);
    if (flow.reliableStateCount() != 2) {
      return false;
    }
    const auto now = std::chrono::steady_clock::now();
    auto polled = flow.pollReliable(now);
    std::size_t spawnTransmissions = 0;
    for (const auto &transmission : polled.transmissions) {
      const auto decoded = RudpCombatCodec::decode(transmission.datagram);
      if (decoded.message.has_value() &&
          std::get_if<RudpMonsterSpawned>(&*decoded.message) != nullptr) {
        ++spawnTransmissions;
      }
    }
    if (spawnTransmissions != 2 || flow.reliableStateCount() != 2) {
      return false;
    }
    polled = flow.pollReliable(now + 250ms);
    if (polled.transmissions.empty() || flow.reliableStateCount() != 2) {
      return false;
    }
  }
  return true;
}

// Finding 1 evidence: repeated battle lifecycles do not retain old snapshot
// sequence identities; encoding a terminal projection retires the battle.
bool combatFlowSnapshotSequenceCountsBoundByLifecycle() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{workers, readiness,
                             [&intents](LobbyRoomOutboundIntent intent) {
                               intents.add(std::move(intent));
                             },
                             [](StateSnapshotProjection) {},
                             [&combat](CombatOutboundIntent intent) {
                               combat.add(std::move(intent));
                             }};
  RudpCombatFlow flow{bindings, gateway};

  if (!createCommittedRoom(gateway, workers) || intents.gameplayStarts() != 1) {
    return false;
  }
  feedCombat(flow, combat);

  const std::vector<BattleParticipantProjection> participants{
      {.sessionId = SessionId{1},
       .generation = SessionGeneration{1},
       .nickname = "player-1"},
      {.sessionId = SessionId{2},
       .generation = SessionGeneration{1},
       .nickname = "player-2"},
  };
  const auto stateIntent = [&participants](std::uint64_t battleId,
                                           bool terminal) {
    return CombatOutboundIntent{
        .actorSessionId = std::nullopt,
        .actorGeneration = std::nullopt,
        .message =
            CombatMonsterStateOutbound{
                .projection =
                    CombatProjection{
                        .battleId = BattleInstanceId{battleId},
                        .monsterId = 1,
                        .hitPoints =
                            terminal ? std::uint32_t{0} : std::uint32_t{1580},
                        .monsterState =
                            terminal ? MonsterState::Dead : MonsterState::Alive,
                        .outcome = terminal ? CombatOutcome::MonsterDefeated
                                            : CombatOutcome::None,
                        .terminal =
                            terminal
                                ? std::optional<
                                      CombatTerminalRecord>{CombatTerminalRecord{
                                      .eventId =
                                          EventId{.high = battleId, .low = 2},
                                      .battleId = BattleInstanceId{battleId},
                                      .eventSequence = 2,
                                      .outcome = CombatOutcome::MonsterDefeated,
                                      .monsterId = 1,
                                      .serverTick = 1,
                                      .rulesetVersion = 1,
                                  }}
                                : std::nullopt,
                        .serverTick = 1,
                    },
                .participants = participants,
            },
    };
  };

  for (std::uint64_t battleId = 1; battleId <= 8; ++battleId) {
    static_cast<void>(flow.handleCombatOutbound(stateIntent(battleId, false)));
    if (flow.snapshotSequenceCount() != 1) {
      return false;
    }
    static_cast<void>(flow.handleCombatOutbound(stateIntent(battleId, true)));
    if (flow.snapshotSequenceCount() != 0) {
      return false;
    }
  }
  return true;
}

// Finding 2 evidence: the real combat deadline control command traverses the
// Room single-writer path, emits exactly one CombatTimeout terminal outcome,
// and the actual timeout projection becomes an encoded RUDP snapshot with no
// fabricated Dead/MonsterDefeated value.
bool combatFlowDeadlineDrivesRealTimeoutPath() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  ManualDeadlineScheduler deadlines;
  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{
      workers,
      readiness,
      [&intents](LobbyRoomOutboundIntent intent) {
        intents.add(std::move(intent));
      },
      [](StateSnapshotProjection) {},
      [&combat](CombatOutboundIntent intent) { combat.add(std::move(intent)); },
      deadlines};
  RudpCombatFlow flow{bindings, gateway};

  if (!createCommittedRoom(gateway, workers) || intents.gameplayStarts() != 1) {
    return false;
  }
  feedCombat(flow, combat);
  if (!combat.take().empty()) {
    return false;
  }

  // Fire the real 30000 ms product deadline deterministically through the
  // Room single-writer path (scheduler -> cell control queue -> gateway).
  deadlines.advance(30s);
  if (!workers.waitUntilIdle(2s)) {
    return false;
  }

  std::size_t timeoutTerminalIntentCount = 0;
  for (auto intent : combat.take()) {
    if (const auto *terminal =
            std::get_if<CombatTerminalEventOutbound>(&intent.message);
        terminal != nullptr) {
      if (terminal->terminal.outcome != CombatOutcome::CombatTimeout) {
        return false;
      }
      ++timeoutTerminalIntentCount;
    }
    if (const auto *state =
            std::get_if<CombatMonsterStateOutbound>(&intent.message);
        state != nullptr) {
      if (state->projection.monsterState == MonsterState::Dead ||
          state->projection.outcome == CombatOutcome::MonsterDefeated) {
        return false;
      }
    }
    static_cast<void>(flow.handleCombatOutbound(std::move(intent)));
  }
  if (timeoutTerminalIntentCount != 1) {
    return false;
  }

  // The actual timeout projection becomes an encoded RUDP snapshot for every
  // participant: TimedOut, never Dead, and no fabricated MonsterDefeated.
  const auto snapshots = flow.takeUnreliableSnapshots();
  if (snapshots.size() != 2) {
    return false;
  }
  for (const auto &snapshot : snapshots) {
    const auto decoded = RudpCombatCodec::decode(snapshot.datagram);
    const auto *state =
        decoded.message.has_value()
            ? std::get_if<RudpMonsterStateSnapshot>(&*decoded.message)
            : nullptr;
    if (state == nullptr || state->monsterState != RudpMonsterState::TimedOut ||
        state->monsterState == RudpMonsterState::Dead ||
        state->hitPoints != 1600) {
      return false;
    }
  }

  // Exactly one CombatTimeout terminal outcome is encoded reliably to each
  // participant (2 transmissions of the same terminal identity).
  const auto polled = flow.pollReliable(std::chrono::steady_clock::now());
  std::size_t timeoutEvents = 0;
  for (const auto &transmission : polled.transmissions) {
    const auto decoded = RudpCombatCodec::decode(transmission.datagram);
    const auto *terminal =
        decoded.message.has_value()
            ? std::get_if<RudpCombatTerminalEvent>(&*decoded.message)
            : nullptr;
    if (terminal != nullptr) {
      if (terminal->combatOutcome != RudpCombatOutcome::CombatTimeout ||
          terminal->combatOutcome == RudpCombatOutcome::MonsterDefeated) {
        return false;
      }
      ++timeoutEvents;
    }
  }
  return timeoutEvents == 2;
}

// Recovery A: after a real enqueue, an actual RudpBindingRegistry::invalidate
// removes the old-epoch reliable state before any further poll. The next poll
// therefore has zero old-epoch transmissions and the map returns to baseline.
bool combatFlowInvalidateDropsOldEpochTransmission() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{workers, readiness,
                             [&intents](LobbyRoomOutboundIntent intent) {
                               intents.add(std::move(intent));
                             },
                             [](StateSnapshotProjection) {},
                             [&combat](CombatOutboundIntent intent) {
                               combat.add(std::move(intent));
                             }};
  RudpCombatFlow flow{bindings, gateway};

  if (!createCommittedRoom(gateway, workers) || intents.gameplayStarts() != 1) {
    return false;
  }
  feedCombat(flow, combat);
  if (flow.reliableStateCount() != 2) {
    return false;
  }

  // Actual transport invalidation of the host identity.
  if (!bindings.invalidate(1, 1)) {
    return false;
  }
  const auto polled = flow.pollReliable(std::chrono::steady_clock::now());
  for (const auto &transmission : polled.transmissions) {
    const auto decoded = RudpCombatCodec::decode(transmission.datagram);
    if (decoded.header.has_value() && decoded.header->sessionId == 1) {
      return false;
    }
  }
  return flow.reliableStateCount() == 1;
}

// Recovery A: actual SessionRegistry + AuthClaimCoordinator authentication and
// close, composed with RoomCommandGateway::disconnect, removes the invalidated
// session's reliable state before the next poll.
bool combatFlowSessionAuthCloseRemovesReliableState() {
  SessionRegistry sessions;
  RudpBindingRegistry bindings;
  AuthClaimCoordinator coordinator{sessions, bindings};
  RudpGameplayReadiness readiness{bindings};

  if (!coordinator.beginClaim(101, RequestId{1})) {
    return false;
  }
  ClaimedIdentity identity{.accountId = {}, .nickname = "player-1"};
  identity.accountId.back() = 1;
  const auto applied = coordinator.apply(
      ClaimCompletion{101, 1, ClaimOutcome::Claimed, identity});
  if (applied.kind != AppliedClaimKind::Accepted ||
      !applied.authenticated.has_value()) {
    return false;
  }
  const auto authenticated = *applied.authenticated;
  if (authenticated.sessionId != SessionId{1} ||
      authenticated.generation != SessionGeneration{1}) {
    return false;
  }
  const auto capability =
      coordinator.issueRudpBindCapability(101, RequestId{2}, kStart);
  if (!capability.has_value()) {
    return false;
  }
  const auto hostBind =
      bindings.bind(RudpHeader{.flag = RudpFlag::Reliable,
                               .sessionId = 1,
                               .sessionGeneration = 1,
                               .transportEpoch = 0,
                               .sequence = 1,
                               .ack = 0,
                               .ackBits = 0,
                               .messageId = 22},
                    RudpBindHello{*capability}, endpoint(1), kStart);
  if (hostBind.status != RudpBindStatus::Accepted) {
    return false;
  }
  const auto memberEpoch = bind(bindings, 2);
  if (!memberEpoch.has_value()) {
    return false;
  }

  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{workers, readiness,
                             [&intents](LobbyRoomOutboundIntent intent) {
                               intents.add(std::move(intent));
                             },
                             [](StateSnapshotProjection) {},
                             [&combat](CombatOutboundIntent intent) {
                               combat.add(std::move(intent));
                             }};
  RudpCombatFlow flow{bindings, gateway};

  if (!createCommittedRoom(gateway, workers) || intents.gameplayStarts() != 1) {
    return false;
  }
  feedCombat(flow, combat);
  if (flow.reliableStateCount() != 2) {
    return false;
  }

  // Close the authenticated connection (invalidates the RUDP binding and
  // disconnects the session) and compose the room gateway disconnect.
  if (!coordinator.closeConnection(101)) {
    return false;
  }
  if (!gateway.disconnect(SessionId{1}, SessionGeneration{1})) {
    return false;
  }
  if (!workers.waitUntilIdle(2s)) {
    return false;
  }
  const auto polled = flow.pollReliable(std::chrono::steady_clock::now());
  for (const auto &transmission : polled.transmissions) {
    const auto decoded = RudpCombatCodec::decode(transmission.datagram);
    if (decoded.header.has_value() && decoded.header->sessionId == 1) {
      return false;
    }
  }
  return flow.reliableStateCount() == 1;
}

bool rudpPeerFailureClosesCurrentOnceAndCannotCloseReplacement() {
  SessionRegistry sessions;
  RudpBindingRegistry bindings;
  AuthClaimCoordinator coordinator{sessions, bindings};

  const auto authenticate = [&](std::uint64_t connectionEpoch,
                                std::uint64_t requestId,
                                std::uint8_t accountSuffix) {
    if (!coordinator.beginClaim(connectionEpoch, RequestId{requestId})) {
      return std::optional<lol::session::AuthenticateSessionResult>{};
    }
    ClaimedIdentity identity{.accountId = {}, .nickname = "player"};
    identity.accountId.back() = accountSuffix;
    const auto applied = coordinator.apply(ClaimCompletion{
        connectionEpoch, requestId, ClaimOutcome::Claimed, identity});
    return applied.kind == AppliedClaimKind::Accepted
               ? applied.authenticated
               : std::optional<lol::session::AuthenticateSessionResult>{};
  };
  const auto bindAuthenticated =
      [&](std::uint64_t connectionEpoch,
          const lol::session::AuthenticateSessionResult &authenticated,
          std::uint8_t endpointSuffix) {
        const auto capability = coordinator.issueRudpBindCapability(
            connectionEpoch, RequestId{connectionEpoch}, kStart);
        if (!capability.has_value()) {
          return std::optional<std::uint32_t>{};
        }
        const auto result = bindings.bind(
            RudpHeader{.flag = RudpFlag::Reliable,
                       .sessionId = authenticated.sessionId.value(),
                       .sessionGeneration = authenticated.generation.value(),
                       .transportEpoch = 0,
                       .sequence = 1,
                       .ack = 0,
                       .ackBits = 0,
                       .messageId = 22},
            RudpBindHello{*capability}, endpoint(endpointSuffix), kStart);
        return result.status == RudpBindStatus::Accepted
                   ? std::optional{result.transportEpoch}
                   : std::optional<std::uint32_t>{};
      };

  const auto oldHost = authenticate(101, 1, 1);
  const auto member = authenticate(102, 2, 2);
  if (!oldHost.has_value() || !member.has_value()) {
    return false;
  }
  const auto oldHostEpoch = bindAuthenticated(101, *oldHost, 1);
  const auto memberEpoch = bindAuthenticated(102, *member, 2);
  if (!oldHostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  const RudpPeerFailure staleFailure{
      .sessionId = oldHost->sessionId,
      .generation = oldHost->generation,
      .transportEpoch = *oldHostEpoch,
      .sequence = 9,
      .reason = RudpPeerFailureReason::ReliableExpired,
      .endpoint = endpoint(1),
  };
  const auto replacement = authenticate(103, 3, 1);
  if (!replacement.has_value()) {
    return false;
  }
  const auto replacementEpoch = bindAuthenticated(103, *replacement, 3);
  if (!replacementEpoch.has_value() ||
      coordinator.closeRudpPeer(staleFailure).has_value() ||
      sessions.activeSessionCount() != 2 ||
      !bindings.isBound(replacement->sessionId.value(),
                        replacement->generation.value()) ||
      !bindings.isBound(member->sessionId.value(),
                        member->generation.value())) {
    return false;
  }

  const RudpPeerFailure currentFailure{
      .sessionId = replacement->sessionId,
      .generation = replacement->generation,
      .transportEpoch = *replacementEpoch,
      .sequence = 10,
      .reason = RudpPeerFailureReason::ReliableAdmissionRejected,
      .endpoint = endpoint(3),
  };
  const auto closed = coordinator.closeRudpPeer(currentFailure);
  return closed.has_value() && closed->connectionEpoch == 103 &&
         closed->sessionId == replacement->sessionId &&
         closed->generation == replacement->generation &&
         !coordinator.closeRudpPeer(currentFailure).has_value() &&
         sessions.activeSessionCount() == 1 &&
         !bindings.isBound(replacement->sessionId.value(),
                           replacement->generation.value()) &&
         bindings.isBound(member->sessionId.value(),
                          member->generation.value());
}

// Recovery A: an accepted same-generation TransportEpoch rebind, with no new
// combat outbound delivered, drops the old-epoch reliable state on the next
// poll. The rebind replaces the active binding under the SAME
// SessionGeneration, so the old reliable identity is stale by epoch, not by
// generation.
bool combatFlowRebindDropsOldEpochWithoutNewOutbound() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{workers, readiness,
                             [&intents](LobbyRoomOutboundIntent intent) {
                               intents.add(std::move(intent));
                             },
                             [](StateSnapshotProjection) {},
                             [&combat](CombatOutboundIntent intent) {
                               combat.add(std::move(intent));
                             }};
  RudpCombatFlow flow{bindings, gateway};

  if (!createCommittedRoom(gateway, workers) || intents.gameplayStarts() != 1) {
    return false;
  }
  feedCombat(flow, combat);
  if (flow.reliableStateCount() != 2) {
    return false;
  }

  // Rebind session 1 to the SAME active SessionGeneration 1 through the real
  // requestCapability + bind path. The accepted binding carries a new nonzero
  // TransportEpoch while the captured battle still refers to generation 1.
  const auto reboundEpoch = bindGeneration(bindings, 1, 1);
  if (!reboundEpoch.has_value() || *reboundEpoch == *hostEpoch) {
    return false;
  }

  // No new combat outbound is delivered; the next poll must drop the old
  // hostEpoch reliable identity (StaleEpoch) without transmitting old-epoch
  // spawn data. The member's active generation-1 state is allowed.
  const auto polled = flow.pollReliable(std::chrono::steady_clock::now());
  for (const auto &transmission : polled.transmissions) {
    const auto decoded = RudpCombatCodec::decode(transmission.datagram);
    if (decoded.header.has_value() && decoded.header->sessionId == 1 &&
        decoded.header->sessionGeneration == 1 &&
        decoded.header->transportEpoch == *hostEpoch) {
      return false;
    }
  }
  return flow.reliableStateCount() == 1;
}

// A committed room whose last active participant leaves has an immutable
// cancellation source. The flow retains the Battle/Room identity until a future
// durability completion while retiring its now-recipientless transport state.
bool combatFlowCancellationHoldsSnapshotUntilDurability() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{workers, readiness,
                             [&intents](LobbyRoomOutboundIntent intent) {
                               intents.add(std::move(intent));
                             },
                             [](StateSnapshotProjection) {},
                             [&combat](CombatOutboundIntent intent) {
                               combat.add(std::move(intent));
                             }};
  RudpCombatFlow flow{bindings, gateway};

  if (!createCommittedRoom(gateway, workers) || intents.gameplayStarts() != 1) {
    return false;
  }
  feedCombat(flow, combat);

  // Real non-terminal attack snapshot for the committed battle.
  const auto attack = attackDatagram(1, *hostEpoch, 2, 1, 1);
  if (!attack.has_value() ||
      flow.submitAttack(*attack, endpoint(1), kStart) !=
          RudpCombatSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }
  feedCombat(flow, combat);
  if (flow.snapshotSequenceCount() != 1) {
    return false;
  }

  // First Leave keeps one active participant. Clear prior lobby output so the
  // last-Leave boundary can be checked in isolation.
  if (gateway.submit(RoomCommandEnvelope{
          .session = session(1),
          .command = LeaveRoomRequest{.requestId = RequestId{8}}}) !=
          RoomSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }
  static_cast<void>(intents.take());

  // Last Leave commits cancellation and must not emit an OPEN projection.
  if (gateway.submit(RoomCommandEnvelope{
          .session = session(2),
          .command = LeaveRoomRequest{.requestId = RequestId{9}}}) !=
          RoomSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }

  const auto cancellationOutput = intents.take();
  const bool emittedOpen = std::any_of(
      cancellationOutput.begin(), cancellationOutput.end(),
      [](const LobbyRoomOutboundIntent &intent) {
        const auto *detail =
            std::get_if<lol::lobby_room::RoomDetailProjection>(&intent.message);
        return detail != nullptr &&
               detail->lifecycle == lol::lobby_room::RoomLifecycle::Open;
      });

  bool sawRetired = false;
  for (auto intent : combat.take()) {
    if (std::holds_alternative<CombatBattleRetiredOutbound>(intent.message)) {
      sawRetired = true;
    }
    static_cast<void>(flow.handleCombatOutbound(std::move(intent)));
  }
  if (!sawRetired || emittedOpen || flow.snapshotSequenceCount() != 0 ||
      !gateway.enterLobby(session(3))) {
    return false;
  }
  const auto lobbyOutput = intents.take();
  const bool hiddenFromLobby = std::any_of(
      lobbyOutput.begin(), lobbyOutput.end(),
      [](const LobbyRoomOutboundIntent &intent) {
        const auto *snapshot =
            std::get_if<lol::game_flow::LobbyEntrySnapshot>(&intent.message);
        return snapshot != nullptr && snapshot->rooms.empty();
      });
  if (!hiddenFromLobby ||
      gateway.submit(RoomCommandEnvelope{
          .session = session(3),
          .command = JoinRoomRequest{.requestId = RequestId{10},
                                     .roomId = RoomId{1}}}) !=
          RoomSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }
  const auto joinOutput = intents.take();
  return std::any_of(
      joinOutput.begin(), joinOutput.end(),
      [](const LobbyRoomOutboundIntent &intent) {
        const auto *response =
            std::get_if<lol::game_flow::RoomCommandResponse>(&intent.message);
        return response != nullptr && response->requestId == RequestId{10} &&
               response->result == lol::lobby_room::RoomResultCode::RoomClosed;
      });
}

// Rooms waiting for durability retain their Battle state while each
// recipientless transport projection drains before a later battle reuses its
// battle-local identity.
bool combatFlowAwaitingRoomsRetireTransportSnapshotsIndependently() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{workers, readiness,
                             [&intents](LobbyRoomOutboundIntent intent) {
                               intents.add(std::move(intent));
                             },
                             [](StateSnapshotProjection) {},
                             [&combat](CombatOutboundIntent intent) {
                               combat.add(std::move(intent));
                             }};
  RudpCombatFlow flow{bindings, gateway};

  for (std::uint64_t round = 0; round < 3; ++round) {
    const auto hostEpoch = bind(bindings, 1);
    const auto memberEpoch = bind(bindings, 2);
    if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
      return false;
    }
    const std::uint64_t roomId = round + 1;
    const std::uint64_t requestBase = round * 100;
    if (!createCommittedRoomAt(gateway, workers, roomId, requestBase) ||
        intents.gameplayStarts() != round + 1) {
      return false;
    }
    feedCombat(flow, combat);
    if (flow.reliableStateCount() != 2 || flow.snapshotSequenceCount() != 0) {
      return false;
    }

    const auto attack = attackDatagram(1, *hostEpoch, 2, 1, 1);
    if (!attack.has_value() ||
        flow.submitAttack(*attack, endpoint(1), kStart) !=
            RudpCombatSubmitResult::Accepted ||
        !workers.waitUntilIdle(2s)) {
      return false;
    }
    feedCombat(flow, combat);
    if (flow.snapshotSequenceCount() != 1) {
      return false;
    }

    // Actual transport invalidation of both identities, then last Leave.
    if (!bindings.invalidate(1, 1) || !bindings.invalidate(2, 1)) {
      return false;
    }
    if (gateway.submit(RoomCommandEnvelope{
            .session = session(1),
            .command =
                LeaveRoomRequest{.requestId = RequestId{requestBase + 8}}}) !=
            RoomSubmitResult::Accepted ||
        !workers.waitUntilIdle(2s) ||
        gateway.submit(RoomCommandEnvelope{
            .session = session(2),
            .command =
                LeaveRoomRequest{.requestId = RequestId{requestBase + 9}}}) !=
            RoomSubmitResult::Accepted ||
        !workers.waitUntilIdle(2s)) {
      return false;
    }
    bool sawRetired = false;
    for (auto intent : combat.take()) {
      if (std::holds_alternative<CombatBattleRetiredOutbound>(intent.message)) {
        sawRetired = true;
      }
      static_cast<void>(flow.handleCombatOutbound(std::move(intent)));
    }
    if (!sawRetired || flow.snapshotSequenceCount() != 0) {
      return false;
    }
    const auto polled = flow.pollReliable(std::chrono::steady_clock::now());
    if (flow.reliableStateCount() != 0) {
      return false;
    }
    for (const auto &transmission : polled.transmissions) {
      const auto decoded = RudpCombatCodec::decode(transmission.datagram);
      if (decoded.header.has_value() &&
          (decoded.header->sessionId == 1 || decoded.header->sessionId == 2)) {
        return false;
      }
    }
  }
  return true;
}

// Recovery A: an active identity keeps reliable replay on the initial poll and
// a retry poll, and drains to baseline after the transport ACK.
bool combatFlowActiveIdentityReplaysUntilAck() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{workers, readiness,
                             [&intents](LobbyRoomOutboundIntent intent) {
                               intents.add(std::move(intent));
                             },
                             [](StateSnapshotProjection) {},
                             [&combat](CombatOutboundIntent intent) {
                               combat.add(std::move(intent));
                             }};
  RudpCombatFlow flow{bindings, gateway};

  if (!createCommittedRoom(gateway, workers) || intents.gameplayStarts() != 1) {
    return false;
  }
  feedCombat(flow, combat);
  if (flow.reliableStateCount() != 2) {
    return false;
  }

  const auto now = std::chrono::steady_clock::now();
  auto polled = flow.pollReliable(now);
  if (polled.transmissions.size() != 2 || flow.reliableStateCount() != 2) {
    return false;
  }
  polled = flow.pollReliable(now + 250ms);
  if (polled.transmissions.empty() || flow.reliableStateCount() != 2) {
    return false;
  }
  for (const auto &transmission : polled.transmissions) {
    const auto decoded = RudpCombatCodec::decode(transmission.datagram);
    if (!decoded.header.has_value()) {
      return false;
    }
    static_cast<void>(flow.discardAcknowledged(
        decoded.header->sessionId, decoded.header->sessionGeneration,
        decoded.header->transportEpoch, decoded.header->sequence, 0));
  }
  return flow.reliableStateCount() == 0;
}

// The pending deadline remains safe after cancellation because the Cell is
// retained at the durability boundary and the stale combat deadline is an
// explicit no-op.
bool combatFlowDeadlineAfterTeardownIsSafe() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  ManualDeadlineScheduler deadlines;
  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{
      workers,
      readiness,
      [&intents](LobbyRoomOutboundIntent intent) {
        intents.add(std::move(intent));
      },
      [](StateSnapshotProjection) {},
      [&combat](CombatOutboundIntent intent) { combat.add(std::move(intent)); },
      deadlines};
  RudpCombatFlow flow{bindings, gateway};

  if (!createCommittedRoom(gateway, workers) || intents.gameplayStarts() != 1) {
    return false;
  }
  feedCombat(flow, combat);

  // Last Leave commits cancellation while the 30s deadline is still pending.
  if (gateway.submit(RoomCommandEnvelope{
          .session = session(1),
          .command = LeaveRoomRequest{.requestId = RequestId{8}}}) !=
          RoomSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s) ||
      gateway.submit(RoomCommandEnvelope{
          .session = session(2),
          .command = LeaveRoomRequest{.requestId = RequestId{9}}}) !=
          RoomSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }
  std::size_t retiredCount = 0;
  for (auto intent : combat.take()) {
    if (std::holds_alternative<CombatBattleRetiredOutbound>(intent.message)) {
      ++retiredCount;
    }
    if (std::holds_alternative<CombatTerminalEventOutbound>(intent.message) ||
        std::holds_alternative<CombatMonsterStateOutbound>(intent.message)) {
      return false;
    }
    static_cast<void>(flow.handleCombatOutbound(std::move(intent)));
  }
  if (retiredCount != 1) {
    return false;
  }

  // Fire the pending combat deadline callback while durability is absent.
  deadlines.advance(30s);
  if (!workers.waitUntilIdle(2s)) {
    return false;
  }
  const auto stale = combat.take();
  for (const auto &intent : stale) {
    if (std::holds_alternative<CombatTerminalEventOutbound>(intent.message) ||
        std::holds_alternative<CombatMonsterStateOutbound>(intent.message)) {
      return false;
    }
  }
  return stale.empty() && flow.snapshotSequenceCount() == 0;
}

// Actual ConfirmedDisconnect for the host and then the last member commits the
// same cancellation hold; firing the pending deadline produces no stale
// state/terminal output.
bool combatFlowDisconnectTeardownCancelsPendingDeadline() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  ManualDeadlineScheduler deadlines;
  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{
      workers,
      readiness,
      [&intents](LobbyRoomOutboundIntent intent) {
        intents.add(std::move(intent));
      },
      [](StateSnapshotProjection) {},
      [&combat](CombatOutboundIntent intent) { combat.add(std::move(intent)); },
      deadlines};
  RudpCombatFlow flow{bindings, gateway};

  if (!createCommittedRoom(gateway, workers) || intents.gameplayStarts() != 1) {
    return false;
  }
  feedCombat(flow, combat);
  if (!combat.take().empty()) {
    return false;
  }

  // Disconnect the host, then the last member, while the 30s deadline is still
  // pending. The last disconnect shares the cancellation boundary with Leave
  // and must not retire the Battle before durability.
  if (!gateway.disconnect(SessionId{1}, SessionGeneration{1}) ||
      !workers.waitUntilIdle(2s) ||
      !gateway.disconnect(SessionId{2}, SessionGeneration{1}) ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }
  std::size_t retiredCount = 0;
  for (auto intent : combat.take()) {
    if (std::holds_alternative<CombatBattleRetiredOutbound>(intent.message)) {
      ++retiredCount;
    }
    if (std::holds_alternative<CombatTerminalEventOutbound>(intent.message) ||
        std::holds_alternative<CombatMonsterStateOutbound>(intent.message)) {
      return false;
    }
    static_cast<void>(flow.handleCombatOutbound(std::move(intent)));
  }
  if (retiredCount != 1) {
    return false;
  }

  deadlines.advance(30s);
  if (!workers.waitUntilIdle(2s)) {
    return false;
  }
  const auto stale = combat.take();
  for (const auto &intent : stale) {
    if (std::holds_alternative<CombatTerminalEventOutbound>(intent.message) ||
        std::holds_alternative<CombatMonsterStateOutbound>(intent.message)) {
      return false;
    }
  }
  return stale.empty() && flow.snapshotSequenceCount() == 0;
}

// A normal result committed before participant exit remains the winner. Later
// exits do not rewrite it and do not retire the durability-held Room.
bool combatFlowPostTerminalExitStaysHeldWithoutNewOutput() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  ManualDeadlineScheduler deadlines;
  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{
      workers,
      readiness,
      [&intents](LobbyRoomOutboundIntent intent) {
        intents.add(std::move(intent));
      },
      [](StateSnapshotProjection) {},
      [&combat](CombatOutboundIntent intent) { combat.add(std::move(intent)); },
      deadlines};
  RudpCombatFlow flow{bindings, gateway};

  if (!createCommittedRoom(gateway, workers) || intents.gameplayStarts() != 1) {
    return false;
  }
  feedCombat(flow, combat);
  if (!combat.take().empty()) {
    return false;
  }

  // Real timeout while the room is active: exactly one terminal event and the
  // terminal projection retires the snapshot identity.
  deadlines.advance(30s);
  if (!workers.waitUntilIdle(2s)) {
    return false;
  }
  std::size_t timeoutTerminalCount = 0;
  for (auto intent : combat.take()) {
    if (const auto *terminal =
            std::get_if<CombatTerminalEventOutbound>(&intent.message);
        terminal != nullptr) {
      if (terminal->terminal.outcome != CombatOutcome::CombatTimeout) {
        return false;
      }
      ++timeoutTerminalCount;
    }
    static_cast<void>(flow.handleCombatOutbound(std::move(intent)));
  }
  if (timeoutTerminalCount != 1 || flow.snapshotSequenceCount() != 0) {
    return false;
  }

  // Post-terminal last Leave emits no new terminal/state and only retires the
  // now-recipientless transport projection.
  if (gateway.submit(RoomCommandEnvelope{
          .session = session(1),
          .command = LeaveRoomRequest{.requestId = RequestId{8}}}) !=
          RoomSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s) ||
      gateway.submit(RoomCommandEnvelope{
          .session = session(2),
          .command = LeaveRoomRequest{.requestId = RequestId{9}}}) !=
          RoomSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }
  std::size_t retiredCount = 0;
  for (auto intent : combat.take()) {
    if (std::holds_alternative<CombatBattleRetiredOutbound>(intent.message)) {
      ++retiredCount;
    }
    if (std::holds_alternative<CombatTerminalEventOutbound>(intent.message) ||
        std::holds_alternative<CombatMonsterStateOutbound>(intent.message)) {
      return false;
    }
    static_cast<void>(flow.handleCombatOutbound(std::move(intent)));
  }
  return retiredCount == 1 && flow.snapshotSequenceCount() == 0;
}

bool lootFlowUsesRealGatewayAndServerProjection() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value()) {
    return false;
  }

  ManualDeadlineScheduler deadlines;
  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  CombatIntentCollector combat;
  RoomCommandGateway gateway{
      workers,
      readiness,
      [&intents](LobbyRoomOutboundIntent intent) {
        intents.add(std::move(intent));
      },
      [](StateSnapshotProjection) {},
      [&combat](CombatOutboundIntent intent) { combat.add(std::move(intent)); },
      deadlines};
  RudpCombatFlow flow{bindings, gateway};
  if (!createCommittedRoom(gateway, workers)) {
    return false;
  }
  feedCombat(flow, combat);

  std::array<std::uint32_t, 3> nextSequence{0, 2, 2};
  std::uint64_t commandId = 1;
  for (std::uint32_t round = 0; round < 40; ++round) {
    for (std::uint64_t sessionId = 1; sessionId <= 2; ++sessionId) {
      const auto datagram =
          attackDatagram(sessionId, sessionId == 1 ? *hostEpoch : *memberEpoch,
                         nextSequence[sessionId]++, commandId++, 1);
      if (!datagram.has_value() ||
          flow.submitAttack(
              *datagram, endpoint(static_cast<std::uint8_t>(sessionId)),
              kStart + round * 750ms) != RudpCombatSubmitResult::Accepted) {
        return false;
      }
    }
  }
  if (!workers.waitUntilIdle(2s)) {
    return false;
  }
  feedCombat(flow, combat);

  std::size_t spawned = 0;
  for (const auto &transmission :
       flow.pollReliable(std::chrono::steady_clock::now()).transmissions) {
    const auto decoded = RudpLootCodec::decode(transmission.datagram);
    if (decoded.message.has_value() &&
        std::holds_alternative<RudpDropSpawned>(*decoded.message)) {
      ++spawned;
    }
  }
  bool sawProjection = false;
  for (const auto &snapshot : flow.takeUnreliableSnapshots()) {
    const auto decoded = RudpLootCodec::decode(snapshot.datagram);
    const auto *loot =
        decoded.message.has_value()
            ? std::get_if<RudpDropStateSnapshot>(&*decoded.message)
            : nullptr;
    sawProjection =
        sawProjection || (loot != nullptr && loot->battleInstanceId == 1 &&
                          loot->drops.size() == 2);
  }
  if (spawned != 4 || !sawProjection) {
    return false;
  }

  const auto claim =
      claimLootDatagram(1, *hostEpoch, nextSequence[1]++, 81, 99);
  if (!claim.has_value() ||
      flow.submitClaimLoot(*claim, endpoint(1), kStart + 31s) !=
          RudpCombatSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }
  feedCombat(flow, combat);
  for (const auto &transmission :
       flow.pollReliable(std::chrono::steady_clock::now()).transmissions) {
    const auto decoded = RudpLootCodec::decode(transmission.datagram);
    const auto *result =
        decoded.message.has_value()
            ? std::get_if<RudpClaimLootTerminalResult>(&*decoded.message)
            : nullptr;
    if (result != nullptr && result->commandId.low == 81 &&
        result->battleInstanceId == 1 && result->dropId == 99 &&
        result->resultCode ==
            lol::transport::rudp::RudpClaimLootResultCode::UnknownDrop) {
      return true;
    }
  }
  return false;
}

} // namespace

int main() {
  return reorderedReliableCommandsReachApplicationExactlyOnce() &&
                 roomAdmissionFailurePreservesPeerIdentityWithoutMutation() &&
                 realBindingDrivesAttackTerminalAndSnapshot() &&
                 combatFlowEmitsStableLifecycleEvents() &&
                 combatFlowReliableQueueAdmissionAckAndExpiry() &&
                 combatFlowSnapshotServerTickAndSharedSequence() &&
                 combatFlowRebindCannotDeliverOldGenerationOutput() &&
                 combatFlowReliableAdmissionIsExplicitAndRetryRecovers() &&
                 combatFlowTimeoutProjectionEncodesTimedOut() &&
                 combatFlowReliableStateCountsBoundByLifecycle() &&
                 combatFlowSnapshotSequenceCountsBoundByLifecycle() &&
                 combatFlowDeadlineDrivesRealTimeoutPath() &&
                 combatFlowInvalidateDropsOldEpochTransmission() &&
                 combatFlowSessionAuthCloseRemovesReliableState() &&
                 rudpPeerFailureClosesCurrentOnceAndCannotCloseReplacement() &&
                 combatFlowRebindDropsOldEpochWithoutNewOutbound() &&
                 combatFlowCancellationHoldsSnapshotUntilDurability() &&
                 combatFlowAwaitingRoomsRetireTransportSnapshotsIndependently() &&
                 combatFlowActiveIdentityReplaysUntilAck() &&
                 combatFlowDeadlineAfterTeardownIsSafe() &&
                 combatFlowDisconnectTeardownCancelsPendingDeadline() &&
                 combatFlowPostTerminalExitStaysHeldWithoutNewOutput() &&
                 lootFlowUsesRealGatewayAndServerProjection()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
