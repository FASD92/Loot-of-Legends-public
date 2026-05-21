#include <gtest/gtest.h>

#include <string>

#include "Net/RudpMetaResponseIdempotencyTracker.hpp"

namespace {
constexpr const char* kSettlementId = "room-42-session-1001-finish-1";
}

TEST(RudpMetaResponseIdempotencyTrackerTests, AppliedCompletesSettlementOnce) {
    Net::RudpMetaResponseIdempotencyTracker tracker;

    EXPECT_EQ(
        tracker.record(kSettlementId, Net::RudpMetaResponseStatus::kApplied),
        Net::RudpMetaResponseIdempotencyResult::kCompletedFirst);

    EXPECT_TRUE(tracker.isCompleted(kSettlementId));
    EXPECT_FALSE(tracker.isRetryObserved(kSettlementId));
    EXPECT_EQ(tracker.completionCount(), 1U);
    EXPECT_EQ(tracker.retryCount(), 0U);
}

TEST(RudpMetaResponseIdempotencyTrackerTests, DuplicateStatusCompletesSettlementOnce) {
    Net::RudpMetaResponseIdempotencyTracker tracker;

    EXPECT_EQ(
        tracker.record(kSettlementId, Net::RudpMetaResponseStatus::kDuplicate),
        Net::RudpMetaResponseIdempotencyResult::kCompletedFirst);

    EXPECT_TRUE(tracker.isCompleted(kSettlementId));
    EXPECT_EQ(tracker.completionCount(), 1U);
}

TEST(RudpMetaResponseIdempotencyTrackerTests, RejectedStatusCompletesSettlementOnce) {
    Net::RudpMetaResponseIdempotencyTracker tracker;

    EXPECT_EQ(
        tracker.record(kSettlementId, Net::RudpMetaResponseStatus::kRejected),
        Net::RudpMetaResponseIdempotencyResult::kCompletedFirst);

    EXPECT_TRUE(tracker.isCompleted(kSettlementId));
    EXPECT_EQ(tracker.completionCount(), 1U);
}

TEST(RudpMetaResponseIdempotencyTrackerTests, TerminalDuplicateDoesNotGrowState) {
    Net::RudpMetaResponseIdempotencyTracker tracker;
    ASSERT_EQ(
        tracker.record(kSettlementId, Net::RudpMetaResponseStatus::kApplied),
        Net::RudpMetaResponseIdempotencyResult::kCompletedFirst);

    EXPECT_EQ(
        tracker.record(kSettlementId, Net::RudpMetaResponseStatus::kRejected),
        Net::RudpMetaResponseIdempotencyResult::kCompletionDuplicate);
    EXPECT_EQ(
        tracker.record(kSettlementId, Net::RudpMetaResponseStatus::kDuplicate),
        Net::RudpMetaResponseIdempotencyResult::kCompletionDuplicate);

    EXPECT_TRUE(tracker.isCompleted(kSettlementId));
    EXPECT_EQ(tracker.completionCount(), 1U);
    EXPECT_EQ(tracker.retryCount(), 0U);
}

TEST(RudpMetaResponseIdempotencyTrackerTests, RetryLaterDoesNotCompleteSettlement) {
    Net::RudpMetaResponseIdempotencyTracker tracker;

    EXPECT_EQ(
        tracker.record(kSettlementId, Net::RudpMetaResponseStatus::kRetryLater),
        Net::RudpMetaResponseIdempotencyResult::kRetryObserved);

    EXPECT_FALSE(tracker.isCompleted(kSettlementId));
    EXPECT_TRUE(tracker.isRetryObserved(kSettlementId));
    EXPECT_EQ(tracker.completionCount(), 0U);
    EXPECT_EQ(tracker.retryCount(), 1U);
}

TEST(RudpMetaResponseIdempotencyTrackerTests, DuplicateRetryLaterKeepsRetryObservationOnly) {
    Net::RudpMetaResponseIdempotencyTracker tracker;
    ASSERT_EQ(
        tracker.record(kSettlementId, Net::RudpMetaResponseStatus::kRetryLater),
        Net::RudpMetaResponseIdempotencyResult::kRetryObserved);

    EXPECT_EQ(
        tracker.record(kSettlementId, Net::RudpMetaResponseStatus::kRetryLater),
        Net::RudpMetaResponseIdempotencyResult::kRetryDuplicate);

    EXPECT_FALSE(tracker.isCompleted(kSettlementId));
    EXPECT_TRUE(tracker.isRetryObserved(kSettlementId));
    EXPECT_EQ(tracker.completionCount(), 0U);
    EXPECT_EQ(tracker.retryCount(), 1U);
}

TEST(RudpMetaResponseIdempotencyTrackerTests, TerminalAfterRetryCompletesAndClearsRetry) {
    Net::RudpMetaResponseIdempotencyTracker tracker;
    ASSERT_EQ(
        tracker.record(kSettlementId, Net::RudpMetaResponseStatus::kRetryLater),
        Net::RudpMetaResponseIdempotencyResult::kRetryObserved);

    EXPECT_EQ(
        tracker.record(kSettlementId, Net::RudpMetaResponseStatus::kApplied),
        Net::RudpMetaResponseIdempotencyResult::kCompletedFirst);

    EXPECT_TRUE(tracker.isCompleted(kSettlementId));
    EXPECT_FALSE(tracker.isRetryObserved(kSettlementId));
    EXPECT_EQ(tracker.completionCount(), 1U);
    EXPECT_EQ(tracker.retryCount(), 0U);
}

TEST(RudpMetaResponseIdempotencyTrackerTests, RetryAfterCompletionDoesNotReopenState) {
    Net::RudpMetaResponseIdempotencyTracker tracker;
    ASSERT_EQ(
        tracker.record(kSettlementId, Net::RudpMetaResponseStatus::kApplied),
        Net::RudpMetaResponseIdempotencyResult::kCompletedFirst);

    EXPECT_EQ(
        tracker.record(kSettlementId, Net::RudpMetaResponseStatus::kRetryLater),
        Net::RudpMetaResponseIdempotencyResult::kRetryIgnoredAfterCompletion);

    EXPECT_TRUE(tracker.isCompleted(kSettlementId));
    EXPECT_FALSE(tracker.isRetryObserved(kSettlementId));
    EXPECT_EQ(tracker.completionCount(), 1U);
    EXPECT_EQ(tracker.retryCount(), 0U);
}

TEST(RudpMetaResponseIdempotencyTrackerTests, TracksSettlementsIndependently) {
    Net::RudpMetaResponseIdempotencyTracker tracker;

    EXPECT_EQ(
        tracker.record("room-1-session-1001-finish-1", Net::RudpMetaResponseStatus::kApplied),
        Net::RudpMetaResponseIdempotencyResult::kCompletedFirst);
    EXPECT_EQ(
        tracker.record("room-1-session-1002-finish-1", Net::RudpMetaResponseStatus::kRetryLater),
        Net::RudpMetaResponseIdempotencyResult::kRetryObserved);

    EXPECT_TRUE(tracker.isCompleted("room-1-session-1001-finish-1"));
    EXPECT_FALSE(tracker.isCompleted("room-1-session-1002-finish-1"));
    EXPECT_TRUE(tracker.isRetryObserved("room-1-session-1002-finish-1"));
    EXPECT_EQ(tracker.completionCount(), 1U);
    EXPECT_EQ(tracker.retryCount(), 1U);
}

TEST(RudpMetaResponseIdempotencyTrackerTests, RejectsInvalidSettlementIdsWithoutState) {
    Net::RudpMetaResponseIdempotencyTracker tracker;
    const std::string tooLong(Net::kRudpMetaResponseSettlementIdMaxLength + 1, 'a');

    EXPECT_EQ(
        tracker.record("", Net::RudpMetaResponseStatus::kApplied),
        Net::RudpMetaResponseIdempotencyResult::kInvalidSettlementId);
    EXPECT_EQ(
        tracker.record(tooLong, Net::RudpMetaResponseStatus::kApplied),
        Net::RudpMetaResponseIdempotencyResult::kInvalidSettlementId);
    EXPECT_EQ(
        tracker.record("room-42\nsession-1001", Net::RudpMetaResponseStatus::kApplied),
        Net::RudpMetaResponseIdempotencyResult::kInvalidSettlementId);
    EXPECT_EQ(
        tracker.record("room-42-session-\x7F", Net::RudpMetaResponseStatus::kRetryLater),
        Net::RudpMetaResponseIdempotencyResult::kInvalidSettlementId);

    EXPECT_EQ(tracker.completionCount(), 0U);
    EXPECT_EQ(tracker.retryCount(), 0U);
}

TEST(RudpMetaResponseIdempotencyTrackerTests, ClearResetsCompletionAndRetryState) {
    Net::RudpMetaResponseIdempotencyTracker tracker;
    ASSERT_EQ(
        tracker.record("room-1-session-1001-finish-1", Net::RudpMetaResponseStatus::kApplied),
        Net::RudpMetaResponseIdempotencyResult::kCompletedFirst);
    ASSERT_EQ(
        tracker.record("room-1-session-1002-finish-1", Net::RudpMetaResponseStatus::kRetryLater),
        Net::RudpMetaResponseIdempotencyResult::kRetryObserved);

    tracker.clear();

    EXPECT_EQ(tracker.completionCount(), 0U);
    EXPECT_EQ(tracker.retryCount(), 0U);
    EXPECT_FALSE(tracker.isCompleted("room-1-session-1001-finish-1"));
    EXPECT_FALSE(tracker.isRetryObserved("room-1-session-1002-finish-1"));
}
