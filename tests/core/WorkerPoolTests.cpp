#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Game/WorkerPool.hpp"

namespace {
struct ProcessedEvent {
    uint32_t roomId{0};
    Game::RoomEvent event{};
};

class WorkerConcurrencyProbe {
public:
    void enter(uint32_t roomId) {
        std::lock_guard<std::mutex> lock(mutex_);
        const size_t activeForRoom = ++activeByRoom_[roomId];
        ++totalActive_;
        maxActiveByRoom_[roomId] = std::max(maxActiveByRoom_[roomId], activeForRoom);
        maxTotalActive_ = std::max(maxTotalActive_, totalActive_);
        if (activeForRoom > 1) {
            ++violationCount_;
        }
        condition_.notify_all();
    }

    void leave(uint32_t roomId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto activeIt = activeByRoom_.find(roomId);
        if (activeIt != activeByRoom_.end() && activeIt->second > 0) {
            --activeIt->second;
            --totalActive_;
        }
        condition_.notify_all();
    }

    bool waitForTotalActiveAtLeast(size_t expectedCount) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(
            lock,
            std::chrono::milliseconds(200),
            [this, expectedCount]() { return totalActive_ >= expectedCount; });
    }

    size_t maxActiveForRoom(uint32_t roomId) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = maxActiveByRoom_.find(roomId);
        return it == maxActiveByRoom_.end() ? 0 : it->second;
    }

    size_t maxTotalActive() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return maxTotalActive_;
    }

    size_t violationCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return violationCount_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::unordered_map<uint32_t, size_t> activeByRoom_;
    std::unordered_map<uint32_t, size_t> maxActiveByRoom_;
    size_t totalActive_{0};
    size_t maxTotalActive_{0};
    size_t violationCount_{0};
};

bool waitForProcessedCount(
    std::condition_variable& condition,
    std::mutex& mutex,
    const std::vector<ProcessedEvent>& processed,
    size_t expectedCount) {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(
        lock,
        std::chrono::seconds(2),
        [&processed, expectedCount]() { return processed.size() >= expectedCount; });
}

void expectRoomEventEquals(const Game::RoomEvent& actual, const Game::RoomEvent& expected) {
    EXPECT_EQ(actual.type, expected.type);
    EXPECT_EQ(actual.sessionId, expected.sessionId);
    EXPECT_EQ(actual.roomId, expected.roomId);
    EXPECT_EQ(actual.argument, expected.argument);
}
}  // namespace

TEST(WorkerPoolTests, ZeroWorkerPoolStartFails) {
    Game::RoomEventDispatcher dispatcher(2);
    Game::WorkerPool pool(0, dispatcher, [](uint32_t) {});

    EXPECT_FALSE(pool.start());
}

TEST(WorkerPoolTests, ScheduledRoomNotifyWakesHandler) {
    Game::RoomEventDispatcher dispatcher(4);
    ASSERT_TRUE(dispatcher.registerRoom(1));

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<ProcessedEvent> processed;
    Game::WorkerPool pool(
        1,
        dispatcher,
        [&dispatcher, &mutex, &condition, &processed](uint32_t roomId) {
            Game::RoomEvent event;
            if (dispatcher.tryDequeueRoomEvent(roomId, event)) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    processed.push_back(ProcessedEvent{roomId, event});
                }
                condition.notify_all();
            }
        });
    ASSERT_TRUE(pool.start());

    const Game::RoomEvent event = Game::makeReadyRoomEvent(10, 1);
    const Game::RoomEventDispatcherEnqueueResult enqueued = dispatcher.enqueue(event);
    ASSERT_EQ(enqueued.status, Game::RoomEventDispatcherEnqueueStatus::kEnqueued);
    ASSERT_TRUE(enqueued.scheduledRoom);
    pool.notifyRoomScheduled();

    ASSERT_TRUE(waitForProcessedCount(condition, mutex, processed, 1));
    pool.shutdownAndJoin();

    ASSERT_EQ(processed.size(), 1U);
    EXPECT_EQ(processed[0].roomId, 1U);
    expectRoomEventEquals(processed[0].event, event);
}

TEST(WorkerPoolTests, RecordsHandlerMetricsWhenMetricsIsConnected) {
    Game::RoomEventMetrics metrics;
    Game::RoomEventDispatcher dispatcher(4, &metrics);
    ASSERT_TRUE(dispatcher.registerRoom(1));

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<ProcessedEvent> processed;
    Game::WorkerPool pool(
        1,
        dispatcher,
        [&dispatcher, &mutex, &condition, &processed](uint32_t roomId) {
            Game::RoomEvent event;
            if (dispatcher.tryDequeueRoomEvent(roomId, event)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    processed.push_back(ProcessedEvent{roomId, event});
                }
                condition.notify_all();
            }
        },
        &metrics);
    ASSERT_TRUE(pool.start());

    const Game::RoomEvent event = Game::makeReadyRoomEvent(10, 1);
    const Game::RoomEventDispatcherEnqueueResult enqueued = dispatcher.enqueue(event);
    ASSERT_EQ(enqueued.status, Game::RoomEventDispatcherEnqueueStatus::kEnqueued);
    ASSERT_TRUE(enqueued.scheduledRoom);
    pool.notifyRoomScheduled();

    ASSERT_TRUE(waitForProcessedCount(condition, mutex, processed, 1));
    pool.shutdownAndJoin();

    const Game::RoomEventMetricsSnapshot snapshot = metrics.snapshot();
    EXPECT_EQ(snapshot.roomEventEnqueuedCount, 1U);
    EXPECT_EQ(snapshot.roomEventProcessedCount, 1U);
    EXPECT_EQ(snapshot.roomEventQueueDepth, 0U);
    EXPECT_EQ(snapshot.activeWorkerCount, 0U);
    EXPECT_GT(snapshot.roomEventProcessingTimeUs, 0U);
    EXPECT_GT(snapshot.workerBusyTimeUs, 0U);
}

TEST(WorkerPoolTests, DifferentActiveRoomsAreHandledThroughNotify) {
    Game::RoomEventDispatcher dispatcher(4);
    ASSERT_TRUE(dispatcher.registerRoom(1));
    ASSERT_TRUE(dispatcher.registerRoom(2));

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<ProcessedEvent> processed;
    Game::WorkerPool pool(
        2,
        dispatcher,
        [&dispatcher, &mutex, &condition, &processed](uint32_t roomId) {
            Game::RoomEvent event;
            if (dispatcher.tryDequeueRoomEvent(roomId, event)) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    processed.push_back(ProcessedEvent{roomId, event});
                }
                condition.notify_all();
            }
        });
    ASSERT_TRUE(pool.start());

    const Game::RoomEvent roomOne = Game::makeReadyRoomEvent(10, 1);
    const Game::RoomEvent roomTwo = Game::makeReadyRoomEvent(20, 2);
    ASSERT_TRUE(dispatcher.enqueue(roomOne).scheduledRoom);
    pool.notifyRoomScheduled();
    ASSERT_TRUE(dispatcher.enqueue(roomTwo).scheduledRoom);
    pool.notifyRoomScheduled();

    ASSERT_TRUE(waitForProcessedCount(condition, mutex, processed, 2));
    pool.shutdownAndJoin();

    std::vector<uint32_t> processedRoomIds;
    for (const ProcessedEvent& processedEvent : processed) {
        processedRoomIds.push_back(processedEvent.roomId);
    }
    std::sort(processedRoomIds.begin(), processedRoomIds.end());
    ASSERT_EQ(processedRoomIds.size(), 2U);
    EXPECT_EQ(processedRoomIds[0], 1U);
    EXPECT_EQ(processedRoomIds[1], 2U);
}

TEST(WorkerPoolTests, ReschedulesUntilRoomQueueDrains) {
    Game::RoomEventDispatcher dispatcher(4);
    ASSERT_TRUE(dispatcher.registerRoom(1));

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<ProcessedEvent> processed;
    Game::WorkerPool pool(
        1,
        dispatcher,
        [&dispatcher, &mutex, &condition, &processed](uint32_t roomId) {
            Game::RoomEvent event;
            if (dispatcher.tryDequeueRoomEvent(roomId, event)) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    processed.push_back(ProcessedEvent{roomId, event});
                }
                condition.notify_all();
            }
        });
    ASSERT_TRUE(pool.start());

    const Game::RoomEvent first = Game::makeReadyRoomEvent(10, 1);
    const Game::RoomEvent second = Game::makeMonsterDeathRoomEvent(10, 1, 7);
    const Game::RoomEvent third = Game::makeClickLootRoomEvent(20, 1, 9);
    ASSERT_TRUE(dispatcher.enqueue(first).scheduledRoom);
    ASSERT_FALSE(dispatcher.enqueue(second).scheduledRoom);
    ASSERT_FALSE(dispatcher.enqueue(third).scheduledRoom);
    pool.notifyRoomScheduled();

    ASSERT_TRUE(waitForProcessedCount(condition, mutex, processed, 3));
    pool.shutdownAndJoin();

    ASSERT_EQ(processed.size(), 3U);
    expectRoomEventEquals(processed[0].event, first);
    expectRoomEventEquals(processed[1].event, second);
    expectRoomEventEquals(processed[2].event, third);
}

TEST(WorkerPoolTests, ShutdownRejectsNewEnqueueAndRegister) {
    Game::RoomEventDispatcher dispatcher(2);
    ASSERT_TRUE(dispatcher.registerRoom(1));
    Game::WorkerPool pool(1, dispatcher, [](uint32_t) {});
    ASSERT_TRUE(pool.start());

    pool.shutdownAndJoin();

    const Game::RoomEvent event = Game::makeReadyRoomEvent(10, 1);
    const Game::RoomEventDispatcherEnqueueResult result = dispatcher.enqueue(event);
    EXPECT_EQ(result.status, Game::RoomEventDispatcherEnqueueStatus::kRejectedShutdown);
    EXPECT_EQ(result.queueResult, Game::RoomEventQueueEnqueueResult::kRejectedShutdown);
    EXPECT_FALSE(result.scheduledRoom);
    EXPECT_FALSE(dispatcher.registerRoom(2));
}

TEST(WorkerPoolTests, ShutdownDrainsAlreadyActiveEventsWithoutDuplicates) {
    Game::RoomEventDispatcher dispatcher(4);
    ASSERT_TRUE(dispatcher.registerRoom(1));

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<ProcessedEvent> processed;
    Game::WorkerPool pool(
        1,
        dispatcher,
        [&dispatcher, &mutex, &condition, &processed](uint32_t roomId) {
            Game::RoomEvent event;
            if (dispatcher.tryDequeueRoomEvent(roomId, event)) {
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    processed.push_back(ProcessedEvent{roomId, event});
                }
                condition.notify_all();
            }
        });
    ASSERT_TRUE(pool.start());

    const Game::RoomEvent first = Game::makeReadyRoomEvent(10, 1);
    const Game::RoomEvent second = Game::makeClickLootRoomEvent(20, 1, 9);
    ASSERT_TRUE(dispatcher.enqueue(first).scheduledRoom);
    ASSERT_FALSE(dispatcher.enqueue(second).scheduledRoom);
    pool.notifyRoomScheduled();

    pool.shutdownAndJoin();

    ASSERT_EQ(processed.size(), 2U);
    expectRoomEventEquals(processed[0].event, first);
    expectRoomEventEquals(processed[1].event, second);
}

TEST(WorkerPoolTests, ShutdownDrainsMultipleRoomsAndRescheduledEventsWithoutDuplicates) {
    constexpr uint32_t kRoomOneId = 1;
    constexpr uint32_t kRoomTwoId = 2;
    Game::RoomEventDispatcher dispatcher(8);
    ASSERT_TRUE(dispatcher.registerRoom(kRoomOneId));
    ASSERT_TRUE(dispatcher.registerRoom(kRoomTwoId));

    const std::vector<Game::RoomEvent> roomOneEvents{
        Game::makeReadyRoomEvent(10, kRoomOneId),
        Game::makeMonsterDeathRoomEvent(10, kRoomOneId, 7),
        Game::makeClickLootRoomEvent(20, kRoomOneId, 9),
    };
    const std::vector<Game::RoomEvent> roomTwoEvents{
        Game::makeReadyRoomEvent(30, kRoomTwoId),
        Game::makeMonsterDeathRoomEvent(30, kRoomTwoId, 8),
        Game::makeClickLootRoomEvent(40, kRoomTwoId, 10),
    };

    std::vector<bool> notifyScheduledRooms;
    notifyScheduledRooms.push_back(dispatcher.enqueue(roomOneEvents[0]).scheduledRoom);
    notifyScheduledRooms.push_back(dispatcher.enqueue(roomTwoEvents[0]).scheduledRoom);
    ASSERT_FALSE(dispatcher.enqueue(roomOneEvents[1]).scheduledRoom);
    ASSERT_FALSE(dispatcher.enqueue(roomTwoEvents[1]).scheduledRoom);
    ASSERT_FALSE(dispatcher.enqueue(roomOneEvents[2]).scheduledRoom);
    ASSERT_FALSE(dispatcher.enqueue(roomTwoEvents[2]).scheduledRoom);
    ASSERT_EQ(notifyScheduledRooms.size(), 2U);
    EXPECT_TRUE(notifyScheduledRooms[0]);
    EXPECT_TRUE(notifyScheduledRooms[1]);

    std::mutex mutex;
    std::vector<ProcessedEvent> processed;
    Game::WorkerPool pool(
        2,
        dispatcher,
        [&dispatcher, &mutex, &processed](uint32_t roomId) {
            Game::RoomEvent event;
            if (dispatcher.tryDequeueRoomEvent(roomId, event)) {
                std::lock_guard<std::mutex> lock(mutex);
                processed.push_back(ProcessedEvent{roomId, event});
            }
        });
    ASSERT_TRUE(pool.start());

    for (bool shouldNotify : notifyScheduledRooms) {
        if (shouldNotify) {
            pool.notifyRoomScheduled();
        }
    }

    pool.shutdownAndJoin();

    ASSERT_EQ(processed.size(), roomOneEvents.size() + roomTwoEvents.size());
    std::vector<Game::RoomEvent> processedRoomOne;
    std::vector<Game::RoomEvent> processedRoomTwo;
    for (const ProcessedEvent& processedEvent : processed) {
        if (processedEvent.roomId == kRoomOneId) {
            processedRoomOne.push_back(processedEvent.event);
        } else if (processedEvent.roomId == kRoomTwoId) {
            processedRoomTwo.push_back(processedEvent.event);
        } else {
            ADD_FAILURE() << "unexpected roomId=" << processedEvent.roomId;
        }
    }

    ASSERT_EQ(processedRoomOne.size(), roomOneEvents.size());
    ASSERT_EQ(processedRoomTwo.size(), roomTwoEvents.size());
    for (size_t i = 0; i < roomOneEvents.size(); ++i) {
        expectRoomEventEquals(processedRoomOne[i], roomOneEvents[i]);
        expectRoomEventEquals(processedRoomTwo[i], roomTwoEvents[i]);
    }

    Game::RoomEventQueueStats stats;
    ASSERT_TRUE(dispatcher.roomQueueStats(kRoomOneId, stats));
    EXPECT_EQ(stats.roomEventQueueDepth, 0U);
    ASSERT_TRUE(dispatcher.roomQueueStats(kRoomTwoId, stats));
    EXPECT_EQ(stats.roomEventQueueDepth, 0U);

    const Game::RoomEventDispatcherEnqueueResult enqueueAfterShutdown =
        dispatcher.enqueue(Game::makeReadyRoomEvent(99, kRoomOneId));
    EXPECT_EQ(
        enqueueAfterShutdown.status,
        Game::RoomEventDispatcherEnqueueStatus::kRejectedShutdown);
    EXPECT_EQ(
        enqueueAfterShutdown.queueResult,
        Game::RoomEventQueueEnqueueResult::kRejectedShutdown);
    EXPECT_FALSE(enqueueAfterShutdown.scheduledRoom);
    EXPECT_FALSE(dispatcher.registerRoom(3));
}

TEST(WorkerPoolTests, MultiRoomShutdownDrainRecordsMetricsBaseline) {
    constexpr uint32_t kRoomOneId = 1;
    constexpr uint32_t kRoomTwoId = 2;
    Game::RoomEventMetrics metrics;
    Game::RoomEventDispatcher dispatcher(8, &metrics);
    ASSERT_TRUE(dispatcher.registerRoom(kRoomOneId));
    ASSERT_TRUE(dispatcher.registerRoom(kRoomTwoId));

    const std::vector<Game::RoomEvent> roomOneEvents{
        Game::makeReadyRoomEvent(10, kRoomOneId),
        Game::makeMonsterDeathRoomEvent(10, kRoomOneId, 7),
        Game::makeClickLootRoomEvent(20, kRoomOneId, 9),
    };
    const std::vector<Game::RoomEvent> roomTwoEvents{
        Game::makeReadyRoomEvent(30, kRoomTwoId),
        Game::makeMonsterDeathRoomEvent(30, kRoomTwoId, 8),
        Game::makeClickLootRoomEvent(40, kRoomTwoId, 10),
    };

    std::vector<bool> notifyScheduledRooms;
    notifyScheduledRooms.push_back(dispatcher.enqueue(roomOneEvents[0]).scheduledRoom);
    notifyScheduledRooms.push_back(dispatcher.enqueue(roomTwoEvents[0]).scheduledRoom);
    ASSERT_FALSE(dispatcher.enqueue(roomOneEvents[1]).scheduledRoom);
    ASSERT_FALSE(dispatcher.enqueue(roomTwoEvents[1]).scheduledRoom);
    ASSERT_FALSE(dispatcher.enqueue(roomOneEvents[2]).scheduledRoom);
    ASSERT_FALSE(dispatcher.enqueue(roomTwoEvents[2]).scheduledRoom);
    EXPECT_TRUE(notifyScheduledRooms[0]);
    EXPECT_TRUE(notifyScheduledRooms[1]);

    std::mutex mutex;
    std::vector<ProcessedEvent> processed;
    Game::WorkerPool pool(
        2,
        dispatcher,
        [&dispatcher, &mutex, &processed](uint32_t roomId) {
            Game::RoomEvent event;
            if (dispatcher.tryDequeueRoomEvent(roomId, event)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                std::lock_guard<std::mutex> lock(mutex);
                processed.push_back(ProcessedEvent{roomId, event});
            }
        },
        &metrics);
    ASSERT_TRUE(pool.start());

    for (bool shouldNotify : notifyScheduledRooms) {
        if (shouldNotify) {
            pool.notifyRoomScheduled();
        }
    }

    pool.shutdownAndJoin();
    ASSERT_EQ(processed.size(), roomOneEvents.size() + roomTwoEvents.size());

    const Game::RoomEventDispatcherEnqueueResult enqueueAfterShutdown =
        dispatcher.enqueue(Game::makeReadyRoomEvent(99, kRoomOneId));
    EXPECT_EQ(
        enqueueAfterShutdown.status,
        Game::RoomEventDispatcherEnqueueStatus::kRejectedShutdown);

    const Game::RoomEventMetricsSnapshot snapshot = metrics.snapshot();
    EXPECT_EQ(snapshot.roomEventEnqueuedCount, 6U);
    EXPECT_EQ(snapshot.roomEventProcessedCount, 6U);
    EXPECT_EQ(snapshot.roomScheduledContentionCount, 4U);
    EXPECT_EQ(snapshot.roomEventRejectedBackpressureCount, 0U);
    EXPECT_EQ(snapshot.roomEventRejectedShutdownCount, 1U);
    EXPECT_EQ(snapshot.roomEventQueueDepth, 0U);
    EXPECT_EQ(snapshot.roomEventMaxQueueDepth, 6U);
    EXPECT_EQ(snapshot.activeWorkerCount, 0U);
    EXPECT_EQ(snapshot.roomEventLatencyUs, 0U);
    EXPECT_GT(snapshot.roomEventProcessingTimeUs, 0U);
    EXPECT_EQ(snapshot.lockWaitTimeUs, 0U);
    EXPECT_GT(snapshot.workerBusyTimeUs, 0U);
    EXPECT_DOUBLE_EQ(snapshot.workerUtilization, 0.0);

    const Game::RoomEventMetricsSnapshot derivedSnapshot =
        metrics.snapshot(snapshot.workerBusyTimeUs * 2);
    EXPECT_DOUBLE_EQ(derivedSnapshot.workerUtilization, 0.5);
}

TEST(WorkerPoolTests, ShutdownAndJoinIsIdempotent) {
    Game::RoomEventDispatcher dispatcher(2);
    ASSERT_TRUE(dispatcher.registerRoom(1));
    Game::WorkerPool pool(1, dispatcher, [](uint32_t) {});
    ASSERT_TRUE(pool.start());

    pool.shutdownAndJoin();
    pool.shutdownAndJoin();

    const Game::RoomEvent event = Game::makeReadyRoomEvent(10, 1);
    EXPECT_EQ(
        dispatcher.enqueue(event).status,
        Game::RoomEventDispatcherEnqueueStatus::kRejectedShutdown);
}

TEST(WorkerPoolTests, SameRoomIsNeverHandledConcurrentlyWithExtraWakeTokens) {
    constexpr size_t kWorkerCount = 4;
    constexpr size_t kEventCount = 32;
    Game::RoomEventDispatcher dispatcher(kEventCount);
    ASSERT_TRUE(dispatcher.registerRoom(1));

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<ProcessedEvent> processed;
    WorkerConcurrencyProbe probe;
    Game::WorkerPool pool(
        kWorkerCount,
        dispatcher,
        [&dispatcher, &mutex, &condition, &processed, &probe](uint32_t roomId) {
            probe.enter(roomId);
            Game::RoomEvent event;
            if (dispatcher.tryDequeueRoomEvent(roomId, event)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    processed.push_back(ProcessedEvent{roomId, event});
                }
                condition.notify_all();
            }
            probe.leave(roomId);
        });
    ASSERT_TRUE(pool.start());

    for (size_t i = 0; i < kEventCount; ++i) {
        const Game::RoomEvent event = Game::makeClickLootRoomEvent(
            static_cast<uint64_t>(i + 1),
            1,
            static_cast<uint32_t>(i + 1));
        const Game::RoomEventDispatcherEnqueueResult result = dispatcher.enqueue(event);
        ASSERT_EQ(result.status, Game::RoomEventDispatcherEnqueueStatus::kEnqueued);
        if (result.scheduledRoom) {
            pool.notifyRoomScheduled();
        }
    }

    for (size_t i = 0; i < kWorkerCount * 4; ++i) {
        pool.notifyRoomScheduled();
    }

    ASSERT_TRUE(waitForProcessedCount(condition, mutex, processed, kEventCount));
    pool.shutdownAndJoin();

    EXPECT_EQ(processed.size(), kEventCount);
    EXPECT_EQ(probe.maxActiveForRoom(1), 1U);
    EXPECT_EQ(probe.violationCount(), 0U);
}

TEST(WorkerPoolTests, DifferentRoomsCanBeHandledByMultipleWorkersSmoke) {
    Game::RoomEventDispatcher dispatcher(4);
    ASSERT_TRUE(dispatcher.registerRoom(1));
    ASSERT_TRUE(dispatcher.registerRoom(2));

    std::mutex mutex;
    std::condition_variable condition;
    std::vector<ProcessedEvent> processed;
    WorkerConcurrencyProbe probe;
    Game::WorkerPool pool(
        2,
        dispatcher,
        [&dispatcher, &mutex, &condition, &processed, &probe](uint32_t roomId) {
            probe.enter(roomId);
            probe.waitForTotalActiveAtLeast(2);
            Game::RoomEvent event;
            if (dispatcher.tryDequeueRoomEvent(roomId, event)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    processed.push_back(ProcessedEvent{roomId, event});
                }
                condition.notify_all();
            }
            probe.leave(roomId);
        });
    ASSERT_TRUE(pool.start());

    ASSERT_TRUE(dispatcher.enqueue(Game::makeReadyRoomEvent(10, 1)).scheduledRoom);
    pool.notifyRoomScheduled();
    ASSERT_TRUE(dispatcher.enqueue(Game::makeReadyRoomEvent(20, 2)).scheduledRoom);
    pool.notifyRoomScheduled();

    ASSERT_TRUE(waitForProcessedCount(condition, mutex, processed, 2));
    pool.shutdownAndJoin();

    EXPECT_EQ(processed.size(), 2U);
    EXPECT_GE(probe.maxTotalActive(), 2U);
    EXPECT_EQ(probe.maxActiveForRoom(1), 1U);
    EXPECT_EQ(probe.maxActiveForRoom(2), 1U);
    EXPECT_EQ(probe.violationCount(), 0U);
}
