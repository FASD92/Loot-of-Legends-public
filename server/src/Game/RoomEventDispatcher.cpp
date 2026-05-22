#include "Game/RoomEventDispatcher.hpp"

#include <atomic>

namespace {
Game::RoomEventDispatcherEnqueueStatus statusFromQueueResult(
    Game::RoomEventQueueEnqueueResult result) {
    switch (result) {
    case Game::RoomEventQueueEnqueueResult::kEnqueued:
        return Game::RoomEventDispatcherEnqueueStatus::kEnqueued;
    case Game::RoomEventQueueEnqueueResult::kRejectedBackpressure:
        return Game::RoomEventDispatcherEnqueueStatus::kRejectedBackpressure;
    case Game::RoomEventQueueEnqueueResult::kRejectedShutdown:
        return Game::RoomEventDispatcherEnqueueStatus::kRejectedShutdown;
    }

    return Game::RoomEventDispatcherEnqueueStatus::kRejectedUnknownRoom;
}
}  // namespace

namespace Game {
struct RoomEventDispatcher::RoomDispatchState {
    explicit RoomDispatchState(size_t queueCapacity) : queue(queueCapacity) {}

    RoomEventQueue queue;
    std::atomic_bool roomScheduled{false};
};

RoomEventDispatcher::RoomEventDispatcher(
    size_t roomQueueCapacity,
    RoomEventMetrics* metrics)
    : roomQueueCapacity_(roomQueueCapacity),
      metrics_(metrics) {}

bool RoomEventDispatcher::registerRoom(uint32_t roomId) {
    if (roomId == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(registryMutex_);
    if (shuttingDown_) {
        return false;
    }

    return rooms_
        .emplace(roomId, std::make_shared<RoomDispatchState>(roomQueueCapacity_))
        .second;
}

RoomEventDispatcherEnqueueResult RoomEventDispatcher::enqueue(const RoomEvent& event) {
    RoomEventDispatcherEnqueueResult result;
    const std::shared_ptr<RoomDispatchState> state = findRoomState(event.roomId);
    if (state == nullptr) {
        result.status = RoomEventDispatcherEnqueueStatus::kRejectedUnknownRoom;
        return result;
    }

    result.queueResult = state->queue.enqueue(event);
    result.status = statusFromQueueResult(result.queueResult);
    if (result.queueResult != RoomEventQueueEnqueueResult::kEnqueued) {
        if (metrics_ != nullptr) {
            if (result.queueResult ==
                RoomEventQueueEnqueueResult::kRejectedBackpressure) {
                metrics_->recordRejectedBackpressure();
            } else if (
                result.queueResult ==
                RoomEventQueueEnqueueResult::kRejectedShutdown) {
                metrics_->recordRejectedShutdown();
            }
        }
        return result;
    }

    if (metrics_ != nullptr) {
        const size_t queueDepth = aggregateQueueDepth();
        metrics_->recordEnqueued(queueDepth, queueDepth);
    }

    if (!state->roomScheduled.exchange(true)) {
        scheduleRoom(event.roomId);
        result.scheduledRoom = true;
    } else if (metrics_ != nullptr) {
        metrics_->recordScheduledContention();
    }

    return result;
}

bool RoomEventDispatcher::tryPopActiveRoom(uint32_t& outRoomId) {
    std::lock_guard<std::mutex> lock(activeRoomMutex_);
    if (activeRooms_.empty()) {
        return false;
    }

    outRoomId = activeRooms_.front();
    activeRooms_.pop_front();
    return true;
}

bool RoomEventDispatcher::tryDequeueRoomEvent(uint32_t roomId, RoomEvent& outEvent) {
    const std::shared_ptr<RoomDispatchState> state = findRoomState(roomId);
    if (state == nullptr) {
        return false;
    }

    const bool dequeued = state->queue.tryDequeue(outEvent);
    if (metrics_ != nullptr) {
        const size_t queueDepth = aggregateQueueDepth();
        metrics_->recordQueueDepth(queueDepth, queueDepth);
    }
    return dequeued;
}

RoomEventDispatcherCompletionResult RoomEventDispatcher::completeRoomProcessing(
    uint32_t roomId) {
    RoomEventDispatcherCompletionResult result;
    const std::shared_ptr<RoomDispatchState> state = findRoomState(roomId);
    if (state == nullptr) {
        return result;
    }

    result.knownRoom = true;
    if (state->queue.depth() > 0) {
        scheduleRoom(roomId);
        result.rescheduledRoom = true;
        return result;
    }

    state->roomScheduled.store(false);
    if (state->queue.depth() > 0 && !state->roomScheduled.exchange(true)) {
        scheduleRoom(roomId);
        result.rescheduledRoom = true;
    }
    return result;
}

void RoomEventDispatcher::beginShutdown() {
    std::lock_guard<std::mutex> lock(registryMutex_);
    shuttingDown_ = true;
    for (auto& roomEntry : rooms_) {
        roomEntry.second->queue.beginShutdown();
    }
}

bool RoomEventDispatcher::beginRoomShutdown(uint32_t roomId) {
    const std::shared_ptr<RoomDispatchState> state = findRoomState(roomId);
    if (state == nullptr) {
        return false;
    }

    state->queue.beginShutdown();
    return true;
}

bool RoomEventDispatcher::roomQueueStats(
    uint32_t roomId,
    RoomEventQueueStats& outStats) const {
    const std::shared_ptr<RoomDispatchState> state = findRoomState(roomId);
    if (state == nullptr) {
        return false;
    }

    outStats = state->queue.stats();
    return true;
}

std::shared_ptr<RoomEventDispatcher::RoomDispatchState>
RoomEventDispatcher::findRoomState(uint32_t roomId) const {
    std::lock_guard<std::mutex> lock(registryMutex_);
    auto it = rooms_.find(roomId);
    if (it == rooms_.end()) {
        return nullptr;
    }

    return it->second;
}

size_t RoomEventDispatcher::aggregateQueueDepth() const {
    size_t queueDepth = 0;
    std::lock_guard<std::mutex> lock(registryMutex_);
    for (const auto& roomEntry : rooms_) {
        queueDepth += roomEntry.second->queue.stats().roomEventQueueDepth;
    }
    return queueDepth;
}

void RoomEventDispatcher::scheduleRoom(uint32_t roomId) {
    std::lock_guard<std::mutex> lock(activeRoomMutex_);
    activeRooms_.push_back(roomId);
}
}  // namespace Game
