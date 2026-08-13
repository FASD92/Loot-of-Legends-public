#pragma once

#include <lol/battle/BattleLoadApi.hpp>
#include <lol/battle/CombatApi.hpp>
#include <lol/game_flow/BattleRecovery.hpp>
#include <lol/game_flow/GameplayTransportReadinessPort.hpp>
#include <lol/lobby_room/RoomProjections.hpp>
#include <lol/runtime/DeadlineScheduler.hpp>
#include <lol/runtime/WorkerPool.hpp>
#include <lol/settlement/SettlementCapacityGate.hpp>
#include <lol/settlement/SettlementPublication.hpp>
#include <lol/shared/Identifiers.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace lol::game_flow {

struct AuthenticatedRoomSession final {
  shared::AccountId accountId;
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
  std::string nickname;

  bool operator==(const AuthenticatedRoomSession &) const = default;
};

struct CreateRoomRequest final {
  shared::RequestId requestId;
  std::string title;
  std::uint8_t capacity;

  bool operator==(const CreateRoomRequest &) const = default;
};

struct JoinRoomRequest final {
  shared::RequestId requestId;
  shared::RoomId roomId;

  bool operator==(const JoinRoomRequest &) const = default;
};

struct LeaveRoomRequest final {
  shared::RequestId requestId;

  bool operator==(const LeaveRoomRequest &) const = default;
};

struct SetReadyRequest final {
  shared::RequestId requestId;
  bool ready;

  bool operator==(const SetReadyRequest &) const = default;
};

struct KickRoomMemberRequest final {
  shared::RequestId requestId;
  shared::SessionId targetSessionId;
  shared::SessionGeneration targetGeneration;

  bool operator==(const KickRoomMemberRequest &) const = default;
};

struct HostStartRequest final {
  shared::RequestId requestId;

  bool operator==(const HostStartRequest &) const = default;
};

struct ArenaLoadCompleteRequest final {
  shared::RequestId requestId;
  shared::RoomId roomId;
  shared::BattleInstanceId battleId;

  bool operator==(const ArenaLoadCompleteRequest &) const = default;
};

using LobbyRoomClientMessage =
    std::variant<CreateRoomRequest, JoinRoomRequest, LeaveRoomRequest,
                 SetReadyRequest, KickRoomMemberRequest, HostStartRequest,
                 ArenaLoadCompleteRequest>;

struct RoomCommandEnvelope final {
  AuthenticatedRoomSession session;
  LobbyRoomClientMessage command;
};

struct LobbyEntrySnapshot final {
  AuthenticatedRoomSession session;
  std::vector<lobby_room::RoomSummary> rooms;

  bool operator==(const LobbyEntrySnapshot &) const = default;
};

struct LobbyRoomListUpdate final {
  std::vector<lobby_room::RoomSummary> rooms;

  bool operator==(const LobbyRoomListUpdate &) const = default;
};

struct RoomCommandResponse final {
  shared::RequestId requestId;
  lobby_room::RoomResultCode result;

  bool operator==(const RoomCommandResponse &) const = default;
};

struct BattleCommandResponse final {
  shared::RequestId requestId;
  battle::BattleLoadResultCode result;

  bool operator==(const BattleCommandResponse &) const = default;
};

struct ArenaLoadEntry final {
  shared::RoomId roomId;
  shared::BattleInstanceId battleId;

  bool operator==(const ArenaLoadEntry &) const = default;
};

struct BattleParticipantProjection final {
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
  std::string nickname;

  bool operator==(const BattleParticipantProjection &) const = default;
};

struct ArenaGameplayStart final {
  shared::RoomId roomId;
  shared::BattleInstanceId battleId;
  std::vector<BattleParticipantProjection> participants;

  bool operator==(const ArenaGameplayStart &) const = default;
};

enum class ArenaLoadCancelReason : std::uint16_t {
  NotEnoughReady = 1,
};

struct ArenaLoadCancelled final {
  shared::RoomId roomId;
  shared::BattleInstanceId battleId;
  ArenaLoadCancelReason reason;

  bool operator==(const ArenaLoadCancelled &) const = default;
};

using LobbyRoomServerMessage =
    std::variant<LobbyEntrySnapshot, LobbyRoomListUpdate, RoomCommandResponse,
                 lobby_room::RoomDetailProjection, BattleCommandResponse,
                 ArenaLoadEntry, ArenaGameplayStart, ArenaLoadCancelled,
                 battle::BattleFinalResult, BattleRecoveryNotice>;

struct SessionAudience final {
  shared::SessionId sessionId;
  shared::SessionGeneration generation;

  bool operator==(const SessionAudience &) const = default;
};

struct LobbyAudience final {
  bool operator==(const LobbyAudience &) const = default;
};

struct RoomAudience final {
  shared::RoomId roomId;

  bool operator==(const RoomAudience &) const = default;
};

using LobbyRoomOutboundAudience =
    std::variant<SessionAudience, LobbyAudience, RoomAudience>;

struct LobbyRoomOutboundIntent final {
  LobbyRoomOutboundAudience audience;
  LobbyRoomServerMessage message;
};

struct CombatMonsterSpawnedOutbound final {
  shared::BattleInstanceId battleId;
  std::vector<BattleParticipantProjection> participants;

  bool operator==(const CombatMonsterSpawnedOutbound &) const = default;
};

struct CombatAttackResultOutbound final {
  battle::AttackTerminalResult result;

  bool operator==(const CombatAttackResultOutbound &) const = default;
};

struct CombatTerminalEventOutbound final {
  battle::CombatTerminalRecord terminal;
  std::vector<BattleParticipantProjection> participants;

  bool operator==(const CombatTerminalEventOutbound &) const = default;
};

struct CombatMonsterStateOutbound final {
  battle::CombatProjection projection;
  std::vector<BattleParticipantProjection> participants;

  bool operator==(const CombatMonsterStateOutbound &) const = default;
};

struct LootDropsSpawnedOutbound final {
  battle::LootProjection projection;
  std::vector<BattleParticipantProjection> participants;

  bool operator==(const LootDropsSpawnedOutbound &) const = default;
};

struct LootClaimResultOutbound final {
  battle::ClaimLootTerminalResult result;

  bool operator==(const LootClaimResultOutbound &) const = default;
};

struct LootStateOutbound final {
  battle::LootProjection projection;
  std::vector<BattleParticipantProjection> participants;

  bool operator==(const LootStateOutbound &) const = default;
};

// Immutable teardown intent emitted by the composition owner once a room is
// actually removed. It carries no wire encoding; the combat flow only erases
// the retired battle's snapshot-sequence identity.
struct CombatBattleRetiredOutbound final {
  shared::BattleInstanceId battleId;

  bool operator==(const CombatBattleRetiredOutbound &) const = default;
};

using CombatOutboundMessage =
    std::variant<CombatMonsterSpawnedOutbound, CombatAttackResultOutbound,
                 CombatTerminalEventOutbound, CombatMonsterStateOutbound,
                 LootDropsSpawnedOutbound, LootClaimResultOutbound,
                 LootStateOutbound, CombatBattleRetiredOutbound>;

struct CombatOutboundIntent final {
  std::optional<shared::SessionId> actorSessionId;
  std::optional<shared::SessionGeneration> actorGeneration;
  CombatOutboundMessage message;

  bool operator==(const CombatOutboundIntent &) const = default;
};

enum class RoomSubmitResult : std::uint8_t {
  Accepted,
  RoomOverloaded,
  SchedulingUnavailable,
  InvalidSession,
};

struct RoomExecutionObservation final {
  std::uint64_t gameplayProgressTotal{};
  std::uint64_t serverInvariantTotal{};
  std::chrono::nanoseconds latestQueueDelay{};
  std::chrono::nanoseconds latestProcessingDuration{};
  std::optional<std::chrono::nanoseconds> latestCriticalTerminalLatency;
};

class RoomCommandGateway final {
public:
  using OutboundSink = std::function<void(LobbyRoomOutboundIntent)>;
  using MovementSnapshotSink =
      std::function<void(battle::StateSnapshotProjection)>;
  using CombatOutboundSink = std::function<void(CombatOutboundIntent)>;

  RoomCommandGateway(runtime::WorkerPool &workers, OutboundSink outboundSink);
  RoomCommandGateway(runtime::WorkerPool &workers,
                     const GameplayTransportReadinessPort &readiness,
                     OutboundSink outboundSink);
  RoomCommandGateway(runtime::WorkerPool &workers,
                     const GameplayTransportReadinessPort &readiness,
                     OutboundSink outboundSink,
                     MovementSnapshotSink movementSnapshotSink);
  RoomCommandGateway(runtime::WorkerPool &workers,
                     const GameplayTransportReadinessPort &readiness,
                     OutboundSink outboundSink,
                     MovementSnapshotSink movementSnapshotSink,
                     CombatOutboundSink combatOutboundSink);
  // Composition seam: uses the caller-provided scheduler for the combat
  // deadline instead of an internally owned ThreadDeadlineScheduler so
  // integration tests can fire the 30000 ms deadline deterministically. All
  // other constructors keep the internally owned scheduler.
  RoomCommandGateway(runtime::WorkerPool &workers,
                     const GameplayTransportReadinessPort &readiness,
                     OutboundSink outboundSink,
                     MovementSnapshotSink movementSnapshotSink,
                     CombatOutboundSink combatOutboundSink,
                     runtime::DeadlineScheduler &deadlines);
  RoomCommandGateway(runtime::WorkerPool &workers,
                     const GameplayTransportReadinessPort &readiness,
                     OutboundSink outboundSink,
                     MovementSnapshotSink movementSnapshotSink,
                     CombatOutboundSink combatOutboundSink,
                     runtime::DeadlineScheduler &deadlines,
                     settlement::SettlementCapacityGate &capacityGate);
  RoomCommandGateway(runtime::WorkerPool &workers,
                     const GameplayTransportReadinessPort &readiness,
                     OutboundSink outboundSink,
                     MovementSnapshotSink movementSnapshotSink,
                     CombatOutboundSink combatOutboundSink,
                     runtime::DeadlineScheduler &deadlines,
                     settlement::SettlementCapacityGate &capacityGate,
                     settlement::SettlementStoragePort &storage);
  ~RoomCommandGateway();

  RoomCommandGateway(const RoomCommandGateway &) = delete;
  RoomCommandGateway &operator=(const RoomCommandGateway &) = delete;

  [[nodiscard]] bool enterLobby(const AuthenticatedRoomSession &session);
  [[nodiscard]] RoomSubmitResult submit(RoomCommandEnvelope envelope);
  [[nodiscard]] RoomSubmitResult
  submitMovement(battle::MoveCommand command,
                 std::chrono::steady_clock::time_point receivedAt);
  [[nodiscard]] RoomSubmitResult
  submitAttack(battle::AttackCommand command,
               std::chrono::steady_clock::time_point receivedAt);
  [[nodiscard]] RoomSubmitResult
  submitClaimLoot(battle::ClaimLootCommand command,
                  std::chrono::steady_clock::time_point receivedAt);
  [[nodiscard]] RoomSubmitResult
  submitMovementTick(shared::RoomId roomId, shared::BattleInstanceId battleId,
                     std::uint32_t serverTick);
  [[nodiscard]] bool disconnect(shared::SessionId sessionId,
                                shared::SessionGeneration generation);
  [[nodiscard]] RoomExecutionObservation observation() const;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lol::game_flow
