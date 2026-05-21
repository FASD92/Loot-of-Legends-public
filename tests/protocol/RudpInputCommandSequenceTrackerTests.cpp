#include <gtest/gtest.h>

#include <cstdint>

#include "Net/RudpInputCommandSequenceTracker.hpp"

TEST(RudpInputCommandSequenceTrackerTests, FirstCommandInitializesSessionState) {
    Net::RudpInputCommandSequenceTracker tracker;

    EXPECT_EQ(
        tracker.record(1001, 10),
        Net::RudpInputCommandSequenceResult::kAcceptedFirst);

    ASSERT_TRUE(tracker.lastAcceptedCmdSeq(1001).has_value());
    EXPECT_EQ(*tracker.lastAcceptedCmdSeq(1001), 10U);
    EXPECT_EQ(tracker.size(), 1U);
}

TEST(RudpInputCommandSequenceTrackerTests, NewerCommandUpdatesLastAccepted) {
    Net::RudpInputCommandSequenceTracker tracker;
    ASSERT_EQ(
        tracker.record(1001, 10),
        Net::RudpInputCommandSequenceResult::kAcceptedFirst);

    EXPECT_EQ(
        tracker.record(1001, 11),
        Net::RudpInputCommandSequenceResult::kAcceptedNewer);

    ASSERT_TRUE(tracker.lastAcceptedCmdSeq(1001).has_value());
    EXPECT_EQ(*tracker.lastAcceptedCmdSeq(1001), 11U);
}

TEST(RudpInputCommandSequenceTrackerTests, GapIsAllowedForNewerCommand) {
    Net::RudpInputCommandSequenceTracker tracker;
    ASSERT_EQ(
        tracker.record(1001, 10),
        Net::RudpInputCommandSequenceResult::kAcceptedFirst);

    EXPECT_EQ(
        tracker.record(1001, 12),
        Net::RudpInputCommandSequenceResult::kAcceptedNewer);

    ASSERT_TRUE(tracker.lastAcceptedCmdSeq(1001).has_value());
    EXPECT_EQ(*tracker.lastAcceptedCmdSeq(1001), 12U);
}

TEST(RudpInputCommandSequenceTrackerTests, DuplicateDoesNotChangeState) {
    Net::RudpInputCommandSequenceTracker tracker;
    ASSERT_EQ(
        tracker.record(1001, 10),
        Net::RudpInputCommandSequenceResult::kAcceptedFirst);

    EXPECT_EQ(
        tracker.record(1001, 10),
        Net::RudpInputCommandSequenceResult::kDuplicate);

    ASSERT_TRUE(tracker.lastAcceptedCmdSeq(1001).has_value());
    EXPECT_EQ(*tracker.lastAcceptedCmdSeq(1001), 10U);
}

TEST(RudpInputCommandSequenceTrackerTests, StaleCommandDoesNotChangeState) {
    Net::RudpInputCommandSequenceTracker tracker;
    ASSERT_EQ(
        tracker.record(1001, 10),
        Net::RudpInputCommandSequenceResult::kAcceptedFirst);
    ASSERT_EQ(
        tracker.record(1001, 12),
        Net::RudpInputCommandSequenceResult::kAcceptedNewer);

    EXPECT_EQ(
        tracker.record(1001, 11),
        Net::RudpInputCommandSequenceResult::kStale);

    ASSERT_TRUE(tracker.lastAcceptedCmdSeq(1001).has_value());
    EXPECT_EQ(*tracker.lastAcceptedCmdSeq(1001), 12U);
}

TEST(RudpInputCommandSequenceTrackerTests, HandlesUint32WrapAround) {
    Net::RudpInputCommandSequenceTracker tracker;

    EXPECT_EQ(
        tracker.record(1001, 0xFFFFFFFEU),
        Net::RudpInputCommandSequenceResult::kAcceptedFirst);
    EXPECT_EQ(
        tracker.record(1001, 0xFFFFFFFFU),
        Net::RudpInputCommandSequenceResult::kAcceptedNewer);
    EXPECT_EQ(
        tracker.record(1001, 0U),
        Net::RudpInputCommandSequenceResult::kAcceptedNewer);

    ASSERT_TRUE(tracker.lastAcceptedCmdSeq(1001).has_value());
    EXPECT_EQ(*tracker.lastAcceptedCmdSeq(1001), 0U);
}

TEST(RudpInputCommandSequenceTrackerTests, RejectsHalfRangeAmbiguousCommand) {
    Net::RudpInputCommandSequenceTracker tracker;
    ASSERT_EQ(
        tracker.record(1001, 100),
        Net::RudpInputCommandSequenceResult::kAcceptedFirst);

    EXPECT_EQ(
        tracker.record(1001, 100U + 0x80000000U),
        Net::RudpInputCommandSequenceResult::kAmbiguous);

    ASSERT_TRUE(tracker.lastAcceptedCmdSeq(1001).has_value());
    EXPECT_EQ(*tracker.lastAcceptedCmdSeq(1001), 100U);
}

TEST(RudpInputCommandSequenceTrackerTests, TracksSessionsIndependently) {
    Net::RudpInputCommandSequenceTracker tracker;

    EXPECT_EQ(
        tracker.record(1001, 10),
        Net::RudpInputCommandSequenceResult::kAcceptedFirst);
    EXPECT_EQ(
        tracker.record(1002, 5),
        Net::RudpInputCommandSequenceResult::kAcceptedFirst);
    EXPECT_EQ(
        tracker.record(1001, 11),
        Net::RudpInputCommandSequenceResult::kAcceptedNewer);

    ASSERT_TRUE(tracker.lastAcceptedCmdSeq(1001).has_value());
    ASSERT_TRUE(tracker.lastAcceptedCmdSeq(1002).has_value());
    EXPECT_EQ(*tracker.lastAcceptedCmdSeq(1001), 11U);
    EXPECT_EQ(*tracker.lastAcceptedCmdSeq(1002), 5U);
    EXPECT_EQ(tracker.size(), 2U);
}

TEST(RudpInputCommandSequenceTrackerTests, RemoveSessionResetsState) {
    Net::RudpInputCommandSequenceTracker tracker;
    ASSERT_EQ(
        tracker.record(1001, 10),
        Net::RudpInputCommandSequenceResult::kAcceptedFirst);

    EXPECT_TRUE(tracker.removeSession(1001));
    EXPECT_FALSE(tracker.removeSession(1001));
    EXPECT_FALSE(tracker.lastAcceptedCmdSeq(1001).has_value());
    EXPECT_EQ(tracker.size(), 0U);

    EXPECT_EQ(
        tracker.record(1001, 9),
        Net::RudpInputCommandSequenceResult::kAcceptedFirst);
    ASSERT_TRUE(tracker.lastAcceptedCmdSeq(1001).has_value());
    EXPECT_EQ(*tracker.lastAcceptedCmdSeq(1001), 9U);
}

TEST(RudpInputCommandSequenceTrackerTests, RejectsInvalidSessionWithoutState) {
    Net::RudpInputCommandSequenceTracker tracker;

    EXPECT_EQ(
        tracker.record(0, 10),
        Net::RudpInputCommandSequenceResult::kInvalidSession);

    EXPECT_FALSE(tracker.lastAcceptedCmdSeq(0).has_value());
    EXPECT_EQ(tracker.size(), 0U);
}
