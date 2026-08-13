#include <lol/game_flow/RoomCommandGateway.hpp>

#include "execution/RoomExecutionDirectory.hpp"
#include "execution/SessionRouteIndex.hpp"
#include "workflows/BattleRecoveryWorkflow.hpp"

#include <lol/lobby_room/RoomApi.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace lol::game_flow {
namespace {

using namespace std::chrono_literals;

constexpr execution::WorkBudget kRoomWorkBudget{
    .maxCommands = 64,
    .maxWallTime = 2ms,
};
constexpr std::size_t kDeadlineQueueCapacity = 1024;

bool validSession(const AuthenticatedRoomSession &session) noexcept {
  const bool validAccount = std::any_of(
      session.accountId.bytes().begin(), session.accountId.bytes().end(),
      [](std::uint8_t value) { return value != 0; });
  return validAccount && session.sessionId.value() != 0 &&
         session.generation.value() != 0 && !session.nickname.empty() &&
         session.nickname.size() <= std::numeric_limits<std::uint16_t>::max();
}

bool isGameplayProgress(execution::RoomCommandKind kind) noexcept {
  switch (kind) {
  case execution::RoomCommandKind::ArenaLoadComplete:
  case execution::RoomCommandKind::Move:
  case execution::RoomCommandKind::Attack:
  case execution::RoomCommandKind::ClaimLoot:
  case execution::RoomCommandKind::LoadBarrierDeadline:
  case execution::RoomCommandKind::MovementTick:
  case execution::RoomCommandKind::CombatDeadline:
  case execution::RoomCommandKind::LootDeadline:
  case execution::RoomCommandKind::DurableAppendCompleted:
  case execution::RoomCommandKind::DurableAppendFailed:
    return true;
  case execution::RoomCommandKind::Join:
  case execution::RoomCommandKind::Leave:
  case execution::RoomCommandKind::SetReady:
  case execution::RoomCommandKind::Kick:
  case execution::RoomCommandKind::HostStartEligibility:
  case execution::RoomCommandKind::ConfirmedDisconnect:
    return false;
  }
  return false;
}

shared::RequestId requestIdOf(const LobbyRoomClientMessage &message) {
  return std::visit([](const auto &request) { return request.requestId; },
                    message);
}

std::vector<BattleParticipantProjection>
participantsOf(const battle::BattleLoadProjection &battle) {
  std::vector<BattleParticipantProjection> participants;
  participants.reserve(battle.capturedParticipants.size());
  for (const auto &participant : battle.capturedParticipants) {
    participants.push_back(BattleParticipantProjection{
        .sessionId = participant.sessionId,
        .generation = participant.generation,
        .nickname = participant.nickname,
    });
  }
  return participants;
}

battle::BattleLoadResultCode
toBattleResult(lobby_room::RoomResultCode result) noexcept {
  switch (result) {
  case lobby_room::RoomResultCode::Ok:
    return battle::BattleLoadResultCode::Ok;
  case lobby_room::RoomResultCode::RoomNotFound:
    return battle::BattleLoadResultCode::RoomNotFound;
  case lobby_room::RoomResultCode::RoomClosed:
    return battle::BattleLoadResultCode::RoomNotOpen;
  case lobby_room::RoomResultCode::NotInRoom:
    return battle::BattleLoadResultCode::NotInRoom;
  case lobby_room::RoomResultCode::NotHost:
    return battle::BattleLoadResultCode::NotHost;
  case lobby_room::RoomResultCode::NotEnoughPlayers:
    return battle::BattleLoadResultCode::NotEnoughPlayers;
  case lobby_room::RoomResultCode::NotAllReady:
    return battle::BattleLoadResultCode::NotAllReady;
  case lobby_room::RoomResultCode::StaleSession:
    return battle::BattleLoadResultCode::StaleSession;
  case lobby_room::RoomResultCode::RoomOverloaded:
    return battle::BattleLoadResultCode::Overloaded;
  default:
    return battle::BattleLoadResultCode::InvalidArgument;
  }
}

class GatewayState final : public std::enable_shared_from_this<GatewayState> {
public:
  GatewayState(runtime::WorkerPool &workers,
               const GameplayTransportReadinessPort *readiness,
               RoomCommandGateway::OutboundSink outboundSink,
               RoomCommandGateway::MovementSnapshotSink movementSnapshotSink,
               RoomCommandGateway::CombatOutboundSink combatOutboundSink,
               runtime::DeadlineScheduler *injectedDeadlines,
               settlement::SettlementCapacityGate *capacityGate,
               settlement::SettlementStoragePort *storage)
      : workers_(workers), readiness_(readiness),
        capacityGate_(
            capacityGate != nullptr
                ? std::optional{*capacityGate}
                : std::optional<settlement::SettlementCapacityGate>{}),
        storage_(storage), outboundSink_(std::move(outboundSink)),
        movementSnapshotSink_(std::move(movementSnapshotSink)),
        combatOutboundSink_(std::move(combatOutboundSink)),
        ownedDeadlines_(
            injectedDeadlines != nullptr
                ? nullptr
                : std::make_unique<runtime::ThreadDeadlineScheduler>(
                      runtime::ThreadDeadlineSchedulerConfig{
                          .queueCapacity = kDeadlineQueueCapacity,
                      })),
        deadlines_(injectedDeadlines != nullptr ? *injectedDeadlines
                                                : *ownedDeadlines_) {
    if (!outboundSink_) {
      throw std::invalid_argument{
          "RoomCommandGateway requires an outbound sink"};
    }
  }

  bool enterLobby(const AuthenticatedRoomSession &session) {
    if (!validSession(session)) {
      return false;
    }
    emit(LobbyRoomOutboundIntent{
        .audience = SessionAudience{session.sessionId, session.generation},
        .message =
            LobbyEntrySnapshot{
                .session = session,
                .rooms = rooms_.summaries(),
            },
    });
    return true;
  }

  RoomSubmitResult submit(RoomCommandEnvelope envelope) {
    if (!validSession(envelope.session) ||
        requestIdOf(envelope.command).value() == 0) {
      return RoomSubmitResult::InvalidSession;
    }
    return std::visit(
        [this, &session = envelope.session](auto request) {
          return submitRequest(session, std::move(request));
        },
        std::move(envelope.command));
  }

  RoomSubmitResult
  submitMovement(battle::MoveCommand command,
                 std::chrono::steady_clock::time_point receivedAt) {
    if (command.sessionId.value() == 0 || command.generation.value() == 0 ||
        command.battleId.value() == 0) {
      return RoomSubmitResult::InvalidSession;
    }
    const auto roomId = routes_.lookup(command.sessionId, command.generation);
    if (!roomId.has_value()) {
      return RoomSubmitResult::InvalidSession;
    }
    const auto entry = rooms_.lookup(*roomId);
    if (!entry.has_value()) {
      static_cast<void>(
          routes_.clear(command.sessionId, command.generation, *roomId));
      return RoomSubmitResult::InvalidSession;
    }
    const auto admission = entry->cell->enqueue(execution::RoomCommandEnvelope{
        .requestId = std::nullopt,
        .command = execution::RoomCellCommand{std::move(command)},
        .receivedAt = receivedAt,
    });
    if (admission == execution::RoomCommandAdmission::Accepted) {
      return RoomSubmitResult::Accepted;
    }
    if (admission == execution::RoomCommandAdmission::RoomRetired) {
      static_cast<void>(
          routes_.clear(command.sessionId, command.generation, *roomId));
      return RoomSubmitResult::InvalidSession;
    }
    return admission == execution::RoomCommandAdmission::RoomOverloaded
               ? RoomSubmitResult::RoomOverloaded
               : RoomSubmitResult::SchedulingUnavailable;
  }

  RoomSubmitResult
  submitAttack(battle::AttackCommand command,
               std::chrono::steady_clock::time_point receivedAt) {
    if (command.sessionId.value() == 0 || command.generation.value() == 0 ||
        command.battleId.value() == 0 ||
        (command.commandId.high == 0 && command.commandId.low == 0)) {
      return RoomSubmitResult::InvalidSession;
    }
    const auto roomId = routes_.lookup(command.sessionId, command.generation);
    if (!roomId.has_value()) {
      return RoomSubmitResult::InvalidSession;
    }
    const auto entry = rooms_.lookup(*roomId);
    if (!entry.has_value()) {
      static_cast<void>(
          routes_.clear(command.sessionId, command.generation, *roomId));
      return RoomSubmitResult::InvalidSession;
    }
    const auto admission = entry->cell->enqueue(execution::RoomCommandEnvelope{
        .requestId = std::nullopt,
        .command = execution::RoomCellCommand{std::move(command)},
        .receivedAt = receivedAt,
    });
    if (admission == execution::RoomCommandAdmission::Accepted) {
      return RoomSubmitResult::Accepted;
    }
    if (admission == execution::RoomCommandAdmission::RoomRetired) {
      static_cast<void>(
          routes_.clear(command.sessionId, command.generation, *roomId));
      return RoomSubmitResult::InvalidSession;
    }
    return admission == execution::RoomCommandAdmission::RoomOverloaded
               ? RoomSubmitResult::RoomOverloaded
               : RoomSubmitResult::SchedulingUnavailable;
  }

  RoomSubmitResult
  submitClaimLoot(battle::ClaimLootCommand command,
                  std::chrono::steady_clock::time_point receivedAt) {
    if (command.sessionId.value() == 0 || command.generation.value() == 0 ||
        command.battleId.value() == 0 ||
        (command.commandId.high == 0 && command.commandId.low == 0)) {
      return RoomSubmitResult::InvalidSession;
    }
    const auto roomId = routes_.lookup(command.sessionId, command.generation);
    if (!roomId.has_value()) {
      return RoomSubmitResult::InvalidSession;
    }
    const auto entry = rooms_.lookup(*roomId);
    if (!entry.has_value()) {
      static_cast<void>(
          routes_.clear(command.sessionId, command.generation, *roomId));
      return RoomSubmitResult::InvalidSession;
    }
    const auto admission = entry->cell->enqueue(execution::RoomCommandEnvelope{
        .requestId = std::nullopt,
        .command = execution::RoomCellCommand{std::move(command)},
        .receivedAt = receivedAt,
    });
    if (admission == execution::RoomCommandAdmission::Accepted) {
      return RoomSubmitResult::Accepted;
    }
    if (admission == execution::RoomCommandAdmission::RoomRetired) {
      static_cast<void>(
          routes_.clear(command.sessionId, command.generation, *roomId));
      return RoomSubmitResult::InvalidSession;
    }
    return admission == execution::RoomCommandAdmission::RoomOverloaded
               ? RoomSubmitResult::RoomOverloaded
               : RoomSubmitResult::SchedulingUnavailable;
  }

  RoomSubmitResult submitMovementTick(shared::RoomId roomId,
                                      shared::BattleInstanceId battleId,
                                      std::uint32_t serverTick) {
    if (roomId.value() == 0 || battleId.value() == 0) {
      return RoomSubmitResult::InvalidSession;
    }
    const auto entry = rooms_.lookup(roomId);
    if (!entry.has_value()) {
      return RoomSubmitResult::InvalidSession;
    }
    const auto admission =
        entry->cell->enqueueControl(execution::RoomControlEnvelope{
            .command =
                execution::RoomControlCommand{battle::MovementTickCommand{
                    .battleId = battleId, .serverTick = serverTick}},
        });
    if (admission == execution::RoomCommandAdmission::Accepted) {
      return RoomSubmitResult::Accepted;
    }
    if (admission == execution::RoomCommandAdmission::RoomRetired) {
      return RoomSubmitResult::InvalidSession;
    }
    return admission == execution::RoomCommandAdmission::ControlReserveExhausted
               ? RoomSubmitResult::RoomOverloaded
               : RoomSubmitResult::SchedulingUnavailable;
  }

  bool disconnect(shared::SessionId sessionId,
                  shared::SessionGeneration generation) {
    const auto roomId = routes_.lookup(sessionId, generation);
    if (!roomId.has_value()) {
      return false;
    }
    const auto entry = rooms_.lookup(*roomId);
    if (!entry.has_value()) {
      static_cast<void>(routes_.clear(sessionId, generation, *roomId));
      return false;
    }
    return entry->cell->enqueueControl(execution::RoomControlEnvelope{
               .command =
                   execution::RoomControlCommand{
                       execution::ConfirmedDisconnectCommand{
                           .sessionId = sessionId,
                           .generation = generation,
                       }},
               .occurredAt = std::chrono::steady_clock::now(),
           }) == execution::RoomCommandAdmission::Accepted;
  }

  RoomExecutionObservation observation() const {
    std::lock_guard lock{observationMutex_};
    return observation_;
  }

private:
  void emit(LobbyRoomOutboundIntent intent) {
    outboundSink_(std::move(intent));
  }

  void respond(const AuthenticatedRoomSession &session,
               shared::RequestId requestId, lobby_room::RoomResultCode result) {
    emit(LobbyRoomOutboundIntent{
        .audience = SessionAudience{session.sessionId, session.generation},
        .message =
            RoomCommandResponse{
                .requestId = requestId,
                .result = result,
            },
    });
  }

  void respondBattle(const AuthenticatedRoomSession &session,
                     shared::RequestId requestId,
                     battle::BattleLoadResultCode result) {
    emit(LobbyRoomOutboundIntent{
        .audience = SessionAudience{session.sessionId, session.generation},
        .message =
            BattleCommandResponse{
                .requestId = requestId,
                .result = result,
            },
    });
  }

  void emitDetail(const lobby_room::RoomDetailProjection &detail) {
    emit(LobbyRoomOutboundIntent{
        .audience = RoomAudience{detail.roomId},
        .message = detail,
    });
  }

  void emitRoomList() {
    emit(LobbyRoomOutboundIntent{
        .audience = LobbyAudience{},
        .message = LobbyRoomListUpdate{.rooms = rooms_.summaries()},
    });
  }

  void emitCombat(CombatOutboundIntent intent) {
    if (combatOutboundSink_) {
      combatOutboundSink_(std::move(intent));
    }
  }

  void
  emitTerminalIfNew(shared::RoomId roomId,
                    const battle::CombatProjection &combat,
                    std::vector<BattleParticipantProjection> participants) {
    if (!combat.terminal.has_value()) {
      return;
    }
    const auto terminal = *combat.terminal;
    bool shouldEmit = false;
    {
      std::lock_guard lock{terminalMutex_};
      const auto previous = lastEmittedTerminal_.find(roomId);
      if (previous == lastEmittedTerminal_.end() ||
          previous->second.eventId != terminal.eventId ||
          previous->second.eventSequence != terminal.eventSequence) {
        lastEmittedTerminal_.insert_or_assign(roomId, terminal);
        shouldEmit = true;
      }
    }
    if (shouldEmit) {
      emitCombat(CombatOutboundIntent{
          .actorSessionId = std::nullopt,
          .actorGeneration = std::nullopt,
          .message =
              CombatTerminalEventOutbound{
                  .terminal = terminal,
                  .participants = std::move(participants),
              },
      });
    }
  }

  void emitAttackOutcome(shared::RoomId roomId,
                         const execution::RoomCommandOutcome &outcome) {
    if (outcome.attackResult.has_value() &&
        outcome.actorSessionId.has_value() &&
        outcome.actorGeneration.has_value()) {
      emitCombat(CombatOutboundIntent{
          .actorSessionId = *outcome.actorSessionId,
          .actorGeneration = *outcome.actorGeneration,
          .message =
              CombatAttackResultOutbound{.result = *outcome.attackResult},
      });
    }
    if (outcome.combat.has_value() && outcome.battle.has_value()) {
      const auto participants = participantsOf(*outcome.battle);
      emitCombat(CombatOutboundIntent{
          .actorSessionId = std::nullopt,
          .actorGeneration = std::nullopt,
          .message =
              CombatMonsterStateOutbound{
                  .projection = *outcome.combat,
                  .participants = participants,
              },
      });
      if (outcome.attackResult.has_value() &&
          outcome.attackResult->code == battle::AttackResultCode::Ok) {
        emitTerminalIfNew(roomId, *outcome.combat, participants);
      }
    }
    if (outcome.lootResolutionOpened && outcome.loot.has_value() &&
        outcome.battle.has_value()) {
      const auto participants = participantsOf(*outcome.battle);
      emitCombat(CombatOutboundIntent{
          .actorSessionId = std::nullopt,
          .actorGeneration = std::nullopt,
          .message =
              LootDropsSpawnedOutbound{
                  .projection = *outcome.loot,
                  .participants = participants,
              },
      });
      emitCombat(CombatOutboundIntent{
          .actorSessionId = std::nullopt,
          .actorGeneration = std::nullopt,
          .message =
              LootStateOutbound{
                  .projection = *outcome.loot,
                  .participants = std::move(participants),
              },
      });
    }
  }

  void emitLootOutcome(const execution::RoomCommandOutcome &outcome) {
    if (outcome.lootClaimResult.has_value() &&
        outcome.actorSessionId.has_value() &&
        outcome.actorGeneration.has_value()) {
      emitCombat(CombatOutboundIntent{
          .actorSessionId = *outcome.actorSessionId,
          .actorGeneration = *outcome.actorGeneration,
          .message =
              LootClaimResultOutbound{.result = *outcome.lootClaimResult},
      });
    }
    if (outcome.loot.has_value() && outcome.battle.has_value()) {
      emitCombat(CombatOutboundIntent{
          .actorSessionId = std::nullopt,
          .actorGeneration = std::nullopt,
          .message =
              LootStateOutbound{
                  .projection = *outcome.loot,
                  .participants = participantsOf(*outcome.battle),
              },
      });
    }
  }

  void emitLootDeadlineOutcome(const execution::RoomCommandOutcome &outcome) {
    if (outcome.lootDeadlineCode == battle::LootDeadlineResultCode::Ok &&
        outcome.loot.has_value() && outcome.battle.has_value()) {
      emitCombat(CombatOutboundIntent{
          .actorSessionId = std::nullopt,
          .actorGeneration = std::nullopt,
          .message =
              LootStateOutbound{
                  .projection = *outcome.loot,
                  .participants = participantsOf(*outcome.battle),
              },
      });
    }
  }

  void emitCombatDeadlineOutcome(shared::RoomId roomId,
                                 const execution::RoomCommandOutcome &outcome) {
    if (outcome.combatDeadlineCode == battle::CombatDeadlineResultCode::Ok &&
        outcome.combat.has_value() && outcome.battle.has_value()) {
      const auto participants = participantsOf(*outcome.battle);
      emitCombat(CombatOutboundIntent{
          .actorSessionId = std::nullopt,
          .actorGeneration = std::nullopt,
          .message =
              CombatMonsterStateOutbound{
                  .projection = *outcome.combat,
                  .participants = participants,
              },
      });
      emitTerminalIfNew(roomId, *outcome.combat, participants);
    }
  }

  RoomSubmitResult submitRequest(const AuthenticatedRoomSession &session,
                                 CreateRoomRequest request) {
    if (routes_.lookup(session.sessionId, session.generation).has_value()) {
      respond(session, request.requestId,
              lobby_room::RoomResultCode::AlreadyInRoom);
      return RoomSubmitResult::Accepted;
    }

    const shared::RoomId roomId{nextRoomId_.fetch_add(1)};
    if (roomId.value() == 0 || routes_.bind(execution::SessionRoomRoute{
                                   .sessionId = session.sessionId,
                                   .generation = session.generation,
                                   .roomId = roomId,
                               }) != execution::RouteBindResult::Inserted) {
      respond(session, request.requestId,
              lobby_room::RoomResultCode::AlreadyInRoom);
      return RoomSubmitResult::Accepted;
    }

    auto created = lobby_room::Room::create(lobby_room::CreateRoomCommand{
        .roomId = roomId,
        .title = std::move(request.title),
        .capacity = request.capacity,
        .creator =
            lobby_room::RoomMemberIdentity{
                .accountId = session.accountId,
                .sessionId = session.sessionId,
                .generation = session.generation,
                .nickname = session.nickname,
            },
    });
    if (!created.room.has_value()) {
      static_cast<void>(
          routes_.clear(session.sessionId, session.generation, roomId));
      respond(session, request.requestId, created.code);
      return RoomSubmitResult::Accepted;
    }

    std::weak_ptr<GatewayState> weak = weak_from_this();
    auto entry = rooms_.create(
        workers_, deadlines_, std::move(*created.room), kRoomWorkBudget,
        [weak, roomId](execution::RoomCommandOutcome outcome) mutable {
          if (auto state = weak.lock()) {
            state->handleOutcome(roomId, std::move(outcome));
          }
        },
        readiness_, capacityGate_.has_value() ? &*capacityGate_ : nullptr,
        storage_);
    if (!entry.has_value()) {
      static_cast<void>(
          routes_.clear(session.sessionId, session.generation, roomId));
      respond(session, request.requestId,
              lobby_room::RoomResultCode::InvalidArgument);
      return RoomSubmitResult::Accepted;
    }

    respond(session, request.requestId, lobby_room::RoomResultCode::Ok);
    if (const auto detail = entry->cell->detail(); detail.has_value()) {
      emitDetail(*detail);
    }
    emitRoomList();
    return RoomSubmitResult::Accepted;
  }

  RoomSubmitResult submitRequest(const AuthenticatedRoomSession &session,
                                 JoinRoomRequest request) {
    std::unique_lock pendingLock{pendingJoinMutex_};
    const auto current = routes_.lookup(session.sessionId, session.generation);
    if (current.has_value() && *current != request.roomId) {
      pendingLock.unlock();
      respond(session, request.requestId,
              lobby_room::RoomResultCode::AlreadyInRoom);
      return RoomSubmitResult::Accepted;
    }

    const auto entry = rooms_.lookup(request.roomId);
    if (!entry.has_value()) {
      pendingLock.unlock();
      respond(session, request.requestId,
              lobby_room::RoomResultCode::RoomNotFound);
      return RoomSubmitResult::Accepted;
    }
    const auto routeAdmission = current.has_value()
                                    ? execution::RouteBindResult::AlreadyBound
                                    : routes_.bind(execution::SessionRoomRoute{
                                          .sessionId = session.sessionId,
                                          .generation = session.generation,
                                          .roomId = request.roomId,
                                      });
    if (routeAdmission == execution::RouteBindResult::Conflict) {
      pendingLock.unlock();
      respond(session, request.requestId,
              lobby_room::RoomResultCode::AlreadyInRoom);
      return RoomSubmitResult::Accepted;
    }
    ++pendingJoins_[pendingJoinKey(session.sessionId, session.generation,
                                   request.roomId)];
    pendingLock.unlock();
    return enqueue(
        session, request.roomId, request.requestId,
        execution::RoomCommandEnvelope{
            .requestId = request.requestId,
            .command = execution::RoomCellCommand{lobby_room::JoinRoomCommand{
                .member =
                    lobby_room::RoomMemberIdentity{
                        .accountId = session.accountId,
                        .sessionId = session.sessionId,
                        .generation = session.generation,
                        .nickname = session.nickname,
                    },
            }},
        },
        routeAdmission == execution::RouteBindResult::Inserted,
        lobby_room::RoomResultCode::RoomNotFound);
  }

  RoomSubmitResult submitRequest(const AuthenticatedRoomSession &session,
                                 LeaveRoomRequest request) {
    return enqueueRouted(
        session, request.requestId, [session](shared::RequestId requestId) {
          return execution::RoomCommandEnvelope{
              .requestId = requestId,
              .command =
                  execution::RoomCellCommand{lobby_room::LeaveRoomCommand{
                      .sessionId = session.sessionId,
                      .generation = session.generation,
                  }},
              .receivedAt = std::chrono::steady_clock::now(),
          };
        });
  }

  RoomSubmitResult submitRequest(const AuthenticatedRoomSession &session,
                                 SetReadyRequest request) {
    return enqueueRouted(
        session, request.requestId,
        [session, ready = request.ready](shared::RequestId requestId) {
          return execution::RoomCommandEnvelope{
              .requestId = requestId,
              .command = execution::RoomCellCommand{lobby_room::SetReadyCommand{
                  .sessionId = session.sessionId,
                  .generation = session.generation,
                  .ready = ready,
              }},
          };
        });
  }

  RoomSubmitResult submitRequest(const AuthenticatedRoomSession &session,
                                 KickRoomMemberRequest request) {
    return enqueueRouted(
        session, request.requestId,
        [session, request](shared::RequestId requestId) {
          return execution::RoomCommandEnvelope{
              .requestId = requestId,
              .command =
                  execution::RoomCellCommand{lobby_room::KickRoomMemberCommand{
                      .actorSessionId = session.sessionId,
                      .actorGeneration = session.generation,
                      .targetSessionId = request.targetSessionId,
                      .targetGeneration = request.targetGeneration,
                  }},
          };
        });
  }

  RoomSubmitResult submitRequest(const AuthenticatedRoomSession &session,
                                 HostStartRequest request) {
    const auto roomId = routes_.lookup(session.sessionId, session.generation);
    if (!roomId.has_value()) {
      respondBattle(session, request.requestId,
                    battle::BattleLoadResultCode::NotInRoom);
      return RoomSubmitResult::Accepted;
    }
    return enqueueBattle(session, *roomId, request.requestId,
                         execution::RoomCommandEnvelope{
                             .requestId = request.requestId,
                             .command =
                                 execution::RoomCellCommand{
                                     lobby_room::HostStartEligibilityCommand{
                                         .actorSessionId = session.sessionId,
                                         .actorGeneration = session.generation,
                                     }},
                         });
  }

  RoomSubmitResult submitRequest(const AuthenticatedRoomSession &session,
                                 ArenaLoadCompleteRequest request) {
    const auto roomId = routes_.lookup(session.sessionId, session.generation);
    if (!roomId.has_value()) {
      respondBattle(session, request.requestId,
                    battle::BattleLoadResultCode::NotInRoom);
      return RoomSubmitResult::Accepted;
    }
    return enqueueBattle(
        session, *roomId, request.requestId,
        execution::RoomCommandEnvelope{
            .requestId = request.requestId,
            .command =
                execution::RoomCellCommand{battle::ArenaLoadCompleteCommand{
                    .sessionId = session.sessionId,
                    .generation = session.generation,
                    .roomId = request.roomId,
                    .battleId = request.battleId,
                }},
        });
  }

  template <typename MakeEnvelope>
  RoomSubmitResult enqueueRouted(const AuthenticatedRoomSession &session,
                                 shared::RequestId requestId,
                                 MakeEnvelope makeEnvelope) {
    const auto roomId = routes_.lookup(session.sessionId, session.generation);
    if (!roomId.has_value()) {
      respond(session, requestId, lobby_room::RoomResultCode::NotInRoom);
      return RoomSubmitResult::Accepted;
    }
    return enqueue(session, *roomId, requestId, makeEnvelope(requestId), false,
                   lobby_room::RoomResultCode::NotInRoom);
  }

  RoomSubmitResult enqueue(const AuthenticatedRoomSession &session,
                           shared::RoomId roomId, shared::RequestId requestId,
                           execution::RoomCommandEnvelope envelope,
                           bool clearRouteOnAdmissionFailure,
                           lobby_room::RoomResultCode missingResult) {
    const bool isJoin =
        std::holds_alternative<lobby_room::JoinRoomCommand>(envelope.command);
    const auto entry = rooms_.lookup(roomId);
    if (!entry.has_value()) {
      respondMissingRoom(session, requestId, roomId, missingResult, isJoin);
      return RoomSubmitResult::Accepted;
    }
    const auto admission = entry->cell->enqueue(std::move(envelope));
    if (admission == execution::RoomCommandAdmission::Accepted) {
      return RoomSubmitResult::Accepted;
    }
    if (admission == execution::RoomCommandAdmission::RoomRetired) {
      // The room was removed between lookup and admission; reuse the
      // missing-room behavior instead of overload.
      respondMissingRoom(session, requestId, roomId, missingResult, isJoin);
      return RoomSubmitResult::Accepted;
    }
    if (isJoin) {
      finishPendingJoin(session.sessionId, session.generation, roomId,
                        clearRouteOnAdmissionFailure);
    } else if (clearRouteOnAdmissionFailure) {
      static_cast<void>(
          routes_.clear(session.sessionId, session.generation, roomId));
    }
    respond(session, requestId, lobby_room::RoomResultCode::RoomOverloaded);
    return admission == execution::RoomCommandAdmission::RoomOverloaded
               ? RoomSubmitResult::RoomOverloaded
               : RoomSubmitResult::SchedulingUnavailable;
  }

  void respondMissingRoom(const AuthenticatedRoomSession &session,
                          shared::RequestId requestId, shared::RoomId roomId,
                          lobby_room::RoomResultCode missingResult,
                          bool isJoin) {
    if (isJoin) {
      finishPendingJoin(session.sessionId, session.generation, roomId, true);
    } else {
      static_cast<void>(
          routes_.clear(session.sessionId, session.generation, roomId));
    }
    respond(session, requestId, missingResult);
  }

  RoomSubmitResult enqueueBattle(const AuthenticatedRoomSession &session,
                                 shared::RoomId roomId,
                                 shared::RequestId requestId,
                                 execution::RoomCommandEnvelope envelope) {
    const auto entry = rooms_.lookup(roomId);
    if (!entry.has_value()) {
      respondBattle(session, requestId,
                    battle::BattleLoadResultCode::RoomNotFound);
      return RoomSubmitResult::Accepted;
    }
    const auto admission = entry->cell->enqueue(std::move(envelope));
    if (admission == execution::RoomCommandAdmission::Accepted) {
      return RoomSubmitResult::Accepted;
    }
    if (admission == execution::RoomCommandAdmission::RoomRetired) {
      respondBattle(session, requestId,
                    battle::BattleLoadResultCode::RoomNotFound);
      return RoomSubmitResult::Accepted;
    }
    respondBattle(session, requestId, battle::BattleLoadResultCode::Overloaded);
    return admission == execution::RoomCommandAdmission::RoomOverloaded
               ? RoomSubmitResult::RoomOverloaded
               : RoomSubmitResult::SchedulingUnavailable;
  }

  void handleOutcome(shared::RoomId roomId,
                     execution::RoomCommandOutcome outcome) {
    recordObservation(outcome);
    if (outcome.recoveryNotice.has_value()) {
      if (outcome.recoveryNotice->reason ==
          BattleRecoveryReason::ResultGenerationFailed) {
        if (outcome.kind == execution::RoomCommandKind::Attack) {
          emitAttackOutcome(roomId, outcome);
        } else if (outcome.kind == execution::RoomCommandKind::CombatDeadline) {
          emitCombatDeadlineOutcome(roomId, outcome);
        } else if (outcome.kind == execution::RoomCommandKind::ClaimLoot) {
          emitLootOutcome(outcome);
        } else if (outcome.kind == execution::RoomCommandKind::LootDeadline) {
          emitLootDeadlineOutcome(outcome);
        }
        handleResultFailureRecovery(roomId, outcome);
        return;
      }
      emit(LobbyRoomOutboundIntent{
          .audience = RoomAudience{roomId},
          .message = *outcome.recoveryNotice,
      });
    }
    if (outcome.kind == execution::RoomCommandKind::Move) {
      return;
    }
    if (outcome.kind == execution::RoomCommandKind::Attack) {
      emitAttackOutcome(roomId, outcome);
      return;
    }
    if (outcome.kind == execution::RoomCommandKind::CombatDeadline) {
      emitCombatDeadlineOutcome(roomId, outcome);
      return;
    }
    if (outcome.kind == execution::RoomCommandKind::ClaimLoot) {
      emitLootOutcome(outcome);
    } else if (outcome.kind == execution::RoomCommandKind::LootDeadline) {
      emitLootDeadlineOutcome(outcome);
    }
    if (outcome.kind == execution::RoomCommandKind::MovementTick) {
      if (outcome.snapshot.has_value() && movementSnapshotSink_) {
        movementSnapshotSink_(std::move(*outcome.snapshot));
      }
      return;
    }
    if (outcome.settlementDurable && outcome.finalResult.has_value() &&
        outcome.detail.has_value()) {
      // Same Cell outcome: enqueue the immutable Final Result before the OPEN
      // Room projection. The downstream sink observes this call order.
      emit(LobbyRoomOutboundIntent{
          .audience = RoomAudience{roomId},
          .message = *outcome.finalResult,
      });
    }
    if (outcome.settlementDurable && outcome.battle.has_value()) {
      emitCombat(CombatOutboundIntent{
          .actorSessionId = std::nullopt,
          .actorGeneration = std::nullopt,
          .message =
              CombatBattleRetiredOutbound{
                  .battleId = outcome.battle->battleId,
              },
      });
      std::lock_guard lock{terminalMutex_};
      static_cast<void>(lastEmittedTerminal_.erase(roomId));
    }
    if (outcome.actorSessionId.has_value() &&
        outcome.actorGeneration.has_value()) {
      if (outcome.kind == execution::RoomCommandKind::Join) {
        finishPendingJoin(*outcome.actorSessionId, *outcome.actorGeneration,
                          roomId,
                          outcome.code != lobby_room::RoomResultCode::Ok);
      } else if (outcome.kind ==
                 execution::RoomCommandKind::ConfirmedDisconnect) {
        static_cast<void>(routes_.clear(*outcome.actorSessionId,
                                        *outcome.actorGeneration, roomId));
      } else if (outcome.kind == execution::RoomCommandKind::Leave ||
                 ((outcome.kind == execution::RoomCommandKind::SetReady ||
                   outcome.kind ==
                       execution::RoomCommandKind::HostStartEligibility) &&
                  (outcome.code == lobby_room::RoomResultCode::NotInRoom ||
                   outcome.code == lobby_room::RoomResultCode::StaleSession))) {
        clearRouteUnlessJoinPending(*outcome.actorSessionId,
                                    *outcome.actorGeneration, roomId);
      }
    }
    if (outcome.kind == execution::RoomCommandKind::Kick &&
        outcome.code == lobby_room::RoomResultCode::Ok &&
        outcome.targetSessionId.has_value() &&
        outcome.targetGeneration.has_value()) {
      clearRouteUnlessJoinPending(*outcome.targetSessionId,
                                  *outcome.targetGeneration, roomId);
    }

    if (outcome.code == lobby_room::RoomResultCode::Ok) {
      if (outcome.summary.has_value()) {
        static_cast<void>(rooms_.updateSummary(*outcome.summary));
      } else if (outcome.lifecycle ==
                 lobby_room::RoomLifecycle::AwaitingSettlementDurability) {
        // The current Room membership is empty, but the immutable terminal
        // batch must survive until a future matching durability completion.
        // Retire only the now-recipientless combat transport projection, then
        // hide the Room from the lobby list without retiring the Cell/Battle.
        const bool enteredDurabilityHold = rooms_.hideSummary(roomId);
        if (enteredDurabilityHold && outcome.battle.has_value()) {
          emitCombat(CombatOutboundIntent{
              .actorSessionId = std::nullopt,
              .actorGeneration = std::nullopt,
              .message =
                  CombatBattleRetiredOutbound{
                      .battleId = outcome.battle->battleId,
                  },
          });
        }
        if (enteredDurabilityHold) {
          std::lock_guard lock{terminalMutex_};
          static_cast<void>(lastEmittedTerminal_.erase(roomId));
        }
      } else {
        // The room removal boundary is shared by last-Leave and
        // ConfirmedDisconnect. Emit the explicit retirement intent first as the
        // final pre-retirement teardown so the combat flow erases the old
        // battle snapshot-sequence identity, then remove the room (which
        // logically retires the Cell and cancels queued work).
        if (outcome.battle.has_value() && !outcome.settlementDurable) {
          emitCombat(CombatOutboundIntent{
              .actorSessionId = std::nullopt,
              .actorGeneration = std::nullopt,
              .message =
                  CombatBattleRetiredOutbound{
                      .battleId = outcome.battle->battleId,
                  },
          });
        }
        static_cast<void>(rooms_.remove(roomId));
        std::lock_guard lock{terminalMutex_};
        static_cast<void>(lastEmittedTerminal_.erase(roomId));
      }
    }

    if (outcome.requestId.has_value() && outcome.actorSessionId.has_value() &&
        outcome.actorGeneration.has_value()) {
      const auto audience =
          SessionAudience{*outcome.actorSessionId, *outcome.actorGeneration};
      if (outcome.kind == execution::RoomCommandKind::HostStartEligibility ||
          outcome.kind == execution::RoomCommandKind::ArenaLoadComplete) {
        emit(LobbyRoomOutboundIntent{
            .audience = audience,
            .message =
                BattleCommandResponse{
                    .requestId = *outcome.requestId,
                    .result = outcome.battleCode.value_or(
                        toBattleResult(outcome.code)),
                },
        });
      } else {
        emit(LobbyRoomOutboundIntent{
            .audience = audience,
            .message =
                RoomCommandResponse{
                    .requestId = *outcome.requestId,
                    .result = outcome.code,
                },
        });
      }
    }
    if (outcome.code != lobby_room::RoomResultCode::Ok) {
      return;
    }
    if (outcome.detail.has_value()) {
      emitDetail(*outcome.detail);
    }
    if (outcome.kind == execution::RoomCommandKind::Join ||
        outcome.kind == execution::RoomCommandKind::Leave ||
        outcome.kind == execution::RoomCommandKind::Kick ||
        outcome.kind == execution::RoomCommandKind::ConfirmedDisconnect) {
      emitRoomList();
    }
    if (outcome.battle.has_value() &&
        outcome.kind == execution::RoomCommandKind::HostStartEligibility &&
        outcome.battle->state == battle::BattleLoadState::LoadBarrierOpen) {
      emit(LobbyRoomOutboundIntent{
          .audience = RoomAudience{roomId},
          .message =
              ArenaLoadEntry{
                  .roomId = outcome.battle->roomId,
                  .battleId = outcome.battle->battleId,
              },
      });
    } else if (outcome.battle.has_value() &&
               outcome.battle->state ==
                   battle::BattleLoadState::LoadCancelled) {
      emit(LobbyRoomOutboundIntent{
          .audience = RoomAudience{roomId},
          .message =
              ArenaLoadCancelled{
                  .roomId = outcome.battle->roomId,
                  .battleId = outcome.battle->battleId,
                  .reason = ArenaLoadCancelReason::NotEnoughReady,
              },
      });
    } else if (outcome.battle.has_value() && outcome.gameplayStartCommitted) {
      const auto participants = participantsOf(*outcome.battle);
      emit(LobbyRoomOutboundIntent{
          .audience = RoomAudience{roomId},
          .message =
              ArenaGameplayStart{
                  .roomId = outcome.battle->roomId,
                  .battleId = outcome.battle->battleId,
                  .participants = std::move(participants),
              },
      });
      emitCombat(CombatOutboundIntent{
          .actorSessionId = std::nullopt,
          .actorGeneration = std::nullopt,
          .message =
              CombatMonsterSpawnedOutbound{
                  .battleId = outcome.battle->battleId,
                  .participants = participantsOf(*outcome.battle),
              },
      });
    }
  }

  void
  handleResultFailureRecovery(shared::RoomId roomId,
                              const execution::RoomCommandOutcome &outcome) {
    if (!outcome.recoveryNotice.has_value() ||
        outcome.recoveryNotice->roomId != roomId ||
        outcome.recoveryNotice->reason !=
            BattleRecoveryReason::ResultGenerationFailed ||
        outcome.recoveryNotice->battleId.value() == 0u) {
      return;
    }
    for (const auto &participant : outcome.recoveryParticipants) {
      static_cast<void>(routes_.clear(participant.sessionId,
                                      participant.sessionGeneration, roomId));
    }
    if (outcome.battle.has_value()) {
      emitCombat(CombatOutboundIntent{
          .actorSessionId = std::nullopt,
          .actorGeneration = std::nullopt,
          .message =
              CombatBattleRetiredOutbound{
                  .battleId = outcome.battle->battleId,
              },
      });
    }
    static_cast<void>(rooms_.remove(roomId));
    {
      std::lock_guard lock{terminalMutex_};
      static_cast<void>(lastEmittedTerminal_.erase(roomId));
    }
    const auto intents = workflows::composeResultFailureVisibility(
        workflows::ResultGenerationFailureRecovery{
            .notice = *outcome.recoveryNotice,
            .participants = outcome.recoveryParticipants,
        },
        rooms_.summaries());
    for (const auto &intent : intents) {
      emit(intent);
    }
  }

  using PendingJoinKey =
      std::tuple<shared::SessionId, shared::SessionGeneration, shared::RoomId>;

  static PendingJoinKey pendingJoinKey(shared::SessionId sessionId,
                                       shared::SessionGeneration generation,
                                       shared::RoomId roomId) {
    return {sessionId, generation, roomId};
  }

  void recordObservation(const execution::RoomCommandOutcome &outcome) {
    std::lock_guard lock{observationMutex_};
    observation_.latestQueueDelay = outcome.queueDelay;
    observation_.latestProcessingDuration = outcome.processingDuration;
    if (outcome.criticalTerminalLatency.has_value()) {
      observation_.latestCriticalTerminalLatency =
          outcome.criticalTerminalLatency;
    }
    if (isGameplayProgress(outcome.kind)) {
      ++observation_.gameplayProgressTotal;
    }
    if (outcome.recoveryNotice.has_value() &&
        outcome.recoveryNotice->reason ==
            BattleRecoveryReason::ResultGenerationFailed) {
      ++observation_.serverInvariantTotal;
    }
  }

  void finishPendingJoin(shared::SessionId sessionId,
                         shared::SessionGeneration generation,
                         shared::RoomId roomId, bool clearIfLast) {
    std::lock_guard lock{pendingJoinMutex_};
    const auto key = pendingJoinKey(sessionId, generation, roomId);
    const auto pending = pendingJoins_.find(key);
    if (pending != pendingJoins_.end() && --pending->second == 0) {
      pendingJoins_.erase(pending);
    }
    if (clearIfLast && !pendingJoins_.contains(key)) {
      static_cast<void>(routes_.clear(sessionId, generation, roomId));
    }
  }

  void clearRouteUnlessJoinPending(shared::SessionId sessionId,
                                   shared::SessionGeneration generation,
                                   shared::RoomId roomId) {
    std::lock_guard lock{pendingJoinMutex_};
    if (!pendingJoins_.contains(
            pendingJoinKey(sessionId, generation, roomId))) {
      static_cast<void>(routes_.clear(sessionId, generation, roomId));
    }
  }

  runtime::WorkerPool &workers_;
  const GameplayTransportReadinessPort *readiness_;
  std::optional<settlement::SettlementCapacityGate> capacityGate_;
  settlement::SettlementStoragePort *storage_;
  RoomCommandGateway::OutboundSink outboundSink_;
  RoomCommandGateway::MovementSnapshotSink movementSnapshotSink_;
  RoomCommandGateway::CombatOutboundSink combatOutboundSink_;
  std::unique_ptr<runtime::ThreadDeadlineScheduler> ownedDeadlines_;
  runtime::DeadlineScheduler &deadlines_;
  execution::RoomExecutionDirectory rooms_;
  execution::SessionRouteIndex routes_;
  std::mutex pendingJoinMutex_;
  std::map<PendingJoinKey, std::size_t> pendingJoins_;
  std::mutex terminalMutex_;
  std::map<shared::RoomId, battle::CombatTerminalRecord> lastEmittedTerminal_;
  std::atomic<std::uint64_t> nextRoomId_{1};
  mutable std::mutex observationMutex_;
  RoomExecutionObservation observation_;
};

} // namespace

class RoomCommandGateway::Impl final {
public:
  Impl(runtime::WorkerPool &workers,
       const GameplayTransportReadinessPort *readiness,
       OutboundSink outboundSink, MovementSnapshotSink movementSnapshotSink,
       CombatOutboundSink combatOutboundSink,
       runtime::DeadlineScheduler *injectedDeadlines,
       settlement::SettlementCapacityGate *capacityGate,
       settlement::SettlementStoragePort *storage)
      : state(std::make_shared<GatewayState>(
            workers, readiness, std::move(outboundSink),
            std::move(movementSnapshotSink), std::move(combatOutboundSink),
            injectedDeadlines, capacityGate, storage)) {}

  std::shared_ptr<GatewayState> state;
};

RoomCommandGateway::RoomCommandGateway(runtime::WorkerPool &workers,
                                       OutboundSink outboundSink)
    : impl_(std::make_unique<Impl>(workers, nullptr, std::move(outboundSink),
                                   MovementSnapshotSink{}, CombatOutboundSink{},
                                   nullptr, nullptr, nullptr)) {}

RoomCommandGateway::RoomCommandGateway(
    runtime::WorkerPool &workers,
    const GameplayTransportReadinessPort &readiness, OutboundSink outboundSink)
    : impl_(std::make_unique<Impl>(workers, &readiness, std::move(outboundSink),
                                   MovementSnapshotSink{}, CombatOutboundSink{},
                                   nullptr, nullptr, nullptr)) {}

RoomCommandGateway::RoomCommandGateway(
    runtime::WorkerPool &workers,
    const GameplayTransportReadinessPort &readiness, OutboundSink outboundSink,
    MovementSnapshotSink movementSnapshotSink)
    : impl_(std::make_unique<Impl>(workers, &readiness, std::move(outboundSink),
                                   std::move(movementSnapshotSink),
                                   CombatOutboundSink{}, nullptr, nullptr,
                                   nullptr)) {}

RoomCommandGateway::RoomCommandGateway(
    runtime::WorkerPool &workers,
    const GameplayTransportReadinessPort &readiness, OutboundSink outboundSink,
    MovementSnapshotSink movementSnapshotSink,
    CombatOutboundSink combatOutboundSink)
    : impl_(std::make_unique<Impl>(workers, &readiness, std::move(outboundSink),
                                   std::move(movementSnapshotSink),
                                   std::move(combatOutboundSink), nullptr,
                                   nullptr, nullptr)) {}

RoomCommandGateway::RoomCommandGateway(
    runtime::WorkerPool &workers,
    const GameplayTransportReadinessPort &readiness, OutboundSink outboundSink,
    MovementSnapshotSink movementSnapshotSink,
    CombatOutboundSink combatOutboundSink,
    runtime::DeadlineScheduler &deadlines)
    : impl_(std::make_unique<Impl>(workers, &readiness, std::move(outboundSink),
                                   std::move(movementSnapshotSink),
                                   std::move(combatOutboundSink), &deadlines,
                                   nullptr, nullptr)) {}

RoomCommandGateway::RoomCommandGateway(
    runtime::WorkerPool &workers,
    const GameplayTransportReadinessPort &readiness, OutboundSink outboundSink,
    MovementSnapshotSink movementSnapshotSink,
    CombatOutboundSink combatOutboundSink,
    runtime::DeadlineScheduler &deadlines,
    settlement::SettlementCapacityGate &capacityGate)
    : impl_(std::make_unique<Impl>(workers, &readiness, std::move(outboundSink),
                                   std::move(movementSnapshotSink),
                                   std::move(combatOutboundSink), &deadlines,
                                   &capacityGate, nullptr)) {}

RoomCommandGateway::RoomCommandGateway(
    runtime::WorkerPool &workers,
    const GameplayTransportReadinessPort &readiness, OutboundSink outboundSink,
    MovementSnapshotSink movementSnapshotSink,
    CombatOutboundSink combatOutboundSink,
    runtime::DeadlineScheduler &deadlines,
    settlement::SettlementCapacityGate &capacityGate,
    settlement::SettlementStoragePort &storage)
    : impl_(std::make_unique<Impl>(workers, &readiness, std::move(outboundSink),
                                   std::move(movementSnapshotSink),
                                   std::move(combatOutboundSink), &deadlines,
                                   &capacityGate, &storage)) {}

RoomCommandGateway::~RoomCommandGateway() = default;

bool RoomCommandGateway::enterLobby(const AuthenticatedRoomSession &session) {
  return impl_->state->enterLobby(session);
}

RoomSubmitResult RoomCommandGateway::submit(RoomCommandEnvelope envelope) {
  return impl_->state->submit(std::move(envelope));
}

RoomSubmitResult RoomCommandGateway::submitMovement(
    battle::MoveCommand command,
    std::chrono::steady_clock::time_point receivedAt) {
  return impl_->state->submitMovement(std::move(command), receivedAt);
}

RoomSubmitResult RoomCommandGateway::submitAttack(
    battle::AttackCommand command,
    std::chrono::steady_clock::time_point receivedAt) {
  return impl_->state->submitAttack(std::move(command), receivedAt);
}

RoomSubmitResult RoomCommandGateway::submitClaimLoot(
    battle::ClaimLootCommand command,
    std::chrono::steady_clock::time_point receivedAt) {
  return impl_->state->submitClaimLoot(std::move(command), receivedAt);
}

RoomSubmitResult
RoomCommandGateway::submitMovementTick(shared::RoomId roomId,
                                       shared::BattleInstanceId battleId,
                                       std::uint32_t serverTick) {
  return impl_->state->submitMovementTick(roomId, battleId, serverTick);
}

bool RoomCommandGateway::disconnect(shared::SessionId sessionId,
                                    shared::SessionGeneration generation) {
  return impl_->state->disconnect(sessionId, generation);
}

RoomExecutionObservation RoomCommandGateway::observation() const {
  return impl_->state->observation();
}

} // namespace lol::game_flow
