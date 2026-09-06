#include "workflows/BattleContinuityWorkflow.hpp"

#include <algorithm>
#include <ranges>

namespace lol::game_flow::workflows {
namespace {

std::optional<std::uint16_t> participantSlotForSessionImpl(
    const battle::BattleDeterministicState &state, shared::SessionId sessionId) {
  const auto participant = std::ranges::find_if(
      state.participants,
      [sessionId](const auto &value) { return value.sessionId == sessionId; });
  if (participant != state.participants.end()) {
    return participant->slot;
  }
  const auto candidate = std::ranges::find_if(
      state.candidates,
      [sessionId](const auto &value) { return value.sessionId == sessionId; });
  if (candidate == state.candidates.end()) {
    return std::nullopt;
  }
  return candidate->slot;
}

std::optional<battle_continuity::RoomRecoveryPhase>
recoveryPhase(lobby_room::RoomLifecycle lifecycle) noexcept {
  switch (lifecycle) {
  case lobby_room::RoomLifecycle::Open:
    return battle_continuity::RoomRecoveryPhase::Open;
  case lobby_room::RoomLifecycle::Loading:
    return battle_continuity::RoomRecoveryPhase::Loading;
  case lobby_room::RoomLifecycle::InProgress:
    return battle_continuity::RoomRecoveryPhase::InProgress;
  case lobby_room::RoomLifecycle::AwaitingSettlementDurability:
    return battle_continuity::RoomRecoveryPhase::AwaitingSettlementDurability;
  }
  return std::nullopt;
}

} // namespace

std::optional<std::uint16_t> participantSlotForSession(
    const battle::BattleDeterministicState &state, shared::SessionId sessionId) {
  return participantSlotForSessionImpl(state, sessionId);
}

std::optional<battle_continuity::RoomRecoveryState>
captureRoomRecoveryState(const lobby_room::Room &room,
                         const battle::BattleInstance &battle,
                         std::uint64_t nextBattleOrdinal) {
  const auto detail = room.detail();
  const auto state = battle.exportDeterministicState();
  const auto phase = detail.has_value() ? recoveryPhase(detail->lifecycle)
                                        : std::nullopt;
  if (!detail.has_value() || !phase.has_value() || nextBattleOrdinal == 0U ||
      detail->roomId != state.roomId || detail->members.empty()) {
    return std::nullopt;
  }

  std::vector<std::uint16_t> memberSlots;
  memberSlots.reserve(detail->members.size());
  std::optional<std::uint16_t> hostParticipantSlot;
  for (const auto &member : detail->members) {
    const auto slot = participantSlotForSession(state, member.sessionId);
    if (!slot.has_value()) {
      return std::nullopt;
    }
    memberSlots.push_back(*slot);
    if (member.sessionId == detail->hostSessionId) {
      hostParticipantSlot = *slot;
    }
  }
  std::ranges::sort(memberSlots);
  if (!hostParticipantSlot.has_value() || memberSlots.empty() ||
      *hostParticipantSlot != memberSlots.front() ||
      std::ranges::adjacent_find(memberSlots) != memberSlots.end()) {
    return std::nullopt;
  }

  const battle_continuity::RoomRecoveryState result{
      .roomId = detail->roomId,
      .capacity = detail->capacity,
      .hostParticipantSlot = *hostParticipantSlot,
      .memberSlots = std::move(memberSlots),
      .phase = *phase,
      .nextBattleOrdinal = nextBattleOrdinal,
  };
  return battle_continuity::encodeRoomRecoveryState(result).ok()
             ? std::optional{result}
             : std::nullopt;
}

std::optional<battle_continuity::Bytes> captureRecoveryEnvelope(
    const lobby_room::Room &room, const battle::BattleInstance &battle,
    battle_continuity::BattleIdentity identity) {
  const auto detail = room.detail();
  const auto state = battle.exportDeterministicState();
  if (!detail.has_value() || detail->roomId != identity.roomId ||
      state.roomId != identity.roomId ||
      state.battleId != identity.battleInstanceId ||
      detail->members.size() != state.candidates.size()) {
    return std::nullopt;
  }

  std::vector<battle_continuity::RecoveryParticipant> participants;
  participants.reserve(state.candidates.size());
  std::optional<std::uint16_t> hostSlot;
  for (const auto &candidate : state.candidates) {
    const auto member = std::ranges::find_if(
        detail->members, [&candidate](const auto &value) {
          return value.sessionId == candidate.sessionId;
        });
    if (member == detail->members.end()) {
      return std::nullopt;
    }
    if (member->sessionId == detail->hostSessionId) {
      hostSlot = candidate.slot;
    }
    participants.push_back(battle_continuity::RecoveryParticipant{
        .participantSlot = candidate.slot,
        .accountId = member->accountId,
        .sessionId = member->sessionId,
        .previousGeneration = member->sessionGeneration,
        .nickname = member->nickname,
    });
  }
  if (!hostSlot.has_value()) {
    return std::nullopt;
  }

  const auto encoded = battle_continuity::encodeBattleRecoveryEnvelope(
      battle_continuity::BattleRecoveryEnvelope{
          .schemaVersion = battle_continuity::kRecoveryEnvelopeSchemaVersion,
          .identity = identity,
          .roomTitle = detail->title,
          .capacity = detail->capacity,
          .hostParticipantSlot = *hostSlot,
          .participants = std::move(participants),
      });
  return encoded.ok() ? std::optional{encoded.bytes} : std::nullopt;
}

std::optional<battle_continuity::TerminalRecording> captureTerminalRecording(
    const battle::BattleInstance &battle,
    const settlement::SettlementIntentBatch &settlementBatch) {
  const auto result = battle.resultProjection().result;
  const auto projection = battle.projection();
  const auto state = battle.exportDeterministicState();
  if (!result.has_value() || result->roomId != settlementBatch.roomId() ||
      result->battleId != settlementBatch.battleId()) {
    return std::nullopt;
  }

  std::vector<battle_continuity::SettlementReceipt> receipts;
  receipts.reserve(settlementBatch.intents().size());
  for (const auto &intent : settlementBatch.intents()) {
    const auto captured = std::ranges::find_if(
        projection.capturedParticipants, [&intent](const auto &participant) {
          return participant.accountId == intent.accountId();
        });
    if (captured == projection.capturedParticipants.end()) {
      return std::nullopt;
    }
    const auto slot = participantSlotForSession(state, captured->sessionId);
    if (!slot.has_value()) {
      return std::nullopt;
    }
    receipts.push_back(battle_continuity::SettlementReceipt{
        .participantSlot = *slot,
        .settlementId = intent.id().bytes(),
        .settlementPayloadHash = intent.canonicalHash().bytes(),
    });
  }
  std::ranges::sort(receipts, {},
                    &battle_continuity::SettlementReceipt::participantSlot);
  return battle_continuity::TerminalRecording{
      .terminalReason = static_cast<std::uint16_t>(result->outcome),
      .resultCommittedUnixEpochMilliseconds =
          settlementBatch.committedAt().unixEpochMilliseconds,
      .settlementBatchId = settlementBatch.id().bytes(),
      .settlements = std::move(receipts),
  };
}

std::optional<battle_continuity::Bytes>
encodeJournal(const battle_continuity::BattleRecording &recording) {
  const auto encoded = recording.encodeJournal();
  return encoded.ok() ? std::optional{encoded.bytes} : std::nullopt;
}

} // namespace lol::game_flow::workflows
