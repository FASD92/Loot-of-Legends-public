#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include "Net/RudpReliableSendQueue.hpp"

namespace {
std::vector<uint8_t> bytes(uint8_t marker) {
    return {0x4C, 0x4F, marker};
}

Net::RudpReliableSendQueue::TimePoint timeAt(int64_t milliseconds) {
    return Net::RudpReliableSendQueue::TimePoint{
        std::chrono::milliseconds(milliseconds)};
}
}  // namespace

TEST(ReliableSendQueueTests, TrackStoresPendingPacket) {
    Net::RudpReliableSendQueue queue;

    ASSERT_TRUE(queue.track(10, bytes(0x10)));

    EXPECT_TRUE(queue.contains(10));
    EXPECT_EQ(queue.pendingCount(), 1U);
    EXPECT_EQ(queue.pendingSequences(), (std::vector<uint32_t>{10}));
    ASSERT_NE(queue.packetBytes(10), nullptr);
    EXPECT_EQ(*queue.packetBytes(10), bytes(0x10));
}

TEST(ReliableSendQueueTests, RejectDuplicateTrackAndKeepOriginalPacket) {
    Net::RudpReliableSendQueue queue;

    ASSERT_TRUE(queue.track(10, bytes(0x10)));
    EXPECT_FALSE(queue.track(10, bytes(0x20)));

    EXPECT_EQ(queue.pendingCount(), 1U);
    ASSERT_NE(queue.packetBytes(10), nullptr);
    EXPECT_EQ(*queue.packetBytes(10), bytes(0x10));
}

TEST(ReliableSendQueueTests, ConsumeAckRemovesOnlyAckSequence) {
    Net::RudpReliableSendQueue queue;
    ASSERT_TRUE(queue.track(10, bytes(0x10)));
    ASSERT_TRUE(queue.track(11, bytes(0x11)));
    ASSERT_TRUE(queue.track(12, bytes(0x12)));
    ASSERT_TRUE(queue.track(13, bytes(0x13)));

    EXPECT_EQ(queue.consumeAck(12, 0), 1U);

    EXPECT_FALSE(queue.contains(12));
    EXPECT_EQ(queue.pendingSequences(), (std::vector<uint32_t>{10, 11, 13}));
}

TEST(ReliableSendQueueTests, ConsumeAckBitsRemovesAckAndPreviousSequences) {
    Net::RudpReliableSendQueue queue;
    ASSERT_TRUE(queue.track(10, bytes(0x10)));
    ASSERT_TRUE(queue.track(11, bytes(0x11)));
    ASSERT_TRUE(queue.track(12, bytes(0x12)));
    ASSERT_TRUE(queue.track(13, bytes(0x13)));

    EXPECT_EQ(queue.consumeAck(12, 0x00000003U), 3U);

    EXPECT_EQ(queue.pendingSequences(), (std::vector<uint32_t>{13}));
}

TEST(ReliableSendQueueTests, MissingAckedSequencesAreIgnored) {
    Net::RudpReliableSendQueue queue;
    ASSERT_TRUE(queue.track(9, bytes(0x09)));
    ASSERT_TRUE(queue.track(13, bytes(0x13)));

    EXPECT_EQ(queue.consumeAck(12, 0x00000003U), 0U);

    EXPECT_EQ(queue.pendingSequences(), (std::vector<uint32_t>{9, 13}));
}

TEST(ReliableSendQueueTests, ConsumeAckHandlesUint32WrapAround) {
    Net::RudpReliableSendQueue queue;
    ASSERT_TRUE(queue.track(0xFFFFFFFFU, bytes(0xFF)));
    ASSERT_TRUE(queue.track(0U, bytes(0x00)));
    ASSERT_TRUE(queue.track(1U, bytes(0x01)));

    EXPECT_EQ(queue.consumeAck(0U, 0x00000001U), 2U);

    EXPECT_EQ(queue.pendingSequences(), (std::vector<uint32_t>{1U}));
}

TEST(ReliableSendQueueTests, PendingOutsideAckWindowIsNotRemoved) {
    Net::RudpReliableSendQueue queue;
    ASSERT_TRUE(queue.track(79, bytes(0x79)));
    ASSERT_TRUE(queue.track(100, bytes(0x10)));
    ASSERT_TRUE(queue.track(113, bytes(0x13)));

    EXPECT_EQ(queue.consumeAck(112, 0xFFFFFFFFU), 1U);

    EXPECT_EQ(queue.pendingSequences(), (std::vector<uint32_t>{79, 113}));
}

TEST(ReliableSendQueueTests, TrackIsNotImmediatelyDueForRetransmission) {
    Net::RudpReliableSendQueue queue;
    const auto sentAt = timeAt(1000);

    ASSERT_TRUE(queue.track(20, bytes(0x20), sentAt));

    EXPECT_TRUE(queue.dueForRetransmission(sentAt).empty());
    EXPECT_TRUE(queue.expiredSequences(sentAt).empty());
}

TEST(ReliableSendQueueTests, PacketIsNotDueBeforeRetransmissionTimeout) {
    Net::RudpReliableSendQueue queue;
    const auto sentAt = timeAt(1000);

    ASSERT_TRUE(queue.track(20, bytes(0x20), sentAt));

    EXPECT_TRUE(queue.dueForRetransmission(timeAt(1199)).empty());
}

TEST(ReliableSendQueueTests, PacketIsDueAtRetransmissionTimeout) {
    Net::RudpReliableSendQueue queue;
    const auto sentAt = timeAt(1000);

    ASSERT_TRUE(queue.track(20, bytes(0x20), sentAt));

    EXPECT_EQ(
        queue.dueForRetransmission(timeAt(1200)),
        (std::vector<uint32_t>{20}));
}

TEST(ReliableSendQueueTests, MarkRetransmittedUpdatesCountAndLastSentAt) {
    Net::RudpReliableSendQueue queue;
    const auto sentAt = timeAt(1000);
    const auto resentAt = timeAt(1200);

    ASSERT_TRUE(queue.track(20, bytes(0x20), sentAt));
    ASSERT_NE(queue.lastSentAt(20), nullptr);
    EXPECT_EQ(*queue.lastSentAt(20), sentAt);

    EXPECT_EQ(
        queue.dueForRetransmission(resentAt),
        (std::vector<uint32_t>{20}));
    EXPECT_TRUE(queue.markRetransmitted(20, resentAt));

    EXPECT_EQ(queue.retransmissionCount(20), 1U);
    ASSERT_NE(queue.lastSentAt(20), nullptr);
    EXPECT_EQ(*queue.lastSentAt(20), resentAt);
    EXPECT_TRUE(queue.dueForRetransmission(timeAt(1399)).empty());
    EXPECT_EQ(
        queue.dueForRetransmission(timeAt(1400)),
        (std::vector<uint32_t>{20}));
}

TEST(ReliableSendQueueTests, AckConsumedPacketIsNotRetransmissionCandidate) {
    Net::RudpReliableSendQueue queue;
    const auto sentAt = timeAt(1000);

    ASSERT_TRUE(queue.track(20, bytes(0x20), sentAt));
    EXPECT_EQ(queue.consumeAck(20, 0), 1U);

    EXPECT_TRUE(queue.dueForRetransmission(timeAt(1200)).empty());
    EXPECT_TRUE(queue.expiredSequences(timeAt(1200)).empty());
}

TEST(ReliableSendQueueTests, MaxRetransmissionsMovePacketToExpiredSequences) {
    Net::RudpReliableSendQueue queue;
    auto lastSentAt = timeAt(1000);

    ASSERT_TRUE(queue.track(20, bytes(0x20), lastSentAt));

    for (uint32_t retry = 0;
         retry < Net::RudpReliableSendQueue::kDefaultMaxRetransmissions;
         ++retry) {
        const auto dueAt =
            lastSentAt + Net::RudpReliableSendQueue::kDefaultRetransmissionTimeout;
        EXPECT_EQ(
            queue.dueForRetransmission(dueAt),
            (std::vector<uint32_t>{20}));
        EXPECT_TRUE(queue.expiredSequences(dueAt).empty());

        ASSERT_TRUE(queue.markRetransmitted(20, dueAt));
        EXPECT_EQ(queue.retransmissionCount(20), retry + 1);
        lastSentAt = dueAt;
    }

    const auto expiredAt =
        lastSentAt + Net::RudpReliableSendQueue::kDefaultRetransmissionTimeout;
    EXPECT_TRUE(queue.dueForRetransmission(expiredAt).empty());
    EXPECT_EQ(queue.expiredSequences(expiredAt), (std::vector<uint32_t>{20}));
}

TEST(ReliableSendQueueTests, MarkRetransmittedReturnsFalseForMissingSequence) {
    Net::RudpReliableSendQueue queue;

    EXPECT_FALSE(queue.markRetransmitted(404, timeAt(1000)));
}
