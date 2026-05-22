#include "Game/RoomEventMetrics.hpp"

namespace Game {
void RoomEventMetrics::recordEnqueued(size_t queueDepth, size_t maxQueueDepth) {
    roomEventEnqueuedCount_.fetch_add(1, std::memory_order_relaxed);
    recordQueueDepth(queueDepth, maxQueueDepth);
}

void RoomEventMetrics::recordQueueDepth(size_t queueDepth, size_t maxQueueDepth) {
    roomEventQueueDepth_.store(queueDepth, std::memory_order_relaxed);
    updateMaxQueueDepth(maxQueueDepth);
}

void RoomEventMetrics::recordRejectedBackpressure() {
    roomEventRejectedBackpressureCount_.fetch_add(1, std::memory_order_relaxed);
}

void RoomEventMetrics::recordRejectedShutdown() {
    roomEventRejectedShutdownCount_.fetch_add(1, std::memory_order_relaxed);
}

void RoomEventMetrics::recordProcessed() {
    roomEventProcessedCount_.fetch_add(1, std::memory_order_relaxed);
}

void RoomEventMetrics::recordScheduledContention() {
    roomScheduledContentionCount_.fetch_add(1, std::memory_order_relaxed);
}

void RoomEventMetrics::recordEventLatencyUs(uint64_t latencyUs) {
    roomEventLatencyUs_.fetch_add(latencyUs, std::memory_order_relaxed);
}

void RoomEventMetrics::recordEventProcessingTimeUs(uint64_t processingTimeUs) {
    roomEventProcessingTimeUs_.fetch_add(processingTimeUs, std::memory_order_relaxed);
}

void RoomEventMetrics::recordLockWaitTimeUs(uint64_t lockWaitTimeUs) {
    lockWaitTimeUs_.fetch_add(lockWaitTimeUs, std::memory_order_relaxed);
}

void RoomEventMetrics::recordWorkerBusyTimeUs(uint64_t busyTimeUs) {
    workerBusyTimeUs_.fetch_add(busyTimeUs, std::memory_order_relaxed);
}

void RoomEventMetrics::workerStarted() {
    activeWorkerCount_.fetch_add(1, std::memory_order_relaxed);
}

void RoomEventMetrics::workerFinished() {
    size_t current = activeWorkerCount_.load(std::memory_order_relaxed);
    while (current > 0 &&
           !activeWorkerCount_.compare_exchange_weak(
               current,
               current - 1,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

RoomEventMetricsSnapshot RoomEventMetrics::snapshot(uint64_t observedTimeUs) const {
    const uint64_t workerBusyTimeUs =
        workerBusyTimeUs_.load(std::memory_order_relaxed);
    const double workerUtilization = observedTimeUs == 0
        ? 0.0
        : static_cast<double>(workerBusyTimeUs) /
              static_cast<double>(observedTimeUs);

    return RoomEventMetricsSnapshot{
        roomEventEnqueuedCount_.load(std::memory_order_relaxed),
        roomEventProcessedCount_.load(std::memory_order_relaxed),
        roomEventRejectedBackpressureCount_.load(std::memory_order_relaxed),
        roomEventRejectedShutdownCount_.load(std::memory_order_relaxed),
        roomScheduledContentionCount_.load(std::memory_order_relaxed),
        roomEventQueueDepth_.load(std::memory_order_relaxed),
        roomEventMaxQueueDepth_.load(std::memory_order_relaxed),
        activeWorkerCount_.load(std::memory_order_relaxed),
        roomEventLatencyUs_.load(std::memory_order_relaxed),
        roomEventProcessingTimeUs_.load(std::memory_order_relaxed),
        lockWaitTimeUs_.load(std::memory_order_relaxed),
        workerBusyTimeUs,
        workerUtilization};
}

void RoomEventMetrics::updateMaxQueueDepth(size_t maxQueueDepth) {
    size_t current = roomEventMaxQueueDepth_.load(std::memory_order_relaxed);
    while (current < maxQueueDepth &&
           !roomEventMaxQueueDepth_.compare_exchange_weak(
               current,
               maxQueueDepth,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}
}  // namespace Game
