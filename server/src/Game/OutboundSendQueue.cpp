#include "Game/OutboundSendQueue.hpp"

namespace {
Game::OutboundEnvelope copyResultFields(
    Game::OutboundEnvelope envelope,
    const Game::RoomCommandResult& result) {
    envelope.error = result.error;
    envelope.room = result.room;
    envelope.playerSessionIds = result.playerSessionIds;
    envelope.monster = result.monster;
    envelope.drops = result.drops;
    envelope.drop = result.drop;
    envelope.lootRejectReason = result.lootRejectReason;
    envelope.winnerSessionId = result.winnerSessionId;
    envelope.inventory = result.inventory;
    return envelope;
}

Game::OutboundEnvelope sessionEnvelope(
    Game::OutboundMessageType message,
    uint64_t targetSessionId,
    Game::RoomEventType sourceEventType,
    const Game::RoomCommandResult& result) {
    Game::OutboundEnvelope envelope;
    envelope.target = Game::OutboundTargetType::kSession;
    envelope.message = message;
    envelope.targetSessionId = targetSessionId;
    envelope.targetRoomId = result.room.roomId;
    envelope.sourceEventType = sourceEventType;
    return copyResultFields(envelope, result);
}

Game::OutboundEnvelope roomEnvelope(
    Game::OutboundMessageType message,
    Game::RoomEventType sourceEventType,
    const Game::RoomCommandResult& result) {
    Game::OutboundEnvelope envelope;
    envelope.target = Game::OutboundTargetType::kRoom;
    envelope.message = message;
    envelope.targetRoomId = result.room.roomId;
    envelope.sourceEventType = sourceEventType;
    return copyResultFields(envelope, result);
}

Game::OutboundEnvelope commandErrorEnvelope(
    const Game::RoomEvent& event,
    const Game::RoomCommandResult& result) {
    Game::OutboundEnvelope envelope =
        sessionEnvelope(
            Game::OutboundMessageType::kError,
            event.sessionId,
            event.type,
            result);
    envelope.targetRoomId = event.roomId;
    return envelope;
}

void appendRoomCommandBroadcasts(
    std::vector<Game::OutboundEnvelope>& envelopes,
    const Game::RoomCommandResult& result,
    Game::RoomEventType sourceEventType) {
    if (result.battleJustStarted) {
        envelopes.push_back(
            roomEnvelope(
                Game::OutboundMessageType::kBattleStart,
                sourceEventType,
                result));
    }

    if (result.monsterJustSpawned) {
        envelopes.push_back(
            roomEnvelope(
                Game::OutboundMessageType::kMonsterSpawn,
                sourceEventType,
                result));
    }

    if (result.monsterJustDefeated) {
        envelopes.push_back(
            roomEnvelope(
                Game::OutboundMessageType::kMonsterDeath,
                sourceEventType,
                result));
        envelopes.push_back(
            roomEnvelope(
                Game::OutboundMessageType::kDropListSnapshot,
                sourceEventType,
                result));
    }

    if (result.lootJustClaimed) {
        envelopes.push_back(
            roomEnvelope(
                Game::OutboundMessageType::kLootResolved,
                sourceEventType,
                result));
    }
}
}  // namespace

namespace Game {
void OutboundSendQueue::push(const OutboundEnvelope& envelope) {
    std::lock_guard<std::mutex> lock(mutex_);
    envelopes_.push_back(envelope);
}

bool OutboundSendQueue::tryPop(OutboundEnvelope& outEnvelope) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (envelopes_.empty()) {
        return false;
    }

    outEnvelope = envelopes_.front();
    envelopes_.pop_front();
    return true;
}

size_t OutboundSendQueue::depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return envelopes_.size();
}

bool OutboundSendQueue::empty() const {
    return depth() == 0;
}

size_t OutboundSendQueue::enqueueFromRoomEventApplyResult(
    const RoomEvent& event,
    const RoomEventApplyResult& result) {
    std::vector<OutboundEnvelope> envelopes;
    const RoomCommandResult& command = result.commandResult;

    switch (result.status) {
    case RoomEventApplyStatus::kInvalidEvent:
    case RoomEventApplyStatus::kRoomMismatch:
        return 0;
    case RoomEventApplyStatus::kRoomCommandRejected:
        envelopes.push_back(commandErrorEnvelope(event, command));
        break;
    case RoomEventApplyStatus::kApplied:
        switch (event.type) {
        case RoomEventType::kReady:
            envelopes.push_back(
                sessionEnvelope(
                    OutboundMessageType::kReadyRoomResponse,
                    event.sessionId,
                    event.type,
                    command));
            appendRoomCommandBroadcasts(envelopes, command, event.type);
            break;
        case RoomEventType::kMonsterDeath:
            appendRoomCommandBroadcasts(envelopes, command, event.type);
            break;
        case RoomEventType::kClickLoot:
            if (command.lootRejected) {
                envelopes.push_back(
                    sessionEnvelope(
                        OutboundMessageType::kLootRejected,
                        event.sessionId,
                        event.type,
                        command));
            } else if (command.lootJustClaimed) {
                envelopes.push_back(
                    roomEnvelope(
                        OutboundMessageType::kLootResolved,
                        event.type,
                        command));
                envelopes.push_back(
                    sessionEnvelope(
                        OutboundMessageType::kInventorySnapshot,
                        event.sessionId,
                        event.type,
                        command));
            }
            break;
        }
        break;
    }

    pushAll(envelopes);
    return envelopes.size();
}

size_t OutboundSendQueue::enqueueRoomCommandBroadcasts(
    const RoomCommandResult& result) {
    std::vector<OutboundEnvelope> envelopes;
    appendRoomCommandBroadcasts(envelopes, result, RoomEventType::kReady);
    pushAll(envelopes);
    return envelopes.size();
}

void OutboundSendQueue::pushAll(const std::vector<OutboundEnvelope>& envelopes) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const OutboundEnvelope& envelope : envelopes) {
        envelopes_.push_back(envelope);
    }
}
}  // namespace Game
