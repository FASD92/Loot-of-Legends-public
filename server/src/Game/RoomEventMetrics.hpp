#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Game {
struct RoomEventMetricsSnapshot {
    size_t roomEventEnqueuedCount{0};
    size_t roomEventProcessedCount{0};
    size_t roomEventRejectedBackpressureCount{0};
    size_t roomEventRejectedShutdownCount{0};
    size_t roomScheduledContentionCount{0};
    size_t roomEventQueueDepth{0};
    size_t roomEventMaxQueueDepth{0};
    size_t activeWorkerCount{0};
    uint64_t roomEventLatencyUs{0};
    uint64_t roomEventProcessingTimeUs{0};
    uint64_t lockWaitTimeUs{0};
    uint64_t workerBusyTimeUs{0};
    double workerUtilization{0.0};
};

class RoomEventMetrics {
public:
    void recordEnqueued(size_t queueDepth, size_t maxQueueDepth);
    void recordQueueDepth(size_t queueDepth, size_t maxQueueDepth);
    void recordRejectedBackpressure();
    void recordRejectedShutdown();
    void recordProcessed();
    void recordScheduledContention();
    void recordEventLatencyUs(uint64_t latencyUs);
    void recordEventProcessingTimeUs(uint64_t processingTimeUs);
    void recordLockWaitTimeUs(uint64_t lockWaitTimeUs);
    void recordWorkerBusyTimeUs(uint64_t busyTimeUs);
    void workerStarted();
    void workerFinished();

    RoomEventMetricsSnapshot snapshot(uint64_t observedTimeUs = 0) const;

private:
    void updateMaxQueueDepth(size_t maxQueueDepth);

    std::atomic_size_t roomEventEnqueuedCount_{0};
    std::atomic_size_t roomEventProcessedCount_{0};
    std::atomic_size_t roomEventRejectedBackpressureCount_{0};
    std::atomic_size_t roomEventRejectedShutdownCount_{0};
    std::atomic_size_t roomScheduledContentionCount_{0};
    std::atomic_size_t roomEventQueueDepth_{0};
    std::atomic_size_t roomEventMaxQueueDepth_{0};
    std::atomic_size_t activeWorkerCount_{0};
    std::atomic_uint64_t roomEventLatencyUs_{0};
    std::atomic_uint64_t roomEventProcessingTimeUs_{0};
    std::atomic_uint64_t lockWaitTimeUs_{0};
    std::atomic_uint64_t workerBusyTimeUs_{0};
};
}  // namespace Game
