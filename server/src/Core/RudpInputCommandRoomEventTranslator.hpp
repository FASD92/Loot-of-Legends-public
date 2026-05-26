#pragma once

#include <cstdint>
#include <optional>

#include "Game/RoomEvent.hpp"
#include "Net/RudpInputCommandPayload.hpp"

namespace Core {
enum class RudpInputCommandRoomEventTranslateStatus : uint8_t {
    kTranslated,
    kInvalidSession,
    kInvalidRoom,
    kInvalidArgument,
    kUnsupportedOp,
};

struct RudpInputCommandRoomEventTranslateResult {
    RudpInputCommandRoomEventTranslateStatus status{
        RudpInputCommandRoomEventTranslateStatus::kUnsupportedOp};
    std::optional<Game::RoomEvent> event{};
};

RudpInputCommandRoomEventTranslateResult translateRudpInputCommandToRoomEvent(
    uint64_t sessionId,
    uint32_t roomId,
    const Net::RudpInputCommandPayload& input);
}  // namespace Core
