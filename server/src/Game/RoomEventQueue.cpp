#include "Game/RoomEventQueue.hpp"

#include <algorithm>

namespace Game {
RoomEventQueue::RoomEventQueue(size_t capacity) : capacity_(capacity) {}

RoomEventQueueEnqueueResult RoomEventQueue::enqueue(const RoomEvent& event) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (shuttingDown_) {
        ++roomEventRejectedShutdownCount_;
        return RoomEventQueueEnqueueResult::kRejectedShutdown;
    }

    if (events_.size() >= capacity_) {
        ++roomEventRejectedBackpressureCount_;
        return RoomEventQueueEnqueueResult::kRejectedBackpressure;
    }

    events_.push_back(event);
    ++roomEventEnqueuedCount_;
    roomEventMaxQueueDepth_ = std::max(roomEventMaxQueueDepth_, events_.size());
    return RoomEventQueueEnqueueResult::kEnqueued;
}

bool RoomEventQueue::tryDequeue(RoomEvent& outEvent) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (events_.empty()) {
        return false;
    }

    outEvent = events_.front();
    events_.pop_front();
    return true;
}

void RoomEventQueue::beginShutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    shuttingDown_ = true;
}

bool RoomEventQueue::shuttingDown() const {
    std::lock_guard<std::mutex> lock(mutex_);

    return shuttingDown_;
}

size_t RoomEventQueue::capacity() const {
    return capacity_;
}

size_t RoomEventQueue::depth() const {
    std::lock_guard<std::mutex> lock(mutex_);

    return events_.size();
}

RoomEventQueueStats RoomEventQueue::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    return RoomEventQueueStats{
        roomEventEnqueuedCount_,
        roomEventRejectedBackpressureCount_,
        roomEventRejectedShutdownCount_,
        events_.size(),
        roomEventMaxQueueDepth_};
}
}  // namespace Game
