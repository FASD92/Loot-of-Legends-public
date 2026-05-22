#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "Game/RoomEventMetrics.hpp"
#include "Game/RoomEventQueue.hpp"

namespace Game {
enum class RoomEventDispatcherEnqueueStatus : uint8_t {
    kEnqueued = 0,
    kRejectedUnknownRoom = 1,
    kRejectedBackpressure = 2,
    kRejectedShutdown = 3,
};

struct RoomEventDispatcherEnqueueResult {
    RoomEventDispatcherEnqueueStatus status{
        RoomEventDispatcherEnqueueStatus::kRejectedUnknownRoom};
    RoomEventQueueEnqueueResult queueResult{RoomEventQueueEnqueueResult::kEnqueued};
    bool scheduledRoom{false};
};

struct RoomEventDispatcherCompletionResult {
    bool knownRoom{false};
    bool rescheduledRoom{false};
};

class RoomEventDispatcher {
public:
    explicit RoomEventDispatcher(
        size_t roomQueueCapacity,
        RoomEventMetrics* metrics = nullptr);
    RoomEventDispatcher(const RoomEventDispatcher&) = delete;
    RoomEventDispatcher& operator=(const RoomEventDispatcher&) = delete;

    bool registerRoom(uint32_t roomId);
    RoomEventDispatcherEnqueueResult enqueue(const RoomEvent& event);
    bool tryPopActiveRoom(uint32_t& outRoomId);
    bool tryDequeueRoomEvent(uint32_t roomId, RoomEvent& outEvent);
    RoomEventDispatcherCompletionResult completeRoomProcessing(uint32_t roomId);
    void beginShutdown();
    bool beginRoomShutdown(uint32_t roomId);
    bool roomQueueStats(uint32_t roomId, RoomEventQueueStats& outStats) const;

private:
    struct RoomDispatchState;

    std::shared_ptr<RoomDispatchState> findRoomState(uint32_t roomId) const;
    size_t aggregateQueueDepth() const;
    void scheduleRoom(uint32_t roomId);

    const size_t roomQueueCapacity_;
    RoomEventMetrics* metrics_{nullptr};
    mutable std::mutex registryMutex_;
    std::unordered_map<uint32_t, std::shared_ptr<RoomDispatchState>> rooms_;
    bool shuttingDown_{false};
    mutable std::mutex activeRoomMutex_;
    std::deque<uint32_t> activeRooms_;
};
}  // namespace Game
