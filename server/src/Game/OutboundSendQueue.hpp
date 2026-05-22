#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include "Game/RoomActor.hpp"

namespace Game {
enum class OutboundTargetType : uint8_t {
    kSession = 0,
    kRoom = 1,
};

enum class OutboundMessageType : uint8_t {
    kError = 0,
    kReadyRoomResponse = 1,
    kBattleStart = 2,
    kMonsterSpawn = 3,
    kMonsterDeath = 4,
    kDropListSnapshot = 5,
    kLootRejected = 6,
    kLootResolved = 7,
    kInventorySnapshot = 8,
};

struct OutboundEnvelope {
    OutboundTargetType target{OutboundTargetType::kSession};
    OutboundMessageType message{OutboundMessageType::kError};
    uint64_t targetSessionId{0};
    uint32_t targetRoomId{0};

    RoomEventType sourceEventType{RoomEventType::kReady};
    RoomCommandError error{RoomCommandError::kNone};
    RoomSummary room{};
    std::vector<uint64_t> playerSessionIds{};
    Monster monster{};
    std::vector<Drop> drops{};
    Drop drop{};
    LootRejectReason lootRejectReason{LootRejectReason::kNone};
    uint64_t winnerSessionId{0};
    InventorySnapshot inventory{};
};

class OutboundSendQueue {
public:
    OutboundSendQueue() = default;
    OutboundSendQueue(const OutboundSendQueue&) = delete;
    OutboundSendQueue& operator=(const OutboundSendQueue&) = delete;

    void push(const OutboundEnvelope& envelope);
    bool tryPop(OutboundEnvelope& outEnvelope);
    size_t depth() const;
    bool empty() const;

    size_t enqueueFromRoomEventApplyResult(
        const RoomEvent& event,
        const RoomEventApplyResult& result);
    size_t enqueueRoomCommandBroadcasts(const RoomCommandResult& result);

private:
    void pushAll(const std::vector<OutboundEnvelope>& envelopes);

    std::deque<OutboundEnvelope> envelopes_;
    mutable std::mutex mutex_;
};
}  // namespace Game
