#include <gtest/gtest.h>

#include <cstddef>
#include <thread>
#include <vector>

#include "Game/RoomEventMetrics.hpp"

TEST(RoomEventMetricsTests, RecordsCountersGaugesDurationsAndDerivedUtilization) {
    Game::RoomEventMetrics metrics;

    metrics.recordEnqueued(1, 1);
    metrics.recordEnqueued(2, 2);
    metrics.recordRejectedBackpressure();
    metrics.recordRejectedShutdown();
    metrics.recordProcessed();
    metrics.recordScheduledContention();
    metrics.recordEventLatencyUs(10);
    metrics.recordEventLatencyUs(15);
    metrics.recordEventProcessingTimeUs(30);
    metrics.recordLockWaitTimeUs(40);
    metrics.recordWorkerBusyTimeUs(50);
    metrics.workerStarted();
    metrics.workerStarted();
    metrics.workerFinished();

    const Game::RoomEventMetricsSnapshot snapshot = metrics.snapshot(100);

    EXPECT_EQ(snapshot.roomEventEnqueuedCount, 2U);
    EXPECT_EQ(snapshot.roomEventProcessedCount, 1U);
    EXPECT_EQ(snapshot.roomEventRejectedBackpressureCount, 1U);
    EXPECT_EQ(snapshot.roomEventRejectedShutdownCount, 1U);
    EXPECT_EQ(snapshot.roomScheduledContentionCount, 1U);
    EXPECT_EQ(snapshot.roomEventQueueDepth, 2U);
    EXPECT_EQ(snapshot.roomEventMaxQueueDepth, 2U);
    EXPECT_EQ(snapshot.activeWorkerCount, 1U);
    EXPECT_EQ(snapshot.roomEventLatencyUs, 25U);
    EXPECT_EQ(snapshot.roomEventProcessingTimeUs, 30U);
    EXPECT_EQ(snapshot.lockWaitTimeUs, 40U);
    EXPECT_EQ(snapshot.workerBusyTimeUs, 50U);
    EXPECT_DOUBLE_EQ(snapshot.workerUtilization, 0.5);
}

TEST(RoomEventMetricsTests, WorkerUtilizationIsDerivedFromObservedTime) {
    Game::RoomEventMetrics metrics;
    metrics.recordWorkerBusyTimeUs(125);

    EXPECT_DOUBLE_EQ(metrics.snapshot().workerUtilization, 0.0);
    EXPECT_DOUBLE_EQ(metrics.snapshot(250).workerUtilization, 0.5);
}

TEST(RoomEventMetricsTests, ConcurrentRecordsPreserveCounterAndDurationTotals) {
    constexpr size_t kThreadCount = 4;
    constexpr size_t kRecordsPerThread = 1000;
    Game::RoomEventMetrics metrics;
    std::vector<std::thread> threads;
    threads.reserve(kThreadCount);

    for (size_t threadIndex = 0; threadIndex < kThreadCount; ++threadIndex) {
        threads.emplace_back([&metrics]() {
            for (size_t i = 0; i < kRecordsPerThread; ++i) {
                metrics.recordProcessed();
                metrics.recordScheduledContention();
                metrics.recordEventLatencyUs(1);
                metrics.recordEventProcessingTimeUs(2);
                metrics.recordLockWaitTimeUs(3);
                metrics.recordWorkerBusyTimeUs(4);
            }
        });
    }

    for (std::thread& thread : threads) {
        thread.join();
    }

    const Game::RoomEventMetricsSnapshot snapshot = metrics.snapshot(16000);
    const size_t expectedCount = kThreadCount * kRecordsPerThread;
    EXPECT_EQ(snapshot.roomEventProcessedCount, expectedCount);
    EXPECT_EQ(snapshot.roomScheduledContentionCount, expectedCount);
    EXPECT_EQ(snapshot.roomEventLatencyUs, expectedCount);
    EXPECT_EQ(snapshot.roomEventProcessingTimeUs, expectedCount * 2);
    EXPECT_EQ(snapshot.lockWaitTimeUs, expectedCount * 3);
    EXPECT_EQ(snapshot.workerBusyTimeUs, expectedCount * 4);
    EXPECT_DOUBLE_EQ(snapshot.workerUtilization, 1.0);
}
