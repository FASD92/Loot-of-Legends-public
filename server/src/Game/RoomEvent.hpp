#pragma once

#include <cstdint>

namespace Game {
enum class RoomEventType : uint8_t {
    kReady = 0,
    kMonsterDeath = 1,
    kClickLoot = 2,
    kAttack = 3,
    kSpaceLoot = 4,
};

struct RoomEvent {
    RoomEventType type{RoomEventType::kReady};
    uint64_t sessionId{0};
    uint32_t roomId{0};
    uint32_t argument{0};
};

constexpr RoomEvent makeReadyRoomEvent(uint64_t sessionId, uint32_t roomId) {
    return RoomEvent{RoomEventType::kReady, sessionId, roomId, 0};
}

constexpr RoomEvent makeMonsterDeathRoomEvent(
    uint64_t sessionId,
    uint32_t roomId,
    uint32_t monsterId) {
    return RoomEvent{RoomEventType::kMonsterDeath, sessionId, roomId, monsterId};
}

constexpr RoomEvent makeClickLootRoomEvent(
    uint64_t sessionId,
    uint32_t roomId,
    uint32_t dropId) {
    return RoomEvent{RoomEventType::kClickLoot, sessionId, roomId, dropId};
}

constexpr RoomEvent makeAttackRoomEvent(
    uint64_t sessionId,
    uint32_t roomId,
    uint32_t targetHintMonsterId) {
    return RoomEvent{RoomEventType::kAttack, sessionId, roomId, targetHintMonsterId};
}

constexpr RoomEvent makeSpaceLootRoomEvent(uint64_t sessionId, uint32_t roomId) {
    return RoomEvent{RoomEventType::kSpaceLoot, sessionId, roomId, 0};
}

inline bool isValidRoomEvent(const RoomEvent& event) {
    if (event.sessionId == 0 || event.roomId == 0) {
        return false;
    }

    switch (event.type) {
    case RoomEventType::kReady:
        return event.argument == 0;
    case RoomEventType::kMonsterDeath:
    case RoomEventType::kClickLoot:
        return event.argument != 0;
    case RoomEventType::kAttack:
        return true;
    case RoomEventType::kSpaceLoot:
        return event.argument == 0;
    }

    return false;
}
}  // namespace Game
