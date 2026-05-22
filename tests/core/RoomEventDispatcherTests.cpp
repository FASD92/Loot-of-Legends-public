#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

#include "Game/RoomEventDispatcher.hpp"
#include "Game/RoomEventMetrics.hpp"

namespace {
void expectRoomEventEquals(const Game::RoomEvent& actual, const Game::RoomEvent& expected) {
    EXPECT_EQ(actual.type, expected.type);
    EXPECT_EQ(actual.sessionId, expected.sessionId);
    EXPECT_EQ(actual.roomId, expected.roomId);
    EXPECT_EQ(actual.argument, expected.argument);
}
}  // namespace

TEST(RoomEventDispatcherTests, RoutesRegisteredRoomEventAndSchedulesOnce) {
    Game::RoomEventDispatcher dispatcher(4);
    ASSERT_TRUE(dispatcher.registerRoom(7));
    const Game::RoomEvent event = Game::makeReadyRoomEvent(10, 7);

    const Game::RoomEventDispatcherEnqueueResult result = dispatcher.enqueue(event);

    EXPECT_EQ(result.status, Game::RoomEventDispatcherEnqueueStatus::kEnqueued);
    EXPECT_EQ(result.queueResult, Game::RoomEventQueueEnqueueResult::kEnqueued);
    EXPECT_TRUE(result.scheduledRoom);

    uint32_t activeRoomId = 0;
    ASSERT_TRUE(dispatcher.tryPopActiveRoom(activeRoomId));
    EXPECT_EQ(activeRoomId, 7U);
    EXPECT_FALSE(dispatcher.tryPopActiveRoom(activeRoomId));

    Game::RoomEvent dequeued;
    ASSERT_TRUE(dispatcher.tryDequeueRoomEvent(7, dequeued));
    expectRoomEventEquals(dequeued, event);
}

TEST(RoomEventDispatcherTests, SameRoomEnqueuesDoNotDuplicateActiveRoomAndPreserveFifo) {
    Game::RoomEventDispatcher dispatcher(4);
    ASSERT_TRUE(dispatcher.registerRoom(1));
    const Game::RoomEvent ready = Game::makeReadyRoomEvent(10, 1);
    const Game::RoomEvent monsterDeath = Game::makeMonsterDeathRoomEvent(10, 1, 5);
    const Game::RoomEvent clickLoot = Game::makeClickLootRoomEvent(20, 1, 9);

    EXPECT_TRUE(dispatcher.enqueue(ready).scheduledRoom);
    EXPECT_FALSE(dispatcher.enqueue(monsterDeath).scheduledRoom);
    EXPECT_FALSE(dispatcher.enqueue(clickLoot).scheduledRoom);

    uint32_t activeRoomId = 0;
    ASSERT_TRUE(dispatcher.tryPopActiveRoom(activeRoomId));
    EXPECT_EQ(activeRoomId, 1U);
    EXPECT_FALSE(dispatcher.tryPopActiveRoom(activeRoomId));

    Game::RoomEvent dequeued;
    ASSERT_TRUE(dispatcher.tryDequeueRoomEvent(1, dequeued));
    expectRoomEventEquals(dequeued, ready);
    ASSERT_TRUE(dispatcher.tryDequeueRoomEvent(1, dequeued));
    expectRoomEventEquals(dequeued, monsterDeath);
    ASSERT_TRUE(dispatcher.tryDequeueRoomEvent(1, dequeued));
    expectRoomEventEquals(dequeued, clickLoot);
}

TEST(RoomEventDispatcherTests, DifferentRoomsBecomeSeparateActiveCandidates) {
    Game::RoomEventDispatcher dispatcher(4);
    ASSERT_TRUE(dispatcher.registerRoom(1));
    ASSERT_TRUE(dispatcher.registerRoom(2));

    const Game::RoomEvent roomOne = Game::makeReadyRoomEvent(10, 1);
    const Game::RoomEvent roomTwo = Game::makeReadyRoomEvent(20, 2);

    EXPECT_TRUE(dispatcher.enqueue(roomOne).scheduledRoom);
    EXPECT_TRUE(dispatcher.enqueue(roomTwo).scheduledRoom);

    uint32_t activeRoomId = 0;
    ASSERT_TRUE(dispatcher.tryPopActiveRoom(activeRoomId));
    EXPECT_EQ(activeRoomId, 1U);
    ASSERT_TRUE(dispatcher.tryPopActiveRoom(activeRoomId));
    EXPECT_EQ(activeRoomId, 2U);
    EXPECT_FALSE(dispatcher.tryPopActiveRoom(activeRoomId));

    Game::RoomEvent dequeued;
    ASSERT_TRUE(dispatcher.tryDequeueRoomEvent(1, dequeued));
    expectRoomEventEquals(dequeued, roomOne);
    ASSERT_TRUE(dispatcher.tryDequeueRoomEvent(2, dequeued));
    expectRoomEventEquals(dequeued, roomTwo);
}

TEST(RoomEventDispatcherTests, UnknownRoomRejectDoesNotChangeQueues) {
    Game::RoomEventDispatcher dispatcher(4);
    ASSERT_TRUE(dispatcher.registerRoom(1));

    const Game::RoomEvent resultEvent = Game::makeReadyRoomEvent(10, 99);
    const Game::RoomEventDispatcherEnqueueResult result = dispatcher.enqueue(resultEvent);

    EXPECT_EQ(result.status, Game::RoomEventDispatcherEnqueueStatus::kRejectedUnknownRoom);
    EXPECT_FALSE(result.scheduledRoom);

    uint32_t activeRoomId = 0;
    EXPECT_FALSE(dispatcher.tryPopActiveRoom(activeRoomId));

    Game::RoomEvent dequeued;
    EXPECT_FALSE(dispatcher.tryDequeueRoomEvent(1, dequeued));
}

TEST(RoomEventDispatcherTests, BackpressureRejectDoesNotAddActiveRoomAgain) {
    Game::RoomEventDispatcher dispatcher(1);
    ASSERT_TRUE(dispatcher.registerRoom(1));

    const Game::RoomEvent first = Game::makeReadyRoomEvent(10, 1);
    const Game::RoomEvent second = Game::makeMonsterDeathRoomEvent(10, 1, 5);

    const Game::RoomEventDispatcherEnqueueResult firstResult = dispatcher.enqueue(first);
    ASSERT_EQ(firstResult.status, Game::RoomEventDispatcherEnqueueStatus::kEnqueued);
    ASSERT_TRUE(firstResult.scheduledRoom);

    const Game::RoomEventDispatcherEnqueueResult secondResult = dispatcher.enqueue(second);
    EXPECT_EQ(
        secondResult.status,
        Game::RoomEventDispatcherEnqueueStatus::kRejectedBackpressure);
    EXPECT_EQ(
        secondResult.queueResult,
        Game::RoomEventQueueEnqueueResult::kRejectedBackpressure);
    EXPECT_FALSE(secondResult.scheduledRoom);

    uint32_t activeRoomId = 0;
    ASSERT_TRUE(dispatcher.tryPopActiveRoom(activeRoomId));
    EXPECT_EQ(activeRoomId, 1U);
    EXPECT_FALSE(dispatcher.tryPopActiveRoom(activeRoomId));

    Game::RoomEvent dequeued;
    ASSERT_TRUE(dispatcher.tryDequeueRoomEvent(1, dequeued));
    expectRoomEventEquals(dequeued, first);
    EXPECT_FALSE(dispatcher.tryDequeueRoomEvent(1, dequeued));
}

TEST(RoomEventDispatcherTests, ShutdownRejectIsPropagatedFromRoomQueue) {
    Game::RoomEventDispatcher dispatcher(2);
    ASSERT_TRUE(dispatcher.registerRoom(1));
    ASSERT_TRUE(dispatcher.beginRoomShutdown(1));

    const Game::RoomEvent event = Game::makeReadyRoomEvent(10, 1);
    const Game::RoomEventDispatcherEnqueueResult result = dispatcher.enqueue(event);

    EXPECT_EQ(result.status, Game::RoomEventDispatcherEnqueueStatus::kRejectedShutdown);
    EXPECT_EQ(result.queueResult, Game::RoomEventQueueEnqueueResult::kRejectedShutdown);
    EXPECT_FALSE(result.scheduledRoom);

    uint32_t activeRoomId = 0;
    EXPECT_FALSE(dispatcher.tryPopActiveRoom(activeRoomId));
}

TEST(RoomEventDispatcherTests, CompleteRoomProcessingClearsScheduleWhenQueueIsEmpty) {
    Game::RoomEventDispatcher dispatcher(2);
    ASSERT_TRUE(dispatcher.registerRoom(1));

    const Game::RoomEvent first = Game::makeReadyRoomEvent(10, 1);
    ASSERT_TRUE(dispatcher.enqueue(first).scheduledRoom);

    uint32_t activeRoomId = 0;
    ASSERT_TRUE(dispatcher.tryPopActiveRoom(activeRoomId));
    EXPECT_EQ(activeRoomId, 1U);

    Game::RoomEvent dequeued;
    ASSERT_TRUE(dispatcher.tryDequeueRoomEvent(1, dequeued));
    expectRoomEventEquals(dequeued, first);
    const Game::RoomEventDispatcherCompletionResult completed =
        dispatcher.completeRoomProcessing(1);
    EXPECT_TRUE(completed.knownRoom);
    EXPECT_FALSE(completed.rescheduledRoom);

    const Game::RoomEvent second = Game::makeClickLootRoomEvent(10, 1, 9);
    EXPECT_TRUE(dispatcher.enqueue(second).scheduledRoom);
    ASSERT_TRUE(dispatcher.tryPopActiveRoom(activeRoomId));
    EXPECT_EQ(activeRoomId, 1U);
}

TEST(RoomEventDispatcherTests, CompleteRoomProcessingReschedulesWhenEventsRemain) {
    Game::RoomEventDispatcher dispatcher(3);
    ASSERT_TRUE(dispatcher.registerRoom(1));

    const Game::RoomEvent first = Game::makeReadyRoomEvent(10, 1);
    const Game::RoomEvent second = Game::makeClickLootRoomEvent(20, 1, 9);
    ASSERT_TRUE(dispatcher.enqueue(first).scheduledRoom);
    ASSERT_FALSE(dispatcher.enqueue(second).scheduledRoom);

    uint32_t activeRoomId = 0;
    ASSERT_TRUE(dispatcher.tryPopActiveRoom(activeRoomId));
    EXPECT_EQ(activeRoomId, 1U);

    Game::RoomEvent dequeued;
    ASSERT_TRUE(dispatcher.tryDequeueRoomEvent(1, dequeued));
    expectRoomEventEquals(dequeued, first);
    const Game::RoomEventDispatcherCompletionResult firstCompleted =
        dispatcher.completeRoomProcessing(1);
    EXPECT_TRUE(firstCompleted.knownRoom);
    EXPECT_TRUE(firstCompleted.rescheduledRoom);

    ASSERT_TRUE(dispatcher.tryPopActiveRoom(activeRoomId));
    EXPECT_EQ(activeRoomId, 1U);
    EXPECT_FALSE(dispatcher.tryPopActiveRoom(activeRoomId));

    ASSERT_TRUE(dispatcher.tryDequeueRoomEvent(1, dequeued));
    expectRoomEventEquals(dequeued, second);
    const Game::RoomEventDispatcherCompletionResult secondCompleted =
        dispatcher.completeRoomProcessing(1);
    EXPECT_TRUE(secondCompleted.knownRoom);
    EXPECT_FALSE(secondCompleted.rescheduledRoom);

    const Game::RoomEvent third = Game::makeMonsterDeathRoomEvent(10, 1, 5);
    EXPECT_TRUE(dispatcher.enqueue(third).scheduledRoom);
}

TEST(RoomEventDispatcherTests, ConcurrentProducersScheduleSameRoomOnlyOnce) {
    constexpr size_t kCapacity = 128;
    constexpr size_t kProducerCount = 4;
    constexpr size_t kAttemptsPerProducer = 50;
    Game::RoomEventDispatcher dispatcher(kCapacity);
    ASSERT_TRUE(dispatcher.registerRoom(1));

    std::atomic<size_t> scheduledCount{0};
    std::atomic<size_t> enqueuedCount{0};
    std::atomic<size_t> backpressureCount{0};
    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);

    for (size_t producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&dispatcher, &scheduledCount, &enqueuedCount,
                                &backpressureCount, producer]() {
            for (size_t attempt = 0; attempt < kAttemptsPerProducer; ++attempt) {
                const Game::RoomEvent event = Game::makeClickLootRoomEvent(
                    static_cast<uint64_t>(producer + 1),
                    1,
                    static_cast<uint32_t>(attempt + 1));
                const Game::RoomEventDispatcherEnqueueResult result =
                    dispatcher.enqueue(event);
                if (result.scheduledRoom) {
                    ++scheduledCount;
                }
                if (result.status == Game::RoomEventDispatcherEnqueueStatus::kEnqueued) {
                    ++enqueuedCount;
                } else if (
                    result.status ==
                    Game::RoomEventDispatcherEnqueueStatus::kRejectedBackpressure) {
                    ++backpressureCount;
                }
            }
        });
    }

    for (std::thread& producer : producers) {
        producer.join();
    }

    EXPECT_EQ(scheduledCount.load(), 1U);
    EXPECT_EQ(enqueuedCount.load(), kCapacity);
    EXPECT_EQ(
        backpressureCount.load(),
        kProducerCount * kAttemptsPerProducer - kCapacity);

    uint32_t activeRoomId = 0;
    ASSERT_TRUE(dispatcher.tryPopActiveRoom(activeRoomId));
    EXPECT_EQ(activeRoomId, 1U);
    EXPECT_FALSE(dispatcher.tryPopActiveRoom(activeRoomId));

    Game::RoomEventQueueStats stats;
    ASSERT_TRUE(dispatcher.roomQueueStats(1, stats));
    EXPECT_EQ(stats.roomEventEnqueuedCount, kCapacity);
    EXPECT_EQ(
        stats.roomEventRejectedBackpressureCount,
        kProducerCount * kAttemptsPerProducer - kCapacity);
    EXPECT_EQ(stats.roomEventQueueDepth, kCapacity);
}

TEST(RoomEventDispatcherTests, RecordsMetricsForEnqueueRejectsContentionAndDepth) {
    Game::RoomEventMetrics metrics;
    Game::RoomEventDispatcher dispatcher(2, &metrics);
    ASSERT_TRUE(dispatcher.registerRoom(1));

    const Game::RoomEvent first = Game::makeReadyRoomEvent(10, 1);
    const Game::RoomEvent second = Game::makeMonsterDeathRoomEvent(10, 1, 5);
    const Game::RoomEvent third = Game::makeClickLootRoomEvent(20, 1, 9);
    const Game::RoomEvent fourth = Game::makeClickLootRoomEvent(30, 1, 10);

    EXPECT_TRUE(dispatcher.enqueue(first).scheduledRoom);
    EXPECT_FALSE(dispatcher.enqueue(second).scheduledRoom);
    EXPECT_EQ(
        dispatcher.enqueue(third).status,
        Game::RoomEventDispatcherEnqueueStatus::kRejectedBackpressure);
    ASSERT_TRUE(dispatcher.beginRoomShutdown(1));
    EXPECT_EQ(
        dispatcher.enqueue(fourth).status,
        Game::RoomEventDispatcherEnqueueStatus::kRejectedShutdown);

    Game::RoomEventMetricsSnapshot snapshot = metrics.snapshot();
    EXPECT_EQ(snapshot.roomEventEnqueuedCount, 2U);
    EXPECT_EQ(snapshot.roomEventRejectedBackpressureCount, 1U);
    EXPECT_EQ(snapshot.roomEventRejectedShutdownCount, 1U);
    EXPECT_EQ(snapshot.roomScheduledContentionCount, 1U);
    EXPECT_EQ(snapshot.roomEventQueueDepth, 2U);
    EXPECT_EQ(snapshot.roomEventMaxQueueDepth, 2U);

    Game::RoomEvent dequeued;
    ASSERT_TRUE(dispatcher.tryDequeueRoomEvent(1, dequeued));
    snapshot = metrics.snapshot();
    EXPECT_EQ(snapshot.roomEventQueueDepth, 1U);
    EXPECT_EQ(snapshot.roomEventMaxQueueDepth, 2U);
}

TEST(RoomEventDispatcherTests, RecordsAggregateQueueDepthAcrossRooms) {
    Game::RoomEventMetrics metrics;
    Game::RoomEventDispatcher dispatcher(2, &metrics);
    ASSERT_TRUE(dispatcher.registerRoom(1));
    ASSERT_TRUE(dispatcher.registerRoom(2));

    ASSERT_TRUE(dispatcher.enqueue(Game::makeReadyRoomEvent(10, 1)).scheduledRoom);
    ASSERT_TRUE(dispatcher.enqueue(Game::makeReadyRoomEvent(20, 2)).scheduledRoom);

    Game::RoomEventMetricsSnapshot snapshot = metrics.snapshot();
    EXPECT_EQ(snapshot.roomEventQueueDepth, 2U);
    EXPECT_EQ(snapshot.roomEventMaxQueueDepth, 2U);

    Game::RoomEvent dequeued;
    ASSERT_TRUE(dispatcher.tryDequeueRoomEvent(1, dequeued));
    snapshot = metrics.snapshot();
    EXPECT_EQ(snapshot.roomEventQueueDepth, 1U);
    EXPECT_EQ(snapshot.roomEventMaxQueueDepth, 2U);
}
