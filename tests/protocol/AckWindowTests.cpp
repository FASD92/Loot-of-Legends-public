#include <gtest/gtest.h>

#include <cstdint>

#include "Net/RudpAckWindow.hpp"

TEST(AckWindowTests, FirstSequenceInitializesAckWithoutAckBits) {
    Net::RudpAckWindow window;

    EXPECT_FALSE(window.hasAck());
    EXPECT_EQ(window.recordReceived(10), Net::RudpAckWindowRecordResult::kAcceptedNew);

    EXPECT_TRUE(window.hasAck());
    EXPECT_EQ(window.ack(), 10U);
    EXPECT_EQ(window.ackBits(), 0U);
}

TEST(AckWindowTests, SequentialReceiveSetsPreviousAckBits) {
    Net::RudpAckWindow window;

    EXPECT_EQ(window.recordReceived(10), Net::RudpAckWindowRecordResult::kAcceptedNew);
    EXPECT_EQ(window.recordReceived(11), Net::RudpAckWindowRecordResult::kAcceptedNew);
    EXPECT_EQ(window.recordReceived(12), Net::RudpAckWindowRecordResult::kAcceptedNew);

    EXPECT_EQ(window.ack(), 12U);
    EXPECT_EQ(window.ackBits(), 0x00000003U);
}

TEST(AckWindowTests, OutOfOrderReceiveSetsMissingAckBit) {
    Net::RudpAckWindow window;

    EXPECT_EQ(window.recordReceived(10), Net::RudpAckWindowRecordResult::kAcceptedNew);
    EXPECT_EQ(window.recordReceived(12), Net::RudpAckWindowRecordResult::kAcceptedNew);

    EXPECT_EQ(window.ack(), 12U);
    EXPECT_EQ(window.ackBits(), 0x00000002U);

    EXPECT_EQ(window.recordReceived(11), Net::RudpAckWindowRecordResult::kAcceptedOutOfOrder);

    EXPECT_EQ(window.ack(), 12U);
    EXPECT_EQ(window.ackBits(), 0x00000003U);
}

TEST(AckWindowTests, DuplicateSequenceDoesNotChangeState) {
    Net::RudpAckWindow window;

    EXPECT_EQ(window.recordReceived(10), Net::RudpAckWindowRecordResult::kAcceptedNew);
    EXPECT_EQ(window.recordReceived(11), Net::RudpAckWindowRecordResult::kAcceptedNew);
    const uint32_t ack = window.ack();
    const uint32_t ackBits = window.ackBits();

    EXPECT_EQ(window.recordReceived(11), Net::RudpAckWindowRecordResult::kDuplicate);
    EXPECT_EQ(window.recordReceived(10), Net::RudpAckWindowRecordResult::kDuplicate);

    EXPECT_EQ(window.ack(), ack);
    EXPECT_EQ(window.ackBits(), ackBits);
}

TEST(AckWindowTests, TooOldSequenceIsRejected) {
    Net::RudpAckWindow window;

    EXPECT_EQ(window.recordReceived(100), Net::RudpAckWindowRecordResult::kAcceptedNew);
    EXPECT_EQ(window.recordReceived(68), Net::RudpAckWindowRecordResult::kAcceptedOutOfOrder);
    EXPECT_EQ(window.recordReceived(67), Net::RudpAckWindowRecordResult::kTooOld);

    EXPECT_EQ(window.ack(), 100U);
    EXPECT_EQ(window.ackBits(), 1U << 31U);
}

TEST(AckWindowTests, HandlesUint32WrapAround) {
    Net::RudpAckWindow window;

    EXPECT_EQ(
        window.recordReceived(0xFFFFFFFEU),
        Net::RudpAckWindowRecordResult::kAcceptedNew);
    EXPECT_EQ(
        window.recordReceived(0xFFFFFFFFU),
        Net::RudpAckWindowRecordResult::kAcceptedNew);
    EXPECT_EQ(window.recordReceived(0U), Net::RudpAckWindowRecordResult::kAcceptedNew);

    EXPECT_EQ(window.ack(), 0U);
    EXPECT_EQ(window.ackBits(), 0x00000003U);
}

TEST(AckWindowTests, TreatsHalfRangeDistanceAsAmbiguousNotNewer) {
    Net::RudpAckWindow window;

    EXPECT_EQ(window.recordReceived(100), Net::RudpAckWindowRecordResult::kAcceptedNew);
    EXPECT_EQ(
        window.recordReceived(100U + 0x80000000U),
        Net::RudpAckWindowRecordResult::kTooOld);

    EXPECT_EQ(window.ack(), 100U);
    EXPECT_EQ(window.ackBits(), 0U);
}
