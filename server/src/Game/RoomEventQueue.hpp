#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

#include "Game/RoomEvent.hpp"

namespace Game {
enum class RoomEventQueueEnqueueResult : uint8_t {
    kEnqueued = 0,
    kRejectedBackpressure = 1,
    kRejectedShutdown = 2,
};

struct RoomEventQueueStats {
    size_t roomEventEnqueuedCount{0};
    size_t roomEventRejectedBackpressureCount{0};
    size_t roomEventRejectedShutdownCount{0};
    size_t roomEventQueueDepth{0};
    size_t roomEventMaxQueueDepth{0};
};

class RoomEventQueue {
public:
    explicit RoomEventQueue(size_t capacity);

    RoomEventQueue(const RoomEventQueue&) = delete;
    RoomEventQueue& operator=(const RoomEventQueue&) = delete;

    RoomEventQueueEnqueueResult enqueue(const RoomEvent& event);
    bool tryDequeue(RoomEvent& outEvent);
    void beginShutdown();

    bool shuttingDown() const;
    size_t capacity() const;
    size_t depth() const;
    RoomEventQueueStats stats() const;

private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<RoomEvent> events_;
    bool shuttingDown_{false};
    size_t roomEventEnqueuedCount_{0};
    size_t roomEventRejectedBackpressureCount_{0};
    size_t roomEventRejectedShutdownCount_{0};
    size_t roomEventMaxQueueDepth_{0};
};
}  // namespace Game
