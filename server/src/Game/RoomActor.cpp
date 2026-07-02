#include "Game/RoomActor.hpp"

#include <optional>

namespace {
Game::RoomEventApplyResult rejected(Game::RoomEventApplyStatus status) {
    return Game::RoomEventApplyResult{status, Game::RoomCommandResult{}};
}

Game::RoomEventApplyResult fromCommandResult(Game::RoomCommandResult result) {
    const Game::RoomEventApplyStatus status =
        result.ok
            ? Game::RoomEventApplyStatus::kApplied
            : Game::RoomEventApplyStatus::kRoomCommandRejected;
    return Game::RoomEventApplyResult{status, result};
}
}  // namespace

namespace Game {
RoomActor::RoomActor(uint32_t roomId) : roomId_(roomId) {}

uint32_t RoomActor::roomId() const {
    return roomId_;
}

RoomEventApplyResult RoomActor::apply(
    RoomManager& roomManager,
    const RoomEvent& event) const {
    if (!isValidRoomEvent(event)) {
        return rejected(RoomEventApplyStatus::kInvalidEvent);
    }

    if (event.roomId != roomId_) {
        return rejected(RoomEventApplyStatus::kRoomMismatch);
    }

    const std::optional<uint32_t> currentRoomId =
        roomManager.findRoomIdForSession(event.sessionId);
    if (!currentRoomId.has_value() || *currentRoomId != event.roomId) {
        return rejected(RoomEventApplyStatus::kRoomMismatch);
    }

    switch (event.type) {
    case RoomEventType::kReady:
        return fromCommandResult(roomManager.markReady(event.sessionId));
    case RoomEventType::kMonsterDeath:
        return fromCommandResult(
            roomManager.defeatMonster(event.sessionId, event.argument));
    case RoomEventType::kClickLoot:
        return fromCommandResult(roomManager.claimLoot(event.sessionId, event.argument));
    case RoomEventType::kAttack:
        return fromCommandResult(roomManager.attackMonster(event.sessionId, event.argument));
    case RoomEventType::kSpaceLoot:
        return fromCommandResult(roomManager.claimNearestLoot(event.sessionId));
    }

    return rejected(RoomEventApplyStatus::kInvalidEvent);
}
}  // namespace Game
