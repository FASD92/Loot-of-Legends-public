#include "Core/RudpInputCommandRoomEventTranslator.hpp"

namespace {
Core::RudpInputCommandRoomEventTranslateResult reject(
    Core::RudpInputCommandRoomEventTranslateStatus status) {
    return Core::RudpInputCommandRoomEventTranslateResult{status, std::nullopt};
}

Core::RudpInputCommandRoomEventTranslateResult translated(
    const Game::RoomEvent& event) {
    return Core::RudpInputCommandRoomEventTranslateResult{
        Core::RudpInputCommandRoomEventTranslateStatus::kTranslated,
        event};
}
}  // namespace

namespace Core {
RudpInputCommandRoomEventTranslateResult translateRudpInputCommandToRoomEvent(
    uint64_t sessionId,
    uint32_t roomId,
    const Net::RudpInputCommandPayload& input) {
    if (sessionId == 0) {
        return reject(RudpInputCommandRoomEventTranslateStatus::kInvalidSession);
    }
    if (roomId == 0) {
        return reject(RudpInputCommandRoomEventTranslateStatus::kInvalidRoom);
    }

    switch (input.op) {
    case Net::RudpInputCommandOp::kReady:
        if (input.argValue != 0) {
            return reject(RudpInputCommandRoomEventTranslateStatus::kInvalidArgument);
        }
        return translated(Game::makeReadyRoomEvent(sessionId, roomId));
    case Net::RudpInputCommandOp::kMonsterDeath:
        if (input.argValue == 0) {
            return reject(RudpInputCommandRoomEventTranslateStatus::kInvalidArgument);
        }
        return translated(Game::makeMonsterDeathRoomEvent(
            sessionId,
            roomId,
            input.argValue));
    case Net::RudpInputCommandOp::kClickLoot:
        if (input.argValue == 0) {
            return reject(RudpInputCommandRoomEventTranslateStatus::kInvalidArgument);
        }
        return translated(Game::makeClickLootRoomEvent(
            sessionId,
            roomId,
            input.argValue));
    case Net::RudpInputCommandOp::kMove:
        return reject(RudpInputCommandRoomEventTranslateStatus::kUnsupportedOp);
    }

    return reject(RudpInputCommandRoomEventTranslateStatus::kUnsupportedOp);
}
}  // namespace Core
