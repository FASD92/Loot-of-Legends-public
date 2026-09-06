#include <lol/game_flow/BattleContinuityRecovery.hpp>

#include "workflows/BattleTerminalWorkflow.hpp"

#include <lol/battle_continuity/BattleReplay.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <limits>
#include <memory>
#include <set>
#include <tuple>
#include <utility>
#include <iostream>

namespace lol::game_flow {
namespace {

using battle::BattleDeterministicCandidateState;
using battle::BattleDeterministicParticipantState;
using battle::BattleLoadState;
using battle::BattleResultState;
using battle::LoadCandidateState;
using battle::ParticipantExitStatus;
using battle_continuity::BattleIdentity;
using battle_continuity::BattleRecoveryEnvelope;
using battle_continuity::BattleStartPayload;
using battle_continuity::Record;
using battle_continuity::RecordType;
using battle_continuity::TerminalReceiptPayload;
using shared::SessionGeneration;

BattleContinuityRecoveryResult failure(BattleContinuityRecoveryCode code,
                                       std::size_t recordIndex = 0U) {
  return BattleContinuityRecoveryResult{
      .recovered = std::nullopt,
      .error = BattleContinuityRecoveryError{.code = code,
                                             .recordIndex = recordIndex}};
}

bool validFreshGeneration(SessionGeneration generation) noexcept {
  return generation.value() != 0U;
}

const RecoveredSessionGeneration *
freshGeneration(std::span<const RecoveredSessionGeneration> generations,
                std::uint16_t slot) noexcept {
  const auto found = std::find_if(
      generations.begin(), generations.end(),
      [slot](const auto &value) { return value.participantSlot == slot; });
  return found == generations.end() ? nullptr : &*found;
}

bool validAndMatchesEnvelope(
    const BattleRecoveryEnvelope &envelope, const BattleIdentity &identity,
    const BattleStartPayload &start,
    std::span<const RecoveredSessionGeneration> generations) noexcept {
  // The codec is also the single source of the envelope value invariants.
  if (envelope.identity != identity ||
      start.participants.size() != envelope.participants.size() ||
      generations.size() != envelope.participants.size()) {
    return false;
  }

  std::set<std::uint64_t> generationValues;
  for (std::size_t index = 0U; index < envelope.participants.size(); ++index) {
    const auto &participant = envelope.participants[index];
    const auto &journalParticipant = start.participants[index];
    const auto *generation =
        freshGeneration(generations, participant.participantSlot);
    if (participant.participantSlot != index + 1U ||
        journalParticipant.participantSlot != participant.participantSlot ||
        journalParticipant.sessionId != participant.sessionId ||
        generation == nullptr ||
        !validFreshGeneration(generation->generation) ||
        generation->generation.value() <=
            participant.previousGeneration.value() ||
        !generationValues.insert(generation->generation.value()).second) {
      return false;
    }
  }
  for (std::size_t index = 0U; index < generations.size(); ++index) {
    if (generations[index].participantSlot != index + 1U ||
        freshGeneration(generations, generations[index].participantSlot) !=
            &generations[index]) {
      return false;
    }
  }
  return true;
}

bool candidateMatches(const BattleDeterministicCandidateState &candidate,
                      const battle_continuity::RecoveryParticipant &participant,
                      std::size_t index) noexcept {
  return candidate.slot == index + 1U &&
         candidate.sessionId == participant.sessionId;
}

bool participantMatches(const BattleDeterministicParticipantState &participant,
                        std::uint16_t slot,
                        const battle_continuity::RecoveryParticipant &identity,
                        const RecoveredSessionGeneration &generation) noexcept {
  return participant.slot == slot &&
         participant.sessionId == identity.sessionId &&
         generation.generation.value() != 0U;
}

bool activeInLoading(
    const BattleDeterministicCandidateState &candidate) noexcept {
  return candidate.state == LoadCandidateState::PendingLoad ||
         candidate.state == LoadCandidateState::Ready;
}

bool activeInGameplay(const BattleDeterministicParticipantState &participant,
                      bool terminal) noexcept {
  return terminal
             ? participant.exitStatus == ParticipantExitStatus::TerminalPresent
             : participant.exitStatus ==
                   ParticipantExitStatus::GameplayEligible;
}

std::optional<lobby_room::Room>
buildRoom(const BattleRecoveryEnvelope &envelope,
          const std::vector<RecoveredBattleParticipant> &active,
          const battle::BattleDeterministicState &state,
          const battle_continuity::RoomRecoveryState &roomState) {
  if (active.empty()) {
    return std::nullopt;
  }

  if (roomState.roomId != envelope.identity.roomId ||
      roomState.capacity != envelope.capacity || roomState.memberSlots.empty() ||
      roomState.phase == battle_continuity::RoomRecoveryPhase::Open) {
    return std::nullopt;
  }

  std::vector<std::uint16_t> activeSlots;
  activeSlots.reserve(active.size());
  for (const auto &participant : active) {
    activeSlots.push_back(participant.participantSlot);
  }
  std::ranges::sort(activeSlots);
  if (activeSlots != roomState.memberSlots) {
    return std::nullopt;
  }

  const auto host = std::find_if(
      active.begin(), active.end(), [&roomState](const auto &participant) {
        return participant.participantSlot == roomState.hostParticipantSlot;
      });
  if (host == active.end()) {
    return std::nullopt;
  }
  const auto creator = host;
  auto created = lobby_room::Room::create(lobby_room::CreateRoomCommand{
      .roomId = envelope.identity.roomId,
      .title = envelope.roomTitle,
      .capacity = roomState.capacity,
      .creator =
          lobby_room::RoomMemberIdentity{
              .accountId = creator->accountId,
              .sessionId = creator->sessionId,
              .generation = creator->generation,
              .nickname = creator->nickname,
          },
  });
  if (!created.room.has_value()) {
    return std::nullopt;
  }
  auto room = std::move(*created.room);
  for (const auto &participant : active) {
    if (participant.participantSlot == creator->participantSlot) {
      continue;
    }
    if (room.join(lobby_room::JoinRoomCommand{lobby_room::RoomMemberIdentity{
            participant.accountId, participant.sessionId,
            participant.generation, participant.nickname}}) !=
        lobby_room::RoomResultCode::Ok) {
      return std::nullopt;
    }
  }

  for (const auto &participant : active) {
    bool ready = roomState.phase !=
                 battle_continuity::RoomRecoveryPhase::Loading;
    if (roomState.phase == battle_continuity::RoomRecoveryPhase::Loading) {
      const auto candidate =
          std::find_if(state.candidates.begin(), state.candidates.end(),
                       [&participant](const auto &value) {
                         return value.slot == participant.participantSlot;
                       });
      ready = candidate != state.candidates.end() &&
              candidate->state == LoadCandidateState::Ready;
    }
    if (ready && room.setReady(lobby_room::SetReadyCommand{
                     .sessionId = participant.sessionId,
                     .generation = participant.generation,
                     .ready = true,
                 }) != lobby_room::RoomResultCode::Ok) {
      return std::nullopt;
    }
  }

  const auto detail = room.detail();
  if (!detail.has_value() || detail->members.empty()) {
    return std::nullopt;
  }
  const auto committed = room.commitLoading(lobby_room::BattleAdmissionSnapshot{
      .roomId = detail->roomId,
      .host = detail->hostSessionId,
      .members = detail->members,
  });
  if (committed != lobby_room::RoomResultCode::Ok) {
    return std::nullopt;
  }
  switch (roomState.phase) {
  case battle_continuity::RoomRecoveryPhase::Loading:
    break;
  case battle_continuity::RoomRecoveryPhase::InProgress:
    if (room.commitInProgress() != lobby_room::RoomResultCode::Ok) {
      return std::nullopt;
    }
    break;
  case battle_continuity::RoomRecoveryPhase::AwaitingSettlementDurability:
    if (room.commitInProgress() != lobby_room::RoomResultCode::Ok ||
        room.commitAwaitingSettlementDurability() !=
            lobby_room::RoomResultCode::Ok) {
      return std::nullopt;
    }
    break;
  case battle_continuity::RoomRecoveryPhase::Open:
    return std::nullopt;
  }
  return room;
}

const TerminalReceiptPayload *
terminalReceipt(const std::vector<Record> &records,
                std::size_t &index) noexcept {
  const TerminalReceiptPayload *receipt = nullptr;
  for (std::size_t current = 0U; current < records.size(); ++current) {
    if (records[current].header.recordType != RecordType::TerminalReceipt) {
      continue;
    }
    if (receipt != nullptr) {
      return nullptr;
    }
    receipt = std::get_if<TerminalReceiptPayload>(&records[current].payload);
    index = current;
  }
  return receipt;
}

bool verifyTerminalReceipt(const battle::BattleInstance &battle,
                           const battle::BattleDeterministicState &state,
                           const TerminalReceiptPayload &receipt,
                           const settlement::SettlementIntentBatch &batch) {
  const auto result = battle.resultProjection().result;
  const auto projection = battle.projection();
  if (!result.has_value() ||
      receipt.terminalReason != static_cast<std::uint16_t>(result->outcome) ||
      receipt.resultCommittedBattleElapsedNanos !=
          state.battleTime.battleElapsedNanos ||
      receipt.settlementBatchId != batch.id().bytes() ||
      receipt.settlementIntentCount != receipt.settlements.size() ||
      receipt.settlementIntentCount != batch.intents().size()) {
    return false;
  }

  std::set<std::uint16_t> seenSlots;
  std::set<std::size_t> seenIntents;
  for (const auto &receiptEntry : receipt.settlements) {
    if (!seenSlots.insert(receiptEntry.participantSlot).second ||
        receiptEntry.participantSlot == battle::systemParticipantSlot ||
        receiptEntry.participantSlot > state.candidates.size()) {
      return false;
    }
    const auto participant =
        std::find_if(state.participants.begin(), state.participants.end(),
                     [&receiptEntry](const auto &value) {
                       return value.slot == receiptEntry.participantSlot;
                     });
    if (participant == state.participants.end()) {
      return false;
    }
    const auto captured =
        std::find_if(projection.capturedParticipants.begin(),
                     projection.capturedParticipants.end(),
                     [&participant](const auto &value) {
                       return value.sessionId == participant->sessionId;
                     });
    if (captured == projection.capturedParticipants.end()) {
      return false;
    }
    const auto intent =
        std::find_if(batch.intents().begin(), batch.intents().end(),
                     [&captured](const auto &value) {
                       return value.accountId() == captured->accountId;
                     });
    if (intent == batch.intents().end()) {
      return false;
    }
    const auto intentIndex = static_cast<std::size_t>(
        std::distance(batch.intents().begin(), intent));
    if (!seenIntents.insert(intentIndex).second ||
        intent->id().bytes() != receiptEntry.settlementId ||
        intent->canonicalHash().bytes() != receiptEntry.settlementPayloadHash) {
      return false;
    }
  }
  return seenIntents.size() == batch.intents().size();
}

constexpr auto kStartupSettlementWait = std::chrono::seconds{5};

bool journalHasTerminalReceipt(std::span<const std::uint8_t> journal) noexcept {
  const auto decoded = battle_continuity::decodeJournal(journal);
  return decoded.ok() &&
         std::ranges::any_of(decoded.records, [](const auto &record) {
           return record.header.recordType == RecordType::TerminalReceipt;
         });
}

bool disposeBattle(StartupRecoveryInput &input, StoredBattleRecovery stored,
                   BattleRecoveryDisposition disposition,
                   std::optional<BattleContinuityRecoveryCode> reason =
                       std::nullopt) {
  return input.dispose(BattleRecoveryRequest{
      .stored = std::move(stored),
      .disposition = disposition,
      .reason = reason,
  });
}

bool rollbackSessions(session::SessionRegistry &sessions,
                      std::span<const session::AuthenticateSessionResult>
                          installed) noexcept {
  bool disconnected = true;
  for (const auto &session : installed) {
    disconnected =
        sessions.disconnect(session.sessionId, session.generation) &&
        disconnected;
  }
  return disconnected;
}

bool appendSettlement(settlement::SettlementStoragePort &storage,
                      const settlement::SettlementIntentBatch &batch) {
  std::vector<std::vector<std::uint8_t>> canonicalIntents;
  canonicalIntents.reserve(batch.intents().size());
  for (const auto &intent : batch.intents()) {
    canonicalIntents.push_back(settlement::canonicalPayload(intent));
  }

  auto completion =
      std::make_shared<std::promise<settlement::DurableAppendOutcome>>();
  auto completed = completion->get_future();
  const auto submitted = storage.submit(
      settlement::DurableAppendRequest{
          .batchId = batch.id(),
          .roomId = batch.roomId(),
          .battleId = batch.battleId(),
          .canonicalIntents = std::move(canonicalIntents),
      },
      [completion](settlement::DurableAppendOutcome outcome) mutable {
        completion->set_value(std::move(outcome));
      });
  if (submitted != settlement::SubmitAppendResult::Accepted ||
      completed.wait_for(kStartupSettlementWait) !=
          std::future_status::ready) {
    return false;
  }

  const auto outcome = completed.get();
  const auto *durable =
      std::get_if<settlement::DurableAppendCompleted>(&outcome);
  return durable != nullptr && durable->batchId == batch.id() &&
         durable->roomId == batch.roomId() &&
         durable->battleId == batch.battleId();
}

} // namespace

BattleContinuityRecoveryResult
reconstructBattle(std::span<const std::uint8_t> committedJournal,
                  const BattleRecoveryEnvelope &envelope,
                  std::span<const RecoveredSessionGeneration> freshGenerations,
                  std::uint32_t currentWriterRecoveryEpoch) {
  if (currentWriterRecoveryEpoch == 0U) {
    return failure(BattleContinuityRecoveryCode::InvalidWriterRecoveryEpoch);
  }

  const auto replay =
      battle_continuity::BattleReplayer::restoreJournal(committedJournal);
  if (!replay.ok() || !replay.battle.has_value() ||
      !replay.finalStateHash.has_value() ||
      !replay.finalRoomRecoveryState.has_value()) {
    return failure(BattleContinuityRecoveryCode::JournalRejected,
                   replay.error.has_value() ? replay.error->recordIndex : 0U);
  }
  const auto decoded = battle_continuity::decodeJournal(committedJournal);
  if (!decoded.ok() || decoded.records.empty()) {
    return failure(BattleContinuityRecoveryCode::JournalRejected);
  }
  const auto *startPayload =
      std::get_if<BattleStartPayload>(&decoded.records.front().payload);
  if (startPayload == nullptr) {
    return failure(BattleContinuityRecoveryCode::JournalRejected);
  }
  const BattleIdentity identity{
      .originRecoveryEpoch = decoded.records.front().header.originRecoveryEpoch,
      .roomId = decoded.records.front().header.roomId,
      .battleInstanceId = decoded.records.front().header.battleInstanceId,
  };
  if (!battle_continuity::encodeBattleRecoveryEnvelope(envelope).ok()) {
    return failure(BattleContinuityRecoveryCode::ParticipantMappingMismatch);
  }
  if (envelope.identity != identity) {
    return failure(BattleContinuityRecoveryCode::IdentityMismatch);
  }
  if (!validAndMatchesEnvelope(envelope, identity, *startPayload,
                               freshGenerations)) {
    return failure(BattleContinuityRecoveryCode::ParticipantMappingMismatch);
  }

  const auto state = replay.battle->exportDeterministicState();
  const auto &roomRecoveryState = *replay.finalRoomRecoveryState;
  if (roomRecoveryState.roomId != identity.roomId ||
      roomRecoveryState.capacity != envelope.capacity ||
      roomRecoveryState.nextBattleOrdinal == 0U) {
    return failure(BattleContinuityRecoveryCode::RoomReconstructionFailed);
  }
  if (state.state == BattleLoadState::Created) {
    return failure(BattleContinuityRecoveryCode::CreatedState);
  }
  if (state.state == BattleLoadState::LoadCancelled) {
    return failure(BattleContinuityRecoveryCode::LoadCancelledState);
  }
  if (state.resultState == BattleResultState::ResultGenerationFailed) {
    return failure(BattleContinuityRecoveryCode::ResultGenerationFailed);
  }
  if (state.candidates.size() != envelope.participants.size()) {
    return failure(BattleContinuityRecoveryCode::ParticipantMappingMismatch);
  }
  for (std::size_t index = 0U; index < state.candidates.size(); ++index) {
    if (!candidateMatches(state.candidates[index], envelope.participants[index],
                          index)) {
      return failure(BattleContinuityRecoveryCode::ParticipantMappingMismatch);
    }
  }

  std::vector<battle::BattleStartCandidate> candidates;
  candidates.reserve(envelope.participants.size());
  for (const auto &participant : envelope.participants) {
    const auto *generation =
        freshGeneration(freshGenerations, participant.participantSlot);
    if (generation == nullptr ||
        !validFreshGeneration(generation->generation)) {
      return failure(BattleContinuityRecoveryCode::InvalidSessionGeneration);
    }
    candidates.push_back(battle::BattleStartCandidate{
        .accountId = participant.accountId,
        .sessionId = participant.sessionId,
        .generation = generation->generation,
        .nickname = participant.nickname,
    });
  }
  auto created = battle::BattleInstance::create(battle::BattleAdmissionSnapshot{
      .roomId = identity.roomId,
      .battleId = identity.battleInstanceId,
      .candidates = std::move(candidates),
      .rulesetVersion = state.rulesetVersion,
      .seed = state.seed,
  });
  if (created.code != battle::BattleLoadResultCode::Ok ||
      !created.battle.has_value()) {
    return failure(BattleContinuityRecoveryCode::BattleReconstructionFailed);
  }
  auto battle = std::move(*created.battle);
  const auto imported = battle.importDeterministicState(state);
  if (imported != battle::BattleStateImportResultCode::Ok ||
      !replay.finalStateHash.has_value()) {
    std::cerr << "recovery import=" << static_cast<unsigned int>(imported)
              << " finalhash=" << replay.finalStateHash.has_value() << '\n';
    return failure(BattleContinuityRecoveryCode::BattleReconstructionFailed);
  }
  const auto encoded = battle_continuity::encodeRecoveryState(
      battle.exportDeterministicState(), roomRecoveryState);
  const auto importedHash =
      encoded.ok() ? battle_continuity::recoveryStateHash(encoded.bytes)
                   : std::nullopt;
  if (!importedHash.has_value() || *importedHash != *replay.finalStateHash) {
    std::cerr << "recovery hash encoded=" << encoded.ok()
              << " imported=" << importedHash.has_value()
              << " equal=" << (importedHash.has_value() &&
                                  *importedHash == *replay.finalStateHash)
              << '\n';
    return failure(BattleContinuityRecoveryCode::BattleReconstructionFailed);
  }

  auto recording = battle_continuity::BattleRecording::resume(
      battle, decoded.records, currentWriterRecoveryEpoch);
  if (!recording.has_value()) {
    return failure(BattleContinuityRecoveryCode::RecordingResumeFailed);
  }

  const bool terminal = state.resultState == BattleResultState::Committed;
  std::vector<RecoveredBattleParticipant> active;
  active.reserve(envelope.participants.size());
  for (std::size_t index = 0U; index < envelope.participants.size(); ++index) {
    const auto &identityParticipant = envelope.participants[index];
    const auto *generation =
        freshGeneration(freshGenerations, identityParticipant.participantSlot);
    if (generation == nullptr) {
      return failure(BattleContinuityRecoveryCode::InvalidSessionGeneration);
    }
    ParticipantExitStatus exitStatus = ParticipantExitStatus::GameplayEligible;
    bool present = false;
    if (state.state == BattleLoadState::LoadBarrierOpen) {
      present = activeInLoading(state.candidates[index]);
    } else {
      const auto participant = std::find_if(
          state.participants.begin(), state.participants.end(),
          [&identityParticipant](const auto &value) {
            return value.sessionId == identityParticipant.sessionId;
          });
      if (participant == state.participants.end()) {
        const auto candidate = std::find_if(
            state.candidates.begin(), state.candidates.end(),
            [&identityParticipant](const auto &value) {
              return value.slot == identityParticipant.participantSlot &&
                     value.sessionId == identityParticipant.sessionId;
            });
        if (candidate == state.candidates.end() ||
            (candidate->state != LoadCandidateState::Disconnected &&
             candidate->state != LoadCandidateState::TimedOut)) {
          return failure(
              BattleContinuityRecoveryCode::ParticipantMappingMismatch);
        }
        continue;
      }
      if (!participantMatches(*participant, identityParticipant.participantSlot,
                              identityParticipant, *generation)) {
        return failure(
            BattleContinuityRecoveryCode::ParticipantMappingMismatch);
      }
      exitStatus = participant->exitStatus;
      present = activeInGameplay(*participant, terminal);
    }
    if (present) {
      active.push_back(RecoveredBattleParticipant{
          .participantSlot = identityParticipant.participantSlot,
          .accountId = identityParticipant.accountId,
          .sessionId = identityParticipant.sessionId,
          .generation = generation->generation,
          .nickname = identityParticipant.nickname,
          .exitStatus = exitStatus,
      });
    }
  }
  if (active.empty() && !terminal) {
    return failure(BattleContinuityRecoveryCode::RoomReconstructionFailed);
  }

  std::optional<settlement::SettlementIntentBatch> settlementBatch;
  if (terminal) {
    std::size_t terminalIndex = 0U;
    const auto *receipt = terminalReceipt(decoded.records, terminalIndex);
    if (receipt == nullptr) {
      return failure(BattleContinuityRecoveryCode::TerminalReceiptMissing);
    }
    settlementBatch = workflows::createSettlementIntentBatchForTerminal(
        battle,
        settlement::ResultCommittedAt{
            .unixEpochMilliseconds =
                receipt->resultCommittedUnixEpochMilliseconds,
            .monotonicNanoseconds = receipt->resultCommittedBattleElapsedNanos,
        });
    if (!settlementBatch.has_value()) {
      return failure(BattleContinuityRecoveryCode::SettlementBatchInvalid,
                     terminalIndex);
    }
    if (!verifyTerminalReceipt(battle, state, *receipt, *settlementBatch)) {
      return failure(BattleContinuityRecoveryCode::TerminalReceiptMismatch,
                     terminalIndex);
    }
  }

  auto room = buildRoom(envelope, active, state, roomRecoveryState);
  if (active.empty() && terminal) {
    room.reset();
  } else if (!room.has_value()) {
    return failure(BattleContinuityRecoveryCode::RoomReconstructionFailed);
  }

  return BattleContinuityRecoveryResult{
      .recovered =
          RecoveredBattleReconstruction{
              .battle = std::move(battle),
              .recording = std::move(*recording),
              .roomRecoveryState = roomRecoveryState,
              .room = std::move(room),
              .settlementBatch = std::move(settlementBatch),
              .activeParticipants = std::move(active),
          },
      .error = std::nullopt,
  };
}

bool recoverBattlesAtStartup(StartupRecoveryInput input) {
  if (input.currentWriterRecoveryEpoch == 0U || !input.dispose ||
      !input.settlementDurable || !input.install) {
    return false;
  }

  std::ranges::sort(input.battles, [](const auto &left, const auto &right) {
    return std::tie(left.identity.roomId, left.identity.battleInstanceId) <
           std::tie(right.identity.roomId, right.identity.battleInstanceId);
  });

  for (std::size_t first = 0U; first < input.battles.size();) {
    std::size_t after = first + 1U;
    while (after < input.battles.size() &&
           input.battles[after].identity.roomId ==
               input.battles[first].identity.roomId) {
      ++after;
    }

    for (std::size_t index = first; index + 1U < after; ++index) {
      const auto disposition =
          journalHasTerminalReceipt(input.battles[index].committedJournal)
              ? BattleRecoveryDisposition::Retire
              : BattleRecoveryDisposition::Quarantine;
      if (!disposeBattle(input, std::move(input.battles[index]), disposition)) {
        return false;
      }
    }

    StoredBattleRecovery stored = std::move(input.battles[after - 1U]);
    first = after;
    if (!stored.privateEnvelope.has_value()) {
      if (!disposeBattle(
              input, std::move(stored), BattleRecoveryDisposition::Quarantine,
              BattleContinuityRecoveryCode::IdentityMismatch)) {
        return false;
      }
      continue;
    }

    const auto decodedEnvelope =
        battle_continuity::decodeBattleRecoveryEnvelope(
            *stored.privateEnvelope);
    if (!decodedEnvelope.ok() || !decodedEnvelope.envelope.has_value() ||
        decodedEnvelope.envelope->identity != stored.identity) {
      if (!disposeBattle(
              input, std::move(stored), BattleRecoveryDisposition::Quarantine,
              BattleContinuityRecoveryCode::IdentityMismatch)) {
        return false;
      }
      continue;
    }
    const auto &envelope = *decodedEnvelope.envelope;

    std::vector<session::RecoveredDetachedSession> detached;
    detached.reserve(envelope.participants.size());
    for (const auto &participant : envelope.participants) {
      detached.push_back(session::RecoveredDetachedSession{
          .identity = session::ClaimedGameIdentity{
              .accountId = participant.accountId,
              .nickname = participant.nickname,
          },
          .sessionId = participant.sessionId,
          .previousGeneration = participant.previousGeneration,
      });
    }
    auto installed = input.sessions.installRecoveredDetached(
        std::move(detached), input.reconnectExpiresAt);
    if (!installed.has_value()) {
      if (!disposeBattle(
              input, std::move(stored), BattleRecoveryDisposition::Quarantine,
              BattleContinuityRecoveryCode::InvalidSessionGeneration)) {
        return false;
      }
      continue;
    }

    const bool mappingMatches =
        installed->size() == envelope.participants.size() &&
        std::equal(installed->begin(), installed->end(),
                   envelope.participants.begin(), envelope.participants.end(),
                   [](const auto &session, const auto &participant) {
                     return session.sessionId == participant.sessionId;
                   });
    if (!mappingMatches) {
      if (!rollbackSessions(input.sessions, *installed) ||
          !disposeBattle(
              input, std::move(stored), BattleRecoveryDisposition::Quarantine,
              BattleContinuityRecoveryCode::ParticipantMappingMismatch)) {
        return false;
      }
      continue;
    }

    std::vector<RecoveredSessionGeneration> generations;
    generations.reserve(installed->size());
    for (std::size_t index = 0U; index < installed->size(); ++index) {
      generations.push_back(RecoveredSessionGeneration{
          .participantSlot = envelope.participants[index].participantSlot,
          .generation = (*installed)[index].generation,
      });
    }
    auto reconstructed = reconstructBattle(
        stored.committedJournal, envelope, generations,
        input.currentWriterRecoveryEpoch);
    if (!reconstructed.ok() || !reconstructed.recovered.has_value()) {
      const auto code = reconstructed.error.has_value()
                            ? reconstructed.error->code
                            : BattleContinuityRecoveryCode::
                                  BattleReconstructionFailed;
      const auto disposition =
          code == BattleContinuityRecoveryCode::LoadCancelledState
              ? BattleRecoveryDisposition::Retire
              : BattleRecoveryDisposition::Quarantine;
      if (!rollbackSessions(input.sessions, *installed) ||
          !disposeBattle(input, std::move(stored), disposition, code)) {
        return false;
      }
      continue;
    }

    auto recovered = std::move(*reconstructed.recovered);
    const auto isActive = [&recovered](shared::SessionId sessionId) {
      return std::ranges::any_of(
          recovered.activeParticipants,
          [sessionId](const auto &participant) {
            return participant.sessionId == sessionId;
          });
    };
    std::vector<session::AuthenticateSessionResult> activeInstalled;
    activeInstalled.reserve(installed->size());
    for (const auto &session : *installed) {
      if (isActive(session.sessionId)) {
        activeInstalled.push_back(session);
      } else if (!input.sessions.disconnect(session.sessionId,
                                            session.generation)) {
        return false;
      }
    }

    if (!recovered.room.has_value()) {
      if (!recovered.settlementBatch.has_value()) {
        if (!rollbackSessions(input.sessions, activeInstalled) ||
            !disposeBattle(
                input, std::move(stored), BattleRecoveryDisposition::Quarantine,
                BattleContinuityRecoveryCode::SettlementBatchInvalid)) {
          return false;
        }
        continue;
      }
      const bool durable =
          input.settlementDurable(*recovered.settlementBatch);
      if ((!durable &&
           !appendSettlement(input.settlementStorage,
                             *recovered.settlementBatch)) ||
          !rollbackSessions(input.sessions, activeInstalled)) {
        return false;
      }
      if (!disposeBattle(input, std::move(stored),
                         BattleRecoveryDisposition::Retire)) {
        return false;
      }
      continue;
    }

    const bool durable =
        recovered.settlementBatch.has_value() &&
        input.settlementDurable(*recovered.settlementBatch);
    auto sessionsForRollback = activeInstalled;
    const bool installedAtRuntime = input.install(
        StartupRecoveredBattleInstall{
            .reconstruction = std::move(recovered),
            .installedSessions = std::move(activeInstalled),
            .reconnectExpiresAt = input.reconnectExpiresAt,
            .settlementAlreadyDurable = durable,
        });
    if (!installedAtRuntime) {
      if (!rollbackSessions(input.sessions, sessionsForRollback) ||
          !disposeBattle(
              input, std::move(stored), BattleRecoveryDisposition::Quarantine,
              BattleContinuityRecoveryCode::RoomReconstructionFailed)) {
        return false;
      }
      return false;
    }
  }

  return true;
}

} // namespace lol::game_flow
