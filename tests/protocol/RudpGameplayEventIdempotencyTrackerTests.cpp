#include <gtest/gtest.h>

#include "Net/RudpGameplayEventIdempotencyTracker.hpp"

TEST(RudpGameplayEventIdempotencyTrackerTests, AcceptsFirstBattleStartKey) {
    Net::RudpGameplayEventIdempotencyTracker tracker;
    const Net::RudpGameplayEventKey key =
        Net::RudpGameplayEventKey::battleStart(42, 1001, 1002);

    EXPECT_EQ(
        tracker.record(key),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);

    EXPECT_TRUE(tracker.contains(key));
    EXPECT_EQ(tracker.size(), 1U);
}

TEST(RudpGameplayEventIdempotencyTrackerTests, SuppressesDuplicateBattleStartKey) {
    Net::RudpGameplayEventIdempotencyTracker tracker;
    const Net::RudpGameplayEventKey key =
        Net::RudpGameplayEventKey::battleStart(42, 1001, 1002);

    ASSERT_EQ(
        tracker.record(key),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);

    EXPECT_EQ(
        tracker.record(key),
        Net::RudpGameplayEventIdempotencyResult::kDuplicate);
    EXPECT_EQ(tracker.size(), 1U);
}

TEST(RudpGameplayEventIdempotencyTrackerTests, AcceptsFirstMonsterDeathKey) {
    Net::RudpGameplayEventIdempotencyTracker tracker;
    const Net::RudpGameplayEventKey key =
        Net::RudpGameplayEventKey::monsterDeath(42, 7);

    EXPECT_EQ(
        tracker.record(key),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);

    EXPECT_TRUE(tracker.contains(key));
    EXPECT_EQ(tracker.size(), 1U);
}

TEST(RudpGameplayEventIdempotencyTrackerTests, SuppressesDuplicateMonsterDeathKey) {
    Net::RudpGameplayEventIdempotencyTracker tracker;
    const Net::RudpGameplayEventKey key =
        Net::RudpGameplayEventKey::monsterDeath(42, 7);

    ASSERT_EQ(
        tracker.record(key),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);

    EXPECT_EQ(
        tracker.record(key),
        Net::RudpGameplayEventIdempotencyResult::kDuplicate);
    EXPECT_EQ(tracker.size(), 1U);
}

TEST(RudpGameplayEventIdempotencyTrackerTests, AcceptsFirstLootResolvedKey) {
    Net::RudpGameplayEventIdempotencyTracker tracker;
    const Net::RudpGameplayEventKey key =
        Net::RudpGameplayEventKey::lootResolved(42, 77);

    EXPECT_EQ(
        tracker.record(key),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);

    EXPECT_TRUE(tracker.contains(key));
    EXPECT_EQ(tracker.size(), 1U);
}

TEST(RudpGameplayEventIdempotencyTrackerTests, SuppressesDuplicateLootResolvedKey) {
    Net::RudpGameplayEventIdempotencyTracker tracker;
    const Net::RudpGameplayEventKey key =
        Net::RudpGameplayEventKey::lootResolved(42, 77);

    ASSERT_EQ(
        tracker.record(key),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);

    EXPECT_EQ(
        tracker.record(key),
        Net::RudpGameplayEventIdempotencyResult::kDuplicate);
    EXPECT_EQ(tracker.size(), 1U);
}

TEST(RudpGameplayEventIdempotencyTrackerTests, EventKindsDoNotCollide) {
    Net::RudpGameplayEventIdempotencyTracker tracker;

    EXPECT_EQ(
        tracker.record(Net::RudpGameplayEventKey::monsterDeath(42, 7)),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);
    EXPECT_EQ(
        tracker.record(Net::RudpGameplayEventKey::lootResolved(42, 7)),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);

    EXPECT_EQ(tracker.size(), 2U);
}

TEST(RudpGameplayEventIdempotencyTrackerTests, DifferentEventFieldsAreAccepted) {
    Net::RudpGameplayEventIdempotencyTracker tracker;

    EXPECT_EQ(
        tracker.record(Net::RudpGameplayEventKey::battleStart(42, 1001, 1002)),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);
    EXPECT_EQ(
        tracker.record(Net::RudpGameplayEventKey::battleStart(43, 1001, 1002)),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);
    EXPECT_EQ(
        tracker.record(Net::RudpGameplayEventKey::monsterDeath(42, 7)),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);
    EXPECT_EQ(
        tracker.record(Net::RudpGameplayEventKey::monsterDeath(42, 8)),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);
    EXPECT_EQ(
        tracker.record(Net::RudpGameplayEventKey::lootResolved(42, 77)),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);
    EXPECT_EQ(
        tracker.record(Net::RudpGameplayEventKey::lootResolved(42, 78)),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);

    EXPECT_EQ(tracker.size(), 6U);
}

TEST(RudpGameplayEventIdempotencyTrackerTests, RejectsInvalidKeysWithoutState) {
    Net::RudpGameplayEventIdempotencyTracker tracker;

    EXPECT_EQ(
        tracker.record(Net::RudpGameplayEventKey::battleStart(0, 1001, 1002)),
        Net::RudpGameplayEventIdempotencyResult::kInvalidKey);
    EXPECT_EQ(
        tracker.record(Net::RudpGameplayEventKey::battleStart(42, 0, 1002)),
        Net::RudpGameplayEventIdempotencyResult::kInvalidKey);
    EXPECT_EQ(
        tracker.record(Net::RudpGameplayEventKey::battleStart(42, 1001, 0)),
        Net::RudpGameplayEventIdempotencyResult::kInvalidKey);
    EXPECT_EQ(
        tracker.record(Net::RudpGameplayEventKey::battleStart(42, 1001, 1001)),
        Net::RudpGameplayEventIdempotencyResult::kInvalidKey);
    EXPECT_EQ(
        tracker.record(Net::RudpGameplayEventKey::monsterDeath(0, 7)),
        Net::RudpGameplayEventIdempotencyResult::kInvalidKey);
    EXPECT_EQ(
        tracker.record(Net::RudpGameplayEventKey::monsterDeath(42, 0)),
        Net::RudpGameplayEventIdempotencyResult::kInvalidKey);
    EXPECT_EQ(
        tracker.record(Net::RudpGameplayEventKey::lootResolved(0, 77)),
        Net::RudpGameplayEventIdempotencyResult::kInvalidKey);
    EXPECT_EQ(
        tracker.record(Net::RudpGameplayEventKey::lootResolved(42, 0)),
        Net::RudpGameplayEventIdempotencyResult::kInvalidKey);

    EXPECT_EQ(tracker.size(), 0U);
}

TEST(
    RudpGameplayEventIdempotencyTrackerTests,
    CurrentFieldKeysSuppressRepeatedLifecycleUntilExplicitEventIdentityExists) {
    Net::RudpGameplayEventIdempotencyTracker tracker;
    const Net::RudpGameplayEventKey firstLifecycle =
        Net::RudpGameplayEventKey::battleStart(42, 1001, 1002);
    const Net::RudpGameplayEventKey repeatedLifecycleSameFields =
        Net::RudpGameplayEventKey::battleStart(42, 1001, 1002);

    ASSERT_EQ(
        tracker.record(firstLifecycle),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);

    EXPECT_EQ(
        tracker.record(repeatedLifecycleSameFields),
        Net::RudpGameplayEventIdempotencyResult::kDuplicate);
    EXPECT_EQ(tracker.size(), 1U);
}

TEST(RudpGameplayEventIdempotencyTrackerTests, ClearResetsRecordedKeys) {
    Net::RudpGameplayEventIdempotencyTracker tracker;
    const Net::RudpGameplayEventKey key =
        Net::RudpGameplayEventKey::lootResolved(42, 77);

    ASSERT_EQ(
        tracker.record(key),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);

    tracker.clear();

    EXPECT_FALSE(tracker.contains(key));
    EXPECT_EQ(tracker.size(), 0U);
    EXPECT_EQ(
        tracker.record(key),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);
}
