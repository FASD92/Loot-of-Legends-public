#pragma once

#include <cstdint>

#include "Game/RoomEvent.hpp"
#include "Game/RoomManager.hpp"

namespace Game {
enum class RoomEventApplyStatus : uint8_t {
    kApplied = 0,
    kInvalidEvent = 1,
    kRoomMismatch = 2,
    kRoomCommandRejected = 3,
};

struct RoomEventApplyResult {
    RoomEventApplyStatus status{RoomEventApplyStatus::kInvalidEvent};
    RoomCommandResult commandResult{};
};

class RoomActor {
public:
    explicit RoomActor(uint32_t roomId);

    uint32_t roomId() const;
    RoomEventApplyResult apply(RoomManager& roomManager, const RoomEvent& event) const;

private:
    uint32_t roomId_;
};
}  // namespace Game
