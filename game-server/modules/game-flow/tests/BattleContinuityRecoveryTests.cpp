#include <lol/battle/BattleLoadApi.hpp>
#include <lol/battle_continuity/BattleReplay.hpp>
#include <lol/battle_continuity/BattleStateCodec.hpp>
#include <lol/battle_continuity/FlightRecorder.hpp>
#include <lol/battle_continuity/RecoveryEnvelopeCodec.hpp>
#include <lol/game_flow/BattleContinuityRecovery.hpp>
#include <lol/game_flow/RoomCommandGateway.hpp>
#include <lol/lobby_room/RoomApi.hpp>
#include <lol/settlement/SettlementIntent.hpp>

#include "workflows/BattleTerminalWorkflow.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <optional>
#include <memory>
#include <utility>
#include <vector>

namespace {

using namespace lol;
using namespace std::chrono_literals;
using battle::ArenaLoadCompleteCommand;
using battle::BattleAdmissionSnapshot;
using battle::BattleInstance;
using battle::BattleLoadResultCode;
using battle::BattleTime;
using battle::CandidateDisconnectedCommand;
using battle_continuity::BattleIdentity;
using battle_continuity::BattleRecording;
using battle_continuity::Bytes;
using battle_continuity::CanonicalCommand;
using battle_continuity::Record;
using battle_continuity::RecordType;
using battle_continuity::SettlementReceipt;
using game_flow::BattleContinuityRecoveryCode;
using game_flow::RecoveredSessionGeneration;
using lobby_room::Room;
using settlement::ResultCommittedAt;
using shared::AccountId;
using shared::BattleInstanceId;
using shared::RoomId;
using shared::SessionGeneration;
using shared::SessionId;

constexpr std::uint32_t kOriginEpoch = 9U;
constexpr std::uint64_t kRoomValue =
    (static_cast<std::uint64_t>(kOriginEpoch) << 32U) | 1U;
const RoomId kRoom{kRoomValue};
const BattleInstanceId kBattle{17U};

AccountId account(std::uint8_t suffix) {
  AccountId::Bytes bytes{};
  bytes.back() = suffix;
  return AccountId{bytes};
}

BattleIdentity identity(BattleInstanceId battleId) {
  return BattleIdentity{.originRecoveryEpoch = kOriginEpoch,
                        .roomId = kRoom,
                        .battleInstanceId = battleId};
}

BattleIdentity identity() { return identity(kBattle); }

struct ParticipantSeed final {
  std::uint16_t slot;
  AccountId accountId;
  SessionId sessionId;
  SessionGeneration generation;
  const char *nickname;
};

std::vector<std::uint16_t> slots(std::size_t count) {
  std::vector<std::uint16_t> result;
  result.reserve(count);
  for (std::size_t index = 0U; index < count; ++index) {
    result.push_back(static_cast<std::uint16_t>(index + 1U));
  }
  return result;
}

battle_continuity::RoomRecoveryState roomState(
    const BattleIdentity &battleIdentity, std::size_t capacity,
    battle_continuity::RoomRecoveryPhase phase,
    std::vector<std::uint16_t> memberSlots) {
  return battle_continuity::RoomRecoveryState{
      .roomId = battleIdentity.roomId,
      .capacity = static_cast<std::uint8_t>(capacity),
      .hostParticipantSlot = memberSlots.empty()
                                 ? static_cast<std::uint16_t>(0U)
                                 : memberSlots.front(),
      .memberSlots = std::move(memberSlots),
      .phase = phase,
      .nextBattleOrdinal = battleIdentity.battleInstanceId.value() + 1U,
  };
}

battle_continuity::RoomRecoveryPhase roomPhase(
    battle::BattleLoadState state) {
  return state == battle::BattleLoadState::LoadBarrierOpen
             ? battle_continuity::RoomRecoveryPhase::Loading
             : battle_continuity::RoomRecoveryPhase::InProgress;
}

std::vector<ParticipantSeed> seeds(std::size_t count) {
  const std::array<ParticipantSeed, 3> all{
      ParticipantSeed{1U, account(1U), SessionId{101U}, SessionGeneration{3U},
                      "one"},
      ParticipantSeed{2U, account(2U), SessionId{202U}, SessionGeneration{4U},
                      "two"},
      ParticipantSeed{3U, account(3U), SessionId{303U}, SessionGeneration{5U},
                      "three"},
  };
  return {all.begin(), all.begin() + static_cast<std::ptrdiff_t>(count)};
}

std::optional<BattleInstance> createBattle(
    const std::vector<ParticipantSeed> &participants,
    const BattleIdentity &battleIdentity) {
  std::vector<battle::BattleStartCandidate> candidates;
  candidates.reserve(participants.size());
  for (const auto &participant : participants) {
    candidates.push_back(battle::BattleStartCandidate{
        .accountId = participant.accountId,
        .sessionId = participant.sessionId,
        .generation = participant.generation,
        .nickname = participant.nickname,
    });
  }
  auto created = BattleInstance::create(BattleAdmissionSnapshot{
      .roomId = battleIdentity.roomId,
      .battleId = battleIdentity.battleInstanceId,
      .candidates = std::move(candidates),
      .rulesetVersion = battle::battleRulesetVersion,
      .seed = 0x1234U,
  });
  if (created.code != BattleLoadResultCode::Ok || !created.battle.has_value() ||
      created.battle->openLoadBarrier(BattleTime::fromLogicalTick(0U)) !=
          BattleLoadResultCode::Ok) {
    return std::nullopt;
  }
  return std::move(*created.battle);
}

bool recordLoad(BattleInstance &battle, BattleRecording &recording,
                const std::vector<ParticipantSeed> &participants,
                const ParticipantSeed &participant, std::uint64_t tick,
                const BattleIdentity &battleIdentity = identity()) {
  const auto before = battle.exportDeterministicState();
  if (battle.completeLoad(
          ArenaLoadCompleteCommand{
              .sessionId = participant.sessionId,
              .generation = participant.generation,
              .roomId = battleIdentity.roomId,
              .battleId = battleIdentity.battleInstanceId,
          },
          true,
          BattleTime::fromLogicalTick(tick)) != BattleLoadResultCode::Ok ||
      !recording.recordDecision(
          CanonicalCommand::arenaLoadComplete(participant.slot), before,
          roomState(battleIdentity, participants.size(), roomPhase(before.state),
                    slots(participants.size())),
          battle,
          roomState(battleIdentity, participants.size(),
                    roomPhase(battle.projection().state), slots(participants.size())),
          static_cast<std::uint16_t>(BattleLoadResultCode::Ok), std::nullopt) ||
      !recording.takePendingBatch().has_value()) {
    return false;
  }
  return true;
}

std::optional<lobby_room::Room>
inProgressRoom(const std::vector<ParticipantSeed> &participants) {
  auto created = Room::create(lobby_room::CreateRoomCommand{
      .roomId = kRoom,
      .title = "recovered",
      .capacity = static_cast<std::uint8_t>(participants.size()),
      .creator = lobby_room::RoomMemberIdentity{participants.front().accountId,
                                                participants.front().sessionId,
                                                participants.front().generation,
                                                participants.front().nickname},
  });
  if (!created.room.has_value()) {
    return std::nullopt;
  }
  auto room = std::move(*created.room);
  for (std::size_t index = 1U; index < participants.size(); ++index) {
    const auto &participant = participants[index];
    if (room.join(lobby_room::JoinRoomCommand{lobby_room::RoomMemberIdentity{
            participant.accountId, participant.sessionId,
            participant.generation, participant.nickname}}) !=
            lobby_room::RoomResultCode::Ok ||
        room.setReady(lobby_room::SetReadyCommand{
            participant.sessionId, participant.generation, true}) !=
            lobby_room::RoomResultCode::Ok) {
      return std::nullopt;
    }
  }
  if (room.setReady(lobby_room::SetReadyCommand{
          participants.front().sessionId, participants.front().generation,
          true}) != lobby_room::RoomResultCode::Ok) {
    return std::nullopt;
  }
  const auto eligibility = room.prepareHostStart({
      .actorSessionId = participants.front().sessionId,
      .actorGeneration = participants.front().generation,
  });
  if (!eligibility.admission.has_value() ||
      room.commitLoading(*eligibility.admission) !=
          lobby_room::RoomResultCode::Ok ||
      room.commitInProgress() != lobby_room::RoomResultCode::Ok) {
    return std::nullopt;
  }
  return room;
}

battle_continuity::BattleRecoveryEnvelope envelopeFor(
    const std::vector<ParticipantSeed> &participants,
    const BattleIdentity &battleIdentity = identity()) {
  std::vector<battle_continuity::RecoveryParticipant> values;
  values.reserve(participants.size());
  for (const auto &participant : participants) {
    values.push_back(battle_continuity::RecoveryParticipant{
        .participantSlot = participant.slot,
        .accountId = participant.accountId,
        .sessionId = participant.sessionId,
        .previousGeneration = participant.generation,
        .nickname = participant.nickname,
    });
  }
  return battle_continuity::BattleRecoveryEnvelope{
      .schemaVersion = battle_continuity::kRecoveryEnvelopeSchemaVersion,
      .identity = battleIdentity,
      .roomTitle = "recovered",
      .capacity = static_cast<std::uint8_t>(participants.size()),
      .hostParticipantSlot = 1U,
      .participants = std::move(values),
  };
}

std::vector<RecoveredSessionGeneration>
freshGenerations(const std::vector<ParticipantSeed> &participants) {
  std::vector<RecoveredSessionGeneration> values;
  values.reserve(participants.size());
  for (const auto &participant : participants) {
    values.push_back(RecoveredSessionGeneration{
        .participantSlot = participant.slot,
        .generation = SessionGeneration{participant.generation.value() + 20U},
    });
  }
  return values;
}

std::optional<Bytes> encodeRecords(const std::vector<Record> &records) {
  Bytes bytes;
  for (const auto &record : records) {
    const auto encoded = battle_continuity::encodeRecord(record);
    if (!encoded.ok()) {
      return std::nullopt;
    }
    bytes.insert(bytes.end(), encoded.bytes.begin(), encoded.bytes.end());
  }
  return bytes;
}

std::optional<Bytes> encodeEnvelope(
    const battle_continuity::BattleRecoveryEnvelope &envelope) {
  const auto encoded = battle_continuity::encodeBattleRecoveryEnvelope(envelope);
  return encoded.ok() ? std::optional{encoded.bytes} : std::nullopt;
}

std::optional<Bytes> departedJournal(const BattleIdentity &battleIdentity = identity());
std::optional<Bytes> terminalJournal(bool cancelBattle,
                                     const BattleIdentity &battleIdentity = identity());

std::optional<game_flow::StoredBattleRecovery> storedBattle(
    const BattleIdentity &battleIdentity, std::optional<Bytes> journal,
    const std::vector<ParticipantSeed> &participants,
    bool includeEnvelope = true) {
  if (!journal.has_value()) {
    return std::nullopt;
  }
  std::optional<Bytes> privateEnvelope;
  if (includeEnvelope) {
    privateEnvelope = encodeEnvelope(envelopeFor(participants, battleIdentity));
    if (!privateEnvelope.has_value()) {
      return std::nullopt;
    }
  }
  return game_flow::StoredBattleRecovery{
      .identity = battleIdentity,
      .committedJournal = std::move(*journal),
      .privateEnvelope = std::move(privateEnvelope),
  };
}

class StartupSettlementStorage final : public settlement::SettlementStoragePort {
public:
  explicit StartupSettlementStorage(bool fail) : fail_(fail) {}

  ~StartupSettlementStorage() override {
    if (completion_.valid()) {
      completion_.wait();
    }
  }

  settlement::SubmitAppendResult submit(
      settlement::DurableAppendRequest request,
      CompletionSink completion) override {
    ++submitCount;
    if (fail_) {
      return settlement::SubmitAppendResult::QueueFull;
    }
    completion_ = std::async(
        std::launch::async,
        [request = std::move(request), completion = std::move(completion)]() mutable {
          completion(settlement::DurableAppendCompleted{
              .batchId = request.batchId,
              .roomId = request.roomId,
              .battleId = request.battleId,
              .commitSequence = 1U,
          });
        });
    return settlement::SubmitAppendResult::Accepted;
  }

  std::size_t submitCount{};

private:
  bool fail_;
  std::future<void> completion_;
};

struct StartupRecoveryObservation final {
  std::vector<game_flow::BattleRecoveryRequest> disposed;
  std::size_t installed{};
  bool durable{};
  bool installResult{true};
  bool disposeResult{true};
};

bool runStartupRecovery(
    std::vector<game_flow::StoredBattleRecovery> battles,
    StartupSettlementStorage &storage, StartupRecoveryObservation &observed,
    bool installResult = true) {
  session::SessionRegistry sessions{SessionGeneration{1U}};
  observed.installResult = installResult;
  const auto result = game_flow::recoverBattlesAtStartup(
      game_flow::StartupRecoveryInput{
          .battles = std::move(battles),
          .currentWriterRecoveryEpoch = 3U,
          .reconnectExpiresAt = std::chrono::steady_clock::now() + 30s,
          .sessions = sessions,
          .settlementStorage = storage,
          .dispose = [&observed](game_flow::BattleRecoveryRequest request) {
            observed.disposed.push_back(std::move(request));
            return observed.disposeResult;
          },
          .settlementDurable = [&observed](
                                  const settlement::SettlementIntentBatch &) {
            return observed.durable;
          },
          .install = [&observed](game_flow::StartupRecoveredBattleInstall) {
            ++observed.installed;
            return observed.installResult;
          },
      });
  return result;
}

bool startupArbitratesNewestBattleBeforeInstalling() {
  const auto participants = seeds(3U);
  const auto olderIdentity = identity(BattleInstanceId{17U});
  const auto newestIdentity = identity(BattleInstanceId{18U});
  auto older = storedBattle(olderIdentity, terminalJournal(true, olderIdentity),
                            seeds(2U));
  auto newest = storedBattle(newestIdentity,
                             departedJournal(newestIdentity), participants);
  if (!older.has_value() || !newest.has_value()) {
    return false;
  }
  StartupSettlementStorage storage{false};
  StartupRecoveryObservation observed;
  if (!runStartupRecovery({std::move(*newest), std::move(*older)}, storage,
                          observed) ||
      observed.installed != 1U || observed.disposed.size() != 1U ||
      observed.disposed.front().stored.identity != olderIdentity ||
      observed.disposed.front().disposition !=
          game_flow::BattleRecoveryDisposition::Retire) {
    return false;
  }
  return storage.submitCount == 0U;
}

bool startupQuarantinesOlderNonterminalBattle() {
  const auto olderIdentity = identity(BattleInstanceId{17U});
  const auto newestIdentity = identity(BattleInstanceId{18U});
  auto older = storedBattle(olderIdentity, departedJournal(olderIdentity),
                            seeds(3U));
  auto newest = storedBattle(newestIdentity,
                             departedJournal(newestIdentity), seeds(3U));
  if (!older.has_value() || !newest.has_value()) {
    return false;
  }
  StartupSettlementStorage storage{false};
  StartupRecoveryObservation observed;
  if (!runStartupRecovery({std::move(*older), std::move(*newest)}, storage,
                          observed) ||
      observed.installed != 1U || observed.disposed.size() != 1U) {
    return false;
  }
  return observed.disposed.front().stored.identity == olderIdentity &&
         observed.disposed.front().disposition ==
             game_flow::BattleRecoveryDisposition::Quarantine;
}

bool malformedEnvelopeDoesNotInstallSessions() {
  const auto battleIdentity = identity();
  auto stored = storedBattle(battleIdentity, departedJournal(), seeds(3U), false);
  if (!stored.has_value()) {
    return false;
  }
  stored->privateEnvelope = Bytes{0xffU};
  StartupSettlementStorage storage{false};
  StartupRecoveryObservation observed;
  return runStartupRecovery({std::move(*stored)}, storage, observed) &&
         observed.installed == 0U && observed.disposed.size() == 1U &&
         observed.disposed.front().disposition ==
             game_flow::BattleRecoveryDisposition::Quarantine &&
         storage.submitCount == 0U;
}

bool installerFailureRollsBackAllSessions() {
  auto stored = storedBattle(identity(), departedJournal(), seeds(3U));
  if (!stored.has_value()) {
    return false;
  }
  StartupSettlementStorage storage{false};
  StartupRecoveryObservation observed;
  return !runStartupRecovery({std::move(*stored)}, storage, observed, false) &&
         observed.installed == 1U && observed.disposed.size() == 1U &&
         observed.disposed.front().disposition ==
             game_flow::BattleRecoveryDisposition::Quarantine &&
         storage.submitCount == 0U;
}

bool headlessAlreadyDurableSkipsAppendAndRetires() {
  auto stored = storedBattle(identity(), terminalJournal(true), seeds(2U));
  if (!stored.has_value()) {
    return false;
  }
  StartupSettlementStorage storage{false};
  StartupRecoveryObservation observed;
  observed.durable = true;
  return runStartupRecovery({std::move(*stored)}, storage, observed) &&
         observed.installed == 0U && observed.disposed.size() == 1U &&
         observed.disposed.front().disposition ==
             game_flow::BattleRecoveryDisposition::Retire &&
         storage.submitCount == 0U;
}

bool headlessNotDurableAppendsOnceThenRetires() {
  auto stored = storedBattle(identity(), terminalJournal(true), seeds(2U));
  if (!stored.has_value()) {
    return false;
  }
  StartupSettlementStorage storage{false};
  StartupRecoveryObservation observed;
  return runStartupRecovery({std::move(*stored)}, storage, observed) &&
         observed.installed == 0U && observed.disposed.size() == 1U &&
         observed.disposed.front().disposition ==
             game_flow::BattleRecoveryDisposition::Retire &&
         storage.submitCount == 1U;
}

bool appendFailureLeavesBattleUnretired() {
  auto stored = storedBattle(identity(), terminalJournal(true), seeds(2U));
  if (!stored.has_value()) {
    return false;
  }
  StartupSettlementStorage storage{true};
  StartupRecoveryObservation observed;
  return !runStartupRecovery({std::move(*stored)}, storage, observed) &&
         observed.installed == 0U && observed.disposed.empty() &&
         storage.submitCount == 1U;
}

bool duplicateTerminalStartupRecoveryDoesNotAppendTwice() {
  auto stored = storedBattle(identity(), terminalJournal(true), seeds(2U));
  if (!stored.has_value()) {
    return false;
  }

  StartupSettlementStorage storage{false};
  StartupRecoveryObservation first;
  first.disposeResult = false;
  if (runStartupRecovery({*stored}, storage, first) ||
      first.installed != 0U || first.disposed.size() != 1U ||
      first.disposed.front().disposition !=
          game_flow::BattleRecoveryDisposition::Retire ||
      storage.submitCount != 1U) {
    return false;
  }

  StartupRecoveryObservation second;
  second.durable = true;
  return runStartupRecovery({std::move(*stored)}, storage, second) &&
         second.installed == 0U && second.disposed.size() == 1U &&
         second.disposed.front().disposition ==
             game_flow::BattleRecoveryDisposition::Retire &&
         storage.submitCount == 1U;
}

std::optional<Bytes> departedJournal(const BattleIdentity &battleIdentity) {
  const auto participants = seeds(3U);
  auto battle = createBattle(participants, battleIdentity);
  if (!battle.has_value()) {
    std::cerr << "departed create failed\n";
    return std::nullopt;
  }
  auto recording = BattleRecording::start(
      *battle, battleIdentity, 2U,
      roomState(battleIdentity, participants.size(),
                battle_continuity::RoomRecoveryPhase::Loading,
                slots(participants.size())));
  if (!recording.has_value() || !recording->takePendingBatch().has_value() ||
      !recordLoad(*battle, *recording, participants, participants[0], 1U,
                  battleIdentity) ||
      !recordLoad(*battle, *recording, participants, participants[1], 1U,
                  battleIdentity)) {
    std::cerr << "departed setup failed recording=" << recording.has_value()
              << '\n';
    return std::nullopt;
  }
  const auto before = battle->exportDeterministicState();
  const auto disconnected = battle->disconnect(
          CandidateDisconnectedCommand{
              .sessionId = participants[2].sessionId,
              .generation = participants[2].generation,
              .roomId = battleIdentity.roomId,
              .battleId = battleIdentity.battleInstanceId,
              },
          BattleTime::fromLogicalTick(2U)) == BattleLoadResultCode::Ok;
  const auto decision = disconnected && recording->recordDecision(
          CanonicalCommand::participantExit(3U, false), before,
          roomState(battleIdentity, participants.size(),
                    roomPhase(before.state),
                    slots(participants.size())),
          *battle,
          roomState(battleIdentity, participants.size(),
                    battle_continuity::RoomRecoveryPhase::InProgress,
                    std::vector<std::uint16_t>{1U, 2U}),
          static_cast<std::uint16_t>(BattleLoadResultCode::Ok), std::nullopt);
  const auto pending = decision && recording->takePendingBatch().has_value();
  const auto committed =
      battle->projection().state == battle::BattleLoadState::GameplayCommitted;
  if (!disconnected || !decision || !pending || !committed) {
    std::cerr << "departed exit failed disconnected=" << disconnected
              << " decision=" << decision << " pending=" << pending
              << " state="
              << static_cast<unsigned int>(battle->projection().state) << '\n';
    return std::nullopt;
  }
  const auto encoded = recording->encodeJournal();
  return encoded.ok() ? std::optional{encoded.bytes} : std::nullopt;
}

std::optional<Bytes> terminalJournal(bool cancelBattle,
                                     const BattleIdentity &battleIdentity) {
  const auto participants = seeds(2U);
  auto battle = createBattle(participants, battleIdentity);
  auto room = inProgressRoom(participants);
  if (!battle.has_value() || !room.has_value()) {
    return std::nullopt;
  }
  auto recording = BattleRecording::start(
      *battle, battleIdentity, 2U,
      roomState(battleIdentity, participants.size(),
                battle_continuity::RoomRecoveryPhase::Loading,
                slots(participants.size())));
  if (!recording.has_value() || !recording->takePendingBatch().has_value() ||
      !recordLoad(*battle, *recording, participants, participants[0], 1U,
                  battleIdentity) ||
      !recordLoad(*battle, *recording, participants, participants[1], 2U,
                  battleIdentity)) {
    return std::nullopt;
  }

  auto before = battle->exportDeterministicState();
  const auto at = BattleTime::fromLogicalTick(cancelBattle ? 4U : 602U);
  if (cancelBattle) {
    if (battle->disconnect(
            CandidateDisconnectedCommand{.sessionId = participants[0].sessionId,
                                         .generation =
                                             participants[0].generation,
                                         .roomId = battleIdentity.roomId,
                                         .battleId = battleIdentity.battleInstanceId},
            BattleTime::fromLogicalTick(3U)) != BattleLoadResultCode::Ok ||
        !recording->recordDecision(
            CanonicalCommand::participantExit(1U, false), before,
            roomState(battleIdentity, participants.size(),
                      battle_continuity::RoomRecoveryPhase::InProgress,
                      std::vector<std::uint16_t>{1U, 2U}),
            *battle,
            roomState(battleIdentity, participants.size(),
                      battle_continuity::RoomRecoveryPhase::InProgress,
                      std::vector<std::uint16_t>{2U}),
            static_cast<std::uint16_t>(BattleLoadResultCode::Ok),
            std::nullopt) ||
        !recording->takePendingBatch().has_value()) {
      return std::nullopt;
    }
    before = battle->exportDeterministicState();
    if (battle->disconnect(
            CandidateDisconnectedCommand{.sessionId = participants[1].sessionId,
                                         .generation =
                                             participants[1].generation,
                                         .roomId = battleIdentity.roomId,
                                         .battleId = battleIdentity.battleInstanceId},
            at) != BattleLoadResultCode::Ok) {
      return std::nullopt;
    }
  } else if (battle->expireCombat(
            battle::CombatDeadlineCommand{.battleId = battleIdentity.battleInstanceId}, at) !=
             battle::CombatDeadlineResultCode::Ok) {
    return std::nullopt;
  }
  const ResultCommittedAt committedAt{
      .unixEpochMilliseconds = 1'700'000'000'123ULL,
      .monotonicNanoseconds = at.battleElapsedNanos};
  auto batch = game_flow::workflows::createSettlementIntentBatchForTerminal(
      *battle, committedAt);
  if (!batch.has_value()) {
    return std::nullopt;
  }
  std::vector<SettlementReceipt> receipts;
  for (const auto &intent : batch->intents()) {
    const auto participant = std::find_if(
        participants.begin(), participants.end(), [&intent](const auto &value) {
          return value.accountId == intent.accountId();
        });
    if (participant == participants.end()) {
      return std::nullopt;
    }
    receipts.push_back(SettlementReceipt{
        .participantSlot = participant->slot,
        .settlementId = intent.id().bytes(),
        .settlementPayloadHash = intent.canonicalHash().bytes(),
    });
  }
  std::ranges::sort(receipts, {}, &SettlementReceipt::participantSlot);
  if (!recording->recordDecision(
          cancelBattle ? CanonicalCommand::participantExit(2U, false)
                       : CanonicalCommand::combatDeadline(),
          before,
          cancelBattle
              ? roomState(battleIdentity, participants.size(),
                          battle_continuity::RoomRecoveryPhase::InProgress,
                          std::vector<std::uint16_t>{2U})
              : roomState(battleIdentity, participants.size(),
                          battle_continuity::RoomRecoveryPhase::InProgress,
                          slots(participants.size())),
          *battle,
          cancelBattle
              ? roomState(battleIdentity, participants.size(),
                          battle_continuity::RoomRecoveryPhase::AwaitingSettlementDurability,
                          {})
              : roomState(battleIdentity, participants.size(),
                          battle_continuity::RoomRecoveryPhase::AwaitingSettlementDurability,
                          slots(participants.size())),
          static_cast<std::uint16_t>(
              cancelBattle
                  ? static_cast<std::uint16_t>(BattleLoadResultCode::Ok)
                  : static_cast<std::uint16_t>(
                        battle::CombatDeadlineResultCode::Ok)),
          battle_continuity::TerminalRecording{
              .terminalReason = static_cast<std::uint16_t>(
                  battle->resultProjection().result->outcome),
              .resultCommittedUnixEpochMilliseconds =
                  committedAt.unixEpochMilliseconds,
              .settlementBatchId = batch->id().bytes(),
              .settlements = std::move(receipts),
          }) ||
      !recording->takePendingBatch().has_value()) {
    return std::nullopt;
  }
  const auto encoded = recording->encodeJournal();
  return encoded.ok() ? std::optional{encoded.bytes} : std::nullopt;
}

bool rejectsUnsupportedState() {
  const auto empty = Bytes{};
  const auto envelope = envelopeFor(seeds(2U));
  const auto generations = freshGenerations(seeds(2U));
  const auto result =
      game_flow::reconstructBattle(empty, envelope, generations, 3U);
  return !result.ok() && result.error.has_value() &&
         result.error->code == BattleContinuityRecoveryCode::JournalRejected;
}

bool departedParticipantIsNotRestoredToRoom() {
  const auto participants = seeds(3U);
  const auto journal = departedJournal();
  const auto envelope = envelopeFor(participants);
  const auto generations = freshGenerations(participants);
  if (!journal.has_value()) {
    return false;
  }
  const auto result =
      game_flow::reconstructBattle(*journal, envelope, generations, 3U);
  if (!result.ok() || !result.recovered.has_value() ||
      !result.recovered->room.has_value() ||
      result.recovered->activeParticipants.size() != 2U) {
    std::cerr << "departed result ok=" << result.ok()
              << " recovered=" << result.recovered.has_value()
              << " room=" << (result.recovered.has_value() &&
                                       result.recovered->room.has_value())
              << " active=" << (result.recovered.has_value()
                                      ? result.recovered->activeParticipants.size()
                                      : 0U);
    if (result.error.has_value()) {
      std::cerr << " error=" << static_cast<unsigned int>(result.error->code);
    }
    std::cerr << '\n';
    return false;
  }
  const auto detail = result.recovered->room->detail();
  if (!detail.has_value() || detail->members.size() != 2U ||
      std::ranges::any_of(detail->members, [](const auto &member) {
        return member.sessionId == SessionId{303U};
      })) {
    return false;
  }
  const auto projection = result.recovered->battle.projection();
  return projection.state == battle::BattleLoadState::GameplayCommitted &&
         projection.capturedParticipants.size() == 2U &&
         projection.candidates.size() == 3U;
}

bool terminalReceiptMustMatchRegeneratedSettlement() {
  const auto participants = seeds(2U);
  const auto journal = terminalJournal(false);
  const auto envelope = envelopeFor(participants);
  const auto generations = freshGenerations(participants);
  if (!journal.has_value()) {
    return false;
  }
  const auto valid =
      game_flow::reconstructBattle(*journal, envelope, generations, 3U);
  if (!valid.ok() || !valid.recovered.has_value() ||
      !valid.recovered->room.has_value() ||
      !valid.recovered->settlementBatch.has_value() ||
      valid.recovered->room->lifecycle() !=
          lobby_room::RoomLifecycle::AwaitingSettlementDurability) {
    return false;
  }

  const auto decoded = battle_continuity::decodeJournal(*journal);
  if (!decoded.ok()) {
    return false;
  }
  auto records = decoded.records;
  auto terminal =
      std::find_if(records.begin(), records.end(), [](const auto &record) {
        return record.header.recordType == RecordType::TerminalReceipt;
      });
  if (terminal == records.end()) {
    return false;
  }
  auto &payload =
      std::get<battle_continuity::TerminalReceiptPayload>(terminal->payload);
  payload.settlementBatchId[0] ^= 0x1U;
  const auto originalReplay =
      battle_continuity::BattleReplayer::restoreJournal(*journal);
  if (!originalReplay.ok() || !originalReplay.battle.has_value()) {
    return false;
  }
  if (!originalReplay.finalRoomRecoveryState.has_value()) {
    return false;
  }
  const auto encodedState = battle_continuity::encodeRecoveryState(
      originalReplay.battle->exportDeterministicState(),
      *originalReplay.finalRoomRecoveryState);
  if (!encodedState.ok()) {
    return false;
  }
  const auto terminalHash = battle_continuity::canonicalRecoveryTerminalHash(
      encodedState.bytes, payload);
  if (!terminalHash.has_value()) {
    return false;
  }
  payload.finalStateHash = *terminalHash;
  const auto tampered = encodeRecords(records);
  if (!tampered.has_value()) {
    return false;
  }
  const auto rejected =
      game_flow::reconstructBattle(*tampered, envelope, generations, 3U);
  return !rejected.ok() && rejected.error.has_value() &&
         rejected.error->code ==
             BattleContinuityRecoveryCode::TerminalReceiptMismatch;
}

bool terminalWithNoPresentMembersRemainsHeadless() {
  const auto participants = seeds(2U);
  const auto journal = terminalJournal(true);
  const auto envelope = envelopeFor(participants);
  const auto generations = freshGenerations(participants);
  if (!journal.has_value()) {
    return false;
  }
  const auto result =
      game_flow::reconstructBattle(*journal, envelope, generations, 3U);
  return result.ok() && result.recovered.has_value() &&
         !result.recovered->room.has_value() &&
         result.recovered->activeParticipants.empty() &&
         result.recovered->settlementBatch.has_value();
}

} // namespace

int main() {
  const auto check = [](const char *name, bool (*test)()) {
    const auto result = test();
    std::cerr << name << '=' << result << '\n';
    return result;
  };
  return check("rejectsUnsupportedState", rejectsUnsupportedState) &&
                 check("departedParticipantIsNotRestoredToRoom",
                       departedParticipantIsNotRestoredToRoom) &&
                 check("terminalReceiptMustMatchRegeneratedSettlement",
                       terminalReceiptMustMatchRegeneratedSettlement) &&
                 check("terminalWithNoPresentMembersRemainsHeadless",
                       terminalWithNoPresentMembersRemainsHeadless) &&
                 check("startupArbitratesNewestBattleBeforeInstalling",
                       startupArbitratesNewestBattleBeforeInstalling) &&
                 check("startupQuarantinesOlderNonterminalBattle",
                       startupQuarantinesOlderNonterminalBattle) &&
                 check("malformedEnvelopeDoesNotInstallSessions",
                       malformedEnvelopeDoesNotInstallSessions) &&
                 check("installerFailureRollsBackAllSessions",
                       installerFailureRollsBackAllSessions) &&
                 check("headlessAlreadyDurableSkipsAppendAndRetires",
                       headlessAlreadyDurableSkipsAppendAndRetires) &&
                     check("headlessNotDurableAppendsOnceThenRetires",
                           headlessNotDurableAppendsOnceThenRetires) &&
                     check("appendFailureLeavesBattleUnretired",
                           appendFailureLeavesBattleUnretired) &&
                     check("duplicateTerminalStartupRecoveryDoesNotAppendTwice",
                           duplicateTerminalStartupRecoveryDoesNotAppendTwice)
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
