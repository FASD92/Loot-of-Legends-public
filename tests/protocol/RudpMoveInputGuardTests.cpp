#include <gtest/gtest.h>

#include <chrono>

#include "Net/RudpMoveInputGuard.hpp"
#include "Util/Time.hpp"

namespace {
Util::TimePoint timeAt(int64_t milliseconds) {
    return Util::TimePoint{std::chrono::milliseconds(milliseconds)};
}

Net::RudpInputCommandMoveArgs move(
    int16_t dirX = 100,
    int16_t dirY = -100,
    uint16_t inputFlags = 0) {
    return Net::RudpInputCommandMoveArgs{dirX, dirY, inputFlags};
}
}  // namespace

TEST(RudpMoveInputGuardTests, AcceptsValidNonZeroMove) {
    Net::RudpMoveInputGuard guard;

    EXPECT_EQ(
        guard.record(1001, move(), timeAt(1000)),
        Net::RudpMoveInputGuardResult::kAccepted);
    EXPECT_EQ(guard.size(), 1U);
}

TEST(RudpMoveInputGuardTests, AcceptsZeroVectorAsStopIntent) {
    Net::RudpMoveInputGuard guard;

    EXPECT_EQ(
        guard.record(1001, move(0, 0), timeAt(1000)),
        Net::RudpMoveInputGuardResult::kAccepted);
}

TEST(RudpMoveInputGuardTests, RejectsReservedInputFlags) {
    Net::RudpMoveInputGuard guard;

    EXPECT_EQ(
        guard.record(1001, move(10, 20, 1), timeAt(1000)),
        Net::RudpMoveInputGuardResult::kInvalidReservedFlags);
    EXPECT_EQ(guard.size(), 0U);
}

TEST(RudpMoveInputGuardTests, RejectsInvalidSession) {
    Net::RudpMoveInputGuard guard;

    EXPECT_EQ(
        guard.record(0, move(), timeAt(1000)),
        Net::RudpMoveInputGuardResult::kInvalidSession);
    EXPECT_EQ(guard.size(), 0U);
}

TEST(RudpMoveInputGuardTests, AppliesThirtyHzBurstTenRateLimit) {
    Net::RudpMoveInputGuard guard;

    for (uint32_t i = 0; i < Net::RudpMoveInputGuard::kDefaultBurstCapacity; ++i) {
        EXPECT_EQ(
            guard.record(1001, move(), timeAt(1000)),
            Net::RudpMoveInputGuardResult::kAccepted);
    }

    EXPECT_EQ(
        guard.record(1001, move(), timeAt(1000)),
        Net::RudpMoveInputGuardResult::kRateLimited);
}

TEST(RudpMoveInputGuardTests, RefillsByElapsedTime) {
    Net::RudpMoveInputGuard guard;

    for (uint32_t i = 0; i < Net::RudpMoveInputGuard::kDefaultBurstCapacity; ++i) {
        ASSERT_EQ(
            guard.record(1001, move(), timeAt(1000)),
            Net::RudpMoveInputGuardResult::kAccepted);
    }
    ASSERT_EQ(
        guard.record(1001, move(), timeAt(1000)),
        Net::RudpMoveInputGuardResult::kRateLimited);

    EXPECT_EQ(
        guard.record(1001, move(), timeAt(1034)),
        Net::RudpMoveInputGuardResult::kAccepted);
}

TEST(RudpMoveInputGuardTests, TracksSessionsIndependently) {
    Net::RudpMoveInputGuard guard;

    for (uint32_t i = 0; i < Net::RudpMoveInputGuard::kDefaultBurstCapacity; ++i) {
        ASSERT_EQ(
            guard.record(1001, move(), timeAt(1000)),
            Net::RudpMoveInputGuardResult::kAccepted);
    }

    EXPECT_EQ(
        guard.record(1001, move(), timeAt(1000)),
        Net::RudpMoveInputGuardResult::kRateLimited);
    EXPECT_EQ(
        guard.record(1002, move(), timeAt(1000)),
        Net::RudpMoveInputGuardResult::kAccepted);
}

TEST(RudpMoveInputGuardTests, RemoveSessionClearsLimiterState) {
    Net::RudpMoveInputGuard guard;

    for (uint32_t i = 0; i < Net::RudpMoveInputGuard::kDefaultBurstCapacity; ++i) {
        ASSERT_EQ(
            guard.record(1001, move(), timeAt(1000)),
            Net::RudpMoveInputGuardResult::kAccepted);
    }
    ASSERT_EQ(
        guard.record(1001, move(), timeAt(1000)),
        Net::RudpMoveInputGuardResult::kRateLimited);

    EXPECT_TRUE(guard.removeSession(1001));
    EXPECT_EQ(guard.size(), 0U);
    EXPECT_EQ(
        guard.record(1001, move(), timeAt(1000)),
        Net::RudpMoveInputGuardResult::kAccepted);
}
