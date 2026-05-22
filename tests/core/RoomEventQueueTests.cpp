#include <gtest/gtest.h>

#include <cstddef>
#include <thread>
#include <vector>

#include "Game/RoomEventQueue.hpp"

namespace {
void expectRoomEventEquals(const Game::RoomEvent& actual, const Game::RoomEvent& expected) {
    EXPECT_EQ(actual.type, expected.type);
    EXPECT_EQ(actual.sessionId, expected.sessionId);
    EXPECT_EQ(actual.roomId, expected.roomId);
    EXPECT_EQ(actual.argument, expected.argument);
}
}  // namespace

TEST(RoomEventQueueTests, DequeuesEventsInFifoOrder) {
    Game::RoomEventQueue queue(3);
    const Game::RoomEvent ready = Game::makeReadyRoomEvent(10, 2);
    const Game::RoomEvent monsterDeath = Game::makeMonsterDeathRoomEvent(20, 2, 7);
    const Game::RoomEvent clickLoot = Game::makeClickLootRoomEvent(10, 2, 9);

    EXPECT_EQ(
        queue.enqueue(ready),
        Game::RoomEventQueueEnqueueResult::kEnqueued);
    EXPECT_EQ(
        queue.enqueue(monsterDeath),
        Game::RoomEventQueueEnqueueResult::kEnqueued);
    EXPECT_EQ(
        queue.enqueue(clickLoot),
        Game::RoomEventQueueEnqueueResult::kEnqueued);

    Game::RoomEvent dequeued;
    ASSERT_TRUE(queue.tryDequeue(dequeued));
    expectRoomEventEquals(dequeued, ready);
    ASSERT_TRUE(queue.tryDequeue(dequeued));
    expectRoomEventEquals(dequeued, monsterDeath);
    ASSERT_TRUE(queue.tryDequeue(dequeued));
    expectRoomEventEquals(dequeued, clickLoot);
    EXPECT_FALSE(queue.tryDequeue(dequeued));
}

TEST(RoomEventQueueTests, TracksDepthAndHighWaterMark) {
    Game::RoomEventQueue queue(3);

    EXPECT_EQ(queue.depth(), 0U);
    EXPECT_EQ(
        queue.enqueue(Game::makeReadyRoomEvent(10, 2)),
        Game::RoomEventQueueEnqueueResult::kEnqueued);
    EXPECT_EQ(
        queue.enqueue(Game::makeMonsterDeathRoomEvent(10, 2, 7)),
        Game::RoomEventQueueEnqueueResult::kEnqueued);
    EXPECT_EQ(queue.depth(), 2U);

    Game::RoomEvent dequeued;
    ASSERT_TRUE(queue.tryDequeue(dequeued));
    EXPECT_EQ(queue.depth(), 1U);

    const Game::RoomEventQueueStats stats = queue.stats();
    EXPECT_EQ(stats.roomEventEnqueuedCount, 2U);
    EXPECT_EQ(stats.roomEventQueueDepth, 1U);
    EXPECT_EQ(stats.roomEventMaxQueueDepth, 2U);
}

TEST(RoomEventQueueTests, RejectsCapacityOverflowWithoutBlockingProducer) {
    Game::RoomEventQueue queue(2);

    EXPECT_EQ(
        queue.enqueue(Game::makeReadyRoomEvent(10, 2)),
        Game::RoomEventQueueEnqueueResult::kEnqueued);
    EXPECT_EQ(
        queue.enqueue(Game::makeMonsterDeathRoomEvent(20, 2, 7)),
        Game::RoomEventQueueEnqueueResult::kEnqueued);
    EXPECT_EQ(
        queue.enqueue(Game::makeClickLootRoomEvent(10, 2, 9)),
        Game::RoomEventQueueEnqueueResult::kRejectedBackpressure);

    const Game::RoomEventQueueStats stats = queue.stats();
    EXPECT_EQ(stats.roomEventEnqueuedCount, 2U);
    EXPECT_EQ(stats.roomEventRejectedBackpressureCount, 1U);
    EXPECT_EQ(stats.roomEventRejectedShutdownCount, 0U);
    EXPECT_EQ(stats.roomEventQueueDepth, 2U);
    EXPECT_EQ(stats.roomEventMaxQueueDepth, 2U);
}

TEST(RoomEventQueueTests, RejectsNewEventsAfterShutdownAndAllowsDrain) {
    Game::RoomEventQueue queue(2);
    const Game::RoomEvent ready = Game::makeReadyRoomEvent(10, 2);
    const Game::RoomEvent monsterDeath = Game::makeMonsterDeathRoomEvent(20, 2, 7);

    ASSERT_EQ(
        queue.enqueue(ready),
        Game::RoomEventQueueEnqueueResult::kEnqueued);
    ASSERT_EQ(
        queue.enqueue(monsterDeath),
        Game::RoomEventQueueEnqueueResult::kEnqueued);

    queue.beginShutdown();

    EXPECT_TRUE(queue.shuttingDown());
    EXPECT_EQ(
        queue.enqueue(Game::makeClickLootRoomEvent(10, 2, 9)),
        Game::RoomEventQueueEnqueueResult::kRejectedShutdown);

    Game::RoomEvent dequeued;
    ASSERT_TRUE(queue.tryDequeue(dequeued));
    expectRoomEventEquals(dequeued, ready);
    ASSERT_TRUE(queue.tryDequeue(dequeued));
    expectRoomEventEquals(dequeued, monsterDeath);
    EXPECT_FALSE(queue.tryDequeue(dequeued));

    const Game::RoomEventQueueStats stats = queue.stats();
    EXPECT_EQ(stats.roomEventRejectedShutdownCount, 1U);
    EXPECT_EQ(stats.roomEventQueueDepth, 0U);
}

TEST(RoomEventQueueTests, ShutdownRejectTakesPrecedenceOverBackpressureReject) {
    Game::RoomEventQueue queue(1);

    ASSERT_EQ(
        queue.enqueue(Game::makeReadyRoomEvent(10, 2)),
        Game::RoomEventQueueEnqueueResult::kEnqueued);
    queue.beginShutdown();

    EXPECT_EQ(
        queue.enqueue(Game::makeMonsterDeathRoomEvent(20, 2, 7)),
        Game::RoomEventQueueEnqueueResult::kRejectedShutdown);

    const Game::RoomEventQueueStats stats = queue.stats();
    EXPECT_EQ(stats.roomEventRejectedBackpressureCount, 0U);
    EXPECT_EQ(stats.roomEventRejectedShutdownCount, 1U);
    EXPECT_EQ(stats.roomEventQueueDepth, 1U);
}

TEST(RoomEventQueueTests, ZeroCapacityRejectsWithBackpressure) {
    Game::RoomEventQueue queue(0);

    EXPECT_EQ(queue.capacity(), 0U);
    EXPECT_EQ(
        queue.enqueue(Game::makeReadyRoomEvent(10, 2)),
        Game::RoomEventQueueEnqueueResult::kRejectedBackpressure);

    const Game::RoomEventQueueStats stats = queue.stats();
    EXPECT_EQ(stats.roomEventEnqueuedCount, 0U);
    EXPECT_EQ(stats.roomEventRejectedBackpressureCount, 1U);
    EXPECT_EQ(stats.roomEventQueueDepth, 0U);
    EXPECT_EQ(stats.roomEventMaxQueueDepth, 0U);
}

TEST(RoomEventQueueTests, ConcurrentProducersRespectCapacityAndCounters) {
    constexpr size_t kCapacity = 32;
    constexpr size_t kProducerCount = 4;
    constexpr size_t kAttemptsPerProducer = 50;
    Game::RoomEventQueue queue(kCapacity);

    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    for (size_t producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&queue, producer]() {
            for (size_t attempt = 0; attempt < kAttemptsPerProducer; ++attempt) {
                queue.enqueue(Game::makeClickLootRoomEvent(
                    static_cast<uint64_t>(producer + 1),
                    2,
                    static_cast<uint32_t>(attempt + 1)));
            }
        });
    }

    for (std::thread& producer : producers) {
        producer.join();
    }

    const Game::RoomEventQueueStats stats = queue.stats();
    EXPECT_EQ(stats.roomEventEnqueuedCount, kCapacity);
    EXPECT_EQ(
        stats.roomEventRejectedBackpressureCount,
        kProducerCount * kAttemptsPerProducer - kCapacity);
    EXPECT_EQ(stats.roomEventRejectedShutdownCount, 0U);
    EXPECT_EQ(stats.roomEventQueueDepth, kCapacity);
    EXPECT_EQ(stats.roomEventMaxQueueDepth, kCapacity);
}
