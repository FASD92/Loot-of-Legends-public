#include <lol/battle/BattleLoadApi.hpp>
#include <lol/battle_continuity/Durability.hpp>
#include <lol/battle_continuity/FlightRecorder.hpp>
#include <lol/game_flow/RoomCommandGateway.hpp>
#include <lol/lobby_room/RoomApi.hpp>
#include <lol/runtime/DeadlineScheduler.hpp>
#include <lol/runtime/WorkerPool.hpp>
#include <lol/settlement/SettlementCapacityGate.hpp>
#include <lol/settlement/SettlementPublication.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

lol::shared::AccountId account(std::uint8_t suffix) {
  lol::shared::AccountId::Bytes bytes{};
  bytes.back() = suffix;
  return lol::shared::AccountId{bytes};
}

class AlwaysReady final
    : public lol::game_flow::GameplayTransportReadinessPort {
public:
  bool isReady(lol::shared::SessionId,
               lol::shared::SessionGeneration) const noexcept override {
    return true;
  }
};

class UnusedSettlementStorage final
    : public lol::settlement::SettlementStoragePort {
public:
  lol::settlement::SubmitAppendResult
  submit(lol::settlement::DurableAppendRequest, CompletionSink) override {
    return lol::settlement::SubmitAppendResult::Accepted;
  }
};

class UnusedContinuityStorage final
    : public lol::battle_continuity::DurableTickWritePort {
public:
  lol::battle_continuity::DurableTickSubmitResult
  submit(lol::battle_continuity::DurableTickWriteRequest,
         CompletionSink) override {
    return lol::battle_continuity::DurableTickSubmitResult::Accepted;
  }
};

std::optional<lol::game_flow::RecoveredBattleInstall> recoveredLoadBarrier(
    std::uint64_t roomValue, std::uint64_t firstSessionId,
    std::uint64_t nextBattleOrdinal = 2U) {
  const auto roomId = lol::shared::RoomId{roomValue};
  const auto battleId = lol::shared::BattleInstanceId{1};
  auto created =
      lol::lobby_room::Room::create(lol::lobby_room::CreateRoomCommand{
          .roomId = roomId,
          .title = "restored",
          .capacity = 2,
          .creator =
              lol::lobby_room::RoomMemberIdentity{
                  .accountId = account(1),
                  .sessionId = lol::shared::SessionId{firstSessionId},
                  .generation = lol::shared::SessionGeneration{11},
                  .nickname = "one",
              },
      });
  if (!created.room.has_value() ||
      created.room->join(
          lol::lobby_room::JoinRoomCommand{lol::lobby_room::RoomMemberIdentity{
              .accountId = account(2),
              .sessionId = lol::shared::SessionId{firstSessionId + 1U},
              .generation = lol::shared::SessionGeneration{12},
              .nickname = "two",
          }}) != lol::lobby_room::RoomResultCode::Ok ||
      created.room->setReady(lol::lobby_room::SetReadyCommand{
          lol::shared::SessionId{firstSessionId},
          lol::shared::SessionGeneration{11}, true}) !=
          lol::lobby_room::RoomResultCode::Ok ||
      created.room->setReady(lol::lobby_room::SetReadyCommand{
          lol::shared::SessionId{firstSessionId + 1U},
          lol::shared::SessionGeneration{12}, true}) !=
          lol::lobby_room::RoomResultCode::Ok) {
    return std::nullopt;
  }
  const auto admission = created.room->prepareHostStart(
      lol::lobby_room::HostStartEligibilityCommand{
          .actorSessionId = lol::shared::SessionId{firstSessionId},
          .actorGeneration = lol::shared::SessionGeneration{11},
      });
  if (!admission.admission.has_value() ||
      created.room->commitLoading(*admission.admission) !=
          lol::lobby_room::RoomResultCode::Ok) {
    return std::nullopt;
  }

  std::vector<lol::battle::BattleStartCandidate> candidates;
  for (const auto &member : admission.admission->members) {
    candidates.push_back(lol::battle::BattleStartCandidate{
        .accountId = member.accountId,
        .sessionId = member.sessionId,
        .generation = member.sessionGeneration,
        .nickname = member.nickname,
    });
  }
  auto battleCreated =
      lol::battle::BattleInstance::create(lol::battle::BattleAdmissionSnapshot{
          .roomId = roomId,
          .battleId = battleId,
          .candidates = std::move(candidates),
          .rulesetVersion = lol::battle::battleRulesetVersion,
          .seed = 700U + roomValue,
      });
  if (!battleCreated.battle.has_value() ||
      battleCreated.battle->openLoadBarrier(lol::battle::BattleTime{}) !=
          lol::battle::BattleLoadResultCode::Ok) {
    return std::nullopt;
  }
  auto battle = std::move(*battleCreated.battle);
  const lol::battle_continuity::RoomRecoveryState roomState{
      .roomId = roomId,
      .capacity = 2U,
      .hostParticipantSlot = 1U,
      .memberSlots = {1U, 2U},
      .phase = lol::battle_continuity::RoomRecoveryPhase::Loading,
      .nextBattleOrdinal = 2U,
  };
  auto initial = lol::battle_continuity::BattleRecording::start(
      battle,
      lol::battle_continuity::BattleIdentity{
          .originRecoveryEpoch = static_cast<std::uint32_t>(roomValue >> 32U),
          .roomId = roomId,
          .battleInstanceId = battleId,
      },
      4U, roomState);
  if (!initial.has_value() || !initial->takePendingBatch().has_value()) {
    return std::nullopt;
  }
  const std::vector<lol::battle_continuity::Record> records{
      initial->records().begin(), initial->records().end()};
  auto recording =
      lol::battle_continuity::BattleRecording::resume(battle, records, 5U);
  if (!recording.has_value()) {
    return std::nullopt;
  }
  return lol::game_flow::RecoveredBattleInstall{
      .room = std::move(*created.room),
      .battle = std::move(battle),
      .recording = std::move(*recording),
      .nextBattleOrdinal = nextBattleOrdinal,
      .settlementBatch = std::nullopt,
  };
}

bool installBindsRecoveredRoutesAndRejectsConflictsAtomically() {
  constexpr std::uint64_t roomValue = (4ULL << 32U) | 1ULL;
  lol::runtime::WorkerPool workers{
      lol::runtime::WorkerPoolConfig{.threadCount = 1, .queueCapacity = 32}};
  lol::runtime::ThreadDeadlineScheduler deadlines{
      lol::runtime::ThreadDeadlineSchedulerConfig{.queueCapacity = 32}};
  AlwaysReady readiness;
  lol::settlement::SettlementCapacityGate capacity;
  capacity.updateBacklog(lol::settlement::OutboxBacklogSnapshot{
      .unretiredRecords = 0U,
      .unretiredBytes = 0U,
      .oldestPendingAge = 0ms,
      .storageHealthy = true,
  });
  UnusedSettlementStorage settlement;
  UnusedContinuityStorage continuity;
  lol::game_flow::RoomCommandGateway gateway{
      workers,
      readiness,
      [](lol::game_flow::LobbyRoomOutboundIntent) {},
      {},
      {},
      deadlines,
      capacity,
      settlement,
      continuity,
      lol::shared::RoomId{(5ULL << 32U) | 1ULL},
  };

  auto recovered = recoveredLoadBarrier(roomValue, 101U);
  auto conflict = recoveredLoadBarrier((4ULL << 32U) | 2ULL, 101U);
  if (!recovered.has_value() || !conflict.has_value() ||
      !gateway.installRecoveredBattle(std::move(*recovered)) ||
      gateway.installRecoveredBattle(std::move(*conflict))) {
    return false;
  }
  const auto journal = gateway.battleJournal(lol::shared::RoomId{roomValue});
  return journal.has_value() && !journal->empty() &&
         gateway.resumeBattleInput(lol::shared::SessionId{101},
                                   lol::shared::SessionGeneration{11}) ==
             lol::game_flow::RoomSubmitResult::RoomOverloaded &&
         gateway.activateRecoveredBattles() &&
         gateway.resumeBattleInput(lol::shared::SessionId{101},
                                   lol::shared::SessionGeneration{11}) ==
             lol::game_flow::RoomSubmitResult::Accepted &&
         workers.waitUntilIdle(2s);
}

bool installRejectsStalePersistedNextBattleOrdinal() {
  constexpr std::uint64_t roomValue = (6ULL << 32U) | 1ULL;
  lol::runtime::WorkerPool workers{
      lol::runtime::WorkerPoolConfig{.threadCount = 1, .queueCapacity = 32}};
  lol::runtime::ThreadDeadlineScheduler deadlines{
      lol::runtime::ThreadDeadlineSchedulerConfig{.queueCapacity = 32}};
  AlwaysReady readiness;
  lol::settlement::SettlementCapacityGate capacity;
  capacity.updateBacklog(lol::settlement::OutboxBacklogSnapshot{
      .unretiredRecords = 0U,
      .unretiredBytes = 0U,
      .oldestPendingAge = 0ms,
      .storageHealthy = true,
  });
  UnusedSettlementStorage settlement;
  UnusedContinuityStorage continuity;
  lol::game_flow::RoomCommandGateway gateway{
      workers,
      readiness,
      [](lol::game_flow::LobbyRoomOutboundIntent) {},
      {},
      {},
      deadlines,
      capacity,
      settlement,
      continuity,
      lol::shared::RoomId{(7ULL << 32U) | 1ULL},
  };
  auto stale = recoveredLoadBarrier(roomValue, 201U, 1U);
  auto valid = recoveredLoadBarrier(roomValue, 201U, 2U);
  return stale.has_value() && valid.has_value() &&
         !gateway.installRecoveredBattle(std::move(*stale)) &&
         gateway.installRecoveredBattle(std::move(*valid)) &&
         gateway.activateRecoveredBattles() && workers.waitUntilIdle(2s);
}

} // namespace

int main() {
  return installBindsRecoveredRoutesAndRejectsConflictsAtomically() &&
                 installRejectsStalePersistedNextBattleOrdinal()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
