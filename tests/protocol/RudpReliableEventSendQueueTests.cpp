#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "Net/RudpReliableEventSendQueue.hpp"

namespace {
Net::RudpReliableEventSendQueue::TimePoint timeAt(int64_t milliseconds) {
    return Net::RudpReliableEventSendQueue::TimePoint{
        std::chrono::milliseconds(milliseconds)};
}

std::vector<uint8_t> packetBytes(uint8_t marker) {
    return {0x4C, 0x4F, marker};
}

Net::RudpReliableEventDescriptor descriptor(
    Net::RudpReliableEventKind kind,
    const std::string& logicalKey) {
    switch (kind) {
        case Net::RudpReliableEventKind::kBattleStart:
            return Net::RudpReliableEventDescriptor{
                kind,
                logicalKey,
                static_cast<uint16_t>(Net::RudpPacketType::kBattleStart),
                static_cast<uint8_t>(Net::RudpChannelId::kEvent)};
        case Net::RudpReliableEventKind::kMonsterDeath:
        case Net::RudpReliableEventKind::kLootResolved:
            return Net::RudpReliableEventDescriptor{
                kind,
                logicalKey,
                static_cast<uint16_t>(Net::RudpPacketType::kGameEvent),
                static_cast<uint8_t>(Net::RudpChannelId::kEvent)};
        case Net::RudpReliableEventKind::kMetaResponse:
            return Net::RudpReliableEventDescriptor{
                kind,
                logicalKey,
                static_cast<uint16_t>(Net::RudpPacketType::kMetaResponse),
                static_cast<uint8_t>(Net::RudpChannelId::kControl)};
    }

    return Net::RudpReliableEventDescriptor{
        kind,
        logicalKey,
        static_cast<uint16_t>(Net::RudpPacketType::kError),
        static_cast<uint8_t>(Net::RudpChannelId::kControl)};
}
}  // namespace

TEST(RudpReliableEventSendQueueTests, TracksValidReliableEventDescriptors) {
    Net::RudpReliableEventSendQueue queue;
    const std::vector<Net::RudpReliableEventDescriptor> descriptors = {
        descriptor(Net::RudpReliableEventKind::kBattleStart, "room-42:1001:1002"),
        descriptor(Net::RudpReliableEventKind::kMonsterDeath, "room-42:monster-7"),
        descriptor(Net::RudpReliableEventKind::kLootResolved, "room-42:drop-77"),
        descriptor(Net::RudpReliableEventKind::kMetaResponse, "settlement-1"),
    };

    for (size_t i = 0; i < descriptors.size(); ++i) {
        EXPECT_EQ(
            queue.track(
                descriptors[i],
                static_cast<uint32_t>(100 + i),
                packetBytes(static_cast<uint8_t>(i)),
                timeAt(1000)),
            Net::RudpReliableEventTrackResult::kTracked);
    }

    EXPECT_EQ(queue.pendingCount(), descriptors.size());
    EXPECT_EQ(
        queue.pendingSequences(),
        (std::vector<uint32_t>{100, 101, 102, 103}));
}

TEST(RudpReliableEventSendQueueTests, TrackStoresEventMetadataAndPacketBytes) {
    Net::RudpReliableEventSendQueue queue;
    const Net::RudpReliableEventDescriptor event =
        descriptor(Net::RudpReliableEventKind::kBattleStart, "room-42:1001:1002");

    ASSERT_EQ(
        queue.track(event, 10, packetBytes(0x10), timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);

    EXPECT_TRUE(queue.containsSequence(10));
    EXPECT_TRUE(queue.containsLogicalEvent(
        Net::RudpReliableEventKind::kBattleStart,
        "room-42:1001:1002"));

    const Net::RudpReliableEventPendingEntry* entry = queue.pendingEntry(10);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->sequence, 10U);
    EXPECT_EQ(entry->descriptor.kind, event.kind);
    EXPECT_EQ(entry->descriptor.logicalKey, event.logicalKey);
    EXPECT_EQ(entry->descriptor.packetType, event.packetType);
    EXPECT_EQ(entry->descriptor.channelId, event.channelId);

    ASSERT_NE(queue.packetBytes(10), nullptr);
    EXPECT_EQ(*queue.packetBytes(10), packetBytes(0x10));
}

TEST(RudpReliableEventSendQueueTests, DuplicateSequenceKeepsOriginalPacket) {
    Net::RudpReliableEventSendQueue queue;
    ASSERT_EQ(
        queue.track(
            descriptor(Net::RudpReliableEventKind::kBattleStart, "battle-a"),
            10,
            packetBytes(0x10),
            timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);

    EXPECT_EQ(
        queue.track(
            descriptor(Net::RudpReliableEventKind::kMonsterDeath, "monster-b"),
            10,
            packetBytes(0x20),
            timeAt(1000)),
        Net::RudpReliableEventTrackResult::kDuplicateSequence);

    EXPECT_EQ(queue.pendingCount(), 1U);
    ASSERT_NE(queue.packetBytes(10), nullptr);
    EXPECT_EQ(*queue.packetBytes(10), packetBytes(0x10));
    EXPECT_FALSE(queue.containsLogicalEvent(
        Net::RudpReliableEventKind::kMonsterDeath,
        "monster-b"));
}

TEST(RudpReliableEventSendQueueTests, DuplicateLogicalEventDoesNotTrackNewSequence) {
    Net::RudpReliableEventSendQueue queue;
    const Net::RudpReliableEventDescriptor event =
        descriptor(Net::RudpReliableEventKind::kLootResolved, "room-42:drop-77");
    ASSERT_EQ(
        queue.track(event, 20, packetBytes(0x20), timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);

    EXPECT_EQ(
        queue.track(event, 21, packetBytes(0x21), timeAt(1000)),
        Net::RudpReliableEventTrackResult::kDuplicateLogicalEvent);

    EXPECT_EQ(queue.pendingCount(), 1U);
    EXPECT_TRUE(queue.containsSequence(20));
    EXPECT_FALSE(queue.containsSequence(21));
    EXPECT_EQ(queue.pendingSequences(), (std::vector<uint32_t>{20}));
}

TEST(RudpReliableEventSendQueueTests, SameLogicalKeyAcrossDifferentKindsDoesNotCollide) {
    Net::RudpReliableEventSendQueue queue;

    EXPECT_EQ(
        queue.track(
            descriptor(Net::RudpReliableEventKind::kBattleStart, "shared-key"),
            30,
            packetBytes(0x30),
            timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);
    EXPECT_EQ(
        queue.track(
            descriptor(Net::RudpReliableEventKind::kMetaResponse, "shared-key"),
            31,
            packetBytes(0x31),
            timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);

    EXPECT_EQ(queue.pendingCount(), 2U);
    EXPECT_TRUE(queue.containsLogicalEvent(
        Net::RudpReliableEventKind::kBattleStart,
        "shared-key"));
    EXPECT_TRUE(queue.containsLogicalEvent(
        Net::RudpReliableEventKind::kMetaResponse,
        "shared-key"));
}

TEST(RudpReliableEventSendQueueTests, RejectsInvalidDescriptors) {
    Net::RudpReliableEventSendQueue queue;
    const std::vector<Net::RudpReliableEventDescriptor> invalidDescriptors = {
        descriptor(Net::RudpReliableEventKind::kBattleStart, ""),
        Net::RudpReliableEventDescriptor{
            Net::RudpReliableEventKind::kBattleStart,
            "wrong-packet",
            static_cast<uint16_t>(Net::RudpPacketType::kGameEvent),
            static_cast<uint8_t>(Net::RudpChannelId::kEvent)},
        Net::RudpReliableEventDescriptor{
            Net::RudpReliableEventKind::kMonsterDeath,
            "wrong-channel",
            static_cast<uint16_t>(Net::RudpPacketType::kGameEvent),
            static_cast<uint8_t>(Net::RudpChannelId::kControl)},
        Net::RudpReliableEventDescriptor{
            Net::RudpReliableEventKind::kMetaResponse,
            "wrong-channel",
            static_cast<uint16_t>(Net::RudpPacketType::kMetaResponse),
            static_cast<uint8_t>(Net::RudpChannelId::kEvent)},
        Net::RudpReliableEventDescriptor{
            static_cast<Net::RudpReliableEventKind>(99),
            "invalid-kind",
            static_cast<uint16_t>(Net::RudpPacketType::kError),
            static_cast<uint8_t>(Net::RudpChannelId::kControl)},
    };

    for (const Net::RudpReliableEventDescriptor& event : invalidDescriptors) {
        EXPECT_EQ(
            queue.track(event, 40, packetBytes(0x40), timeAt(1000)),
            Net::RudpReliableEventTrackResult::kInvalidDescriptor);
        EXPECT_EQ(queue.pendingCount(), 0U);
    }
}

TEST(RudpReliableEventSendQueueTests, RejectsEmptyPacketBytes) {
    Net::RudpReliableEventSendQueue queue;

    EXPECT_EQ(
        queue.track(
            descriptor(Net::RudpReliableEventKind::kMetaResponse, "settlement-1"),
            50,
            {},
            timeAt(1000)),
        Net::RudpReliableEventTrackResult::kInvalidPacketBytes);

    EXPECT_EQ(queue.pendingCount(), 0U);
    EXPECT_FALSE(queue.containsSequence(50));
    EXPECT_EQ(queue.packetBytes(50), nullptr);
}

TEST(RudpReliableEventSendQueueTests, ConsumeAckRemovesSequenceAndEventMetadata) {
    Net::RudpReliableEventSendQueue queue;
    ASSERT_EQ(
        queue.track(
            descriptor(Net::RudpReliableEventKind::kBattleStart, "battle-a"),
            10,
            packetBytes(0x10),
            timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);
    ASSERT_EQ(
        queue.track(
            descriptor(Net::RudpReliableEventKind::kMonsterDeath, "monster-b"),
            11,
            packetBytes(0x11),
            timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);
    ASSERT_EQ(
        queue.track(
            descriptor(Net::RudpReliableEventKind::kLootResolved, "loot-c"),
            12,
            packetBytes(0x12),
            timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);

    EXPECT_EQ(queue.consumeAck(11, 0), 1U);

    EXPECT_EQ(queue.pendingCount(), 2U);
    EXPECT_EQ(queue.pendingSequences(), (std::vector<uint32_t>{10, 12}));
    EXPECT_FALSE(queue.containsSequence(11));
    EXPECT_FALSE(queue.containsLogicalEvent(
        Net::RudpReliableEventKind::kMonsterDeath,
        "monster-b"));
    EXPECT_EQ(queue.packetBytes(11), nullptr);
    EXPECT_TRUE(queue.containsLogicalEvent(
        Net::RudpReliableEventKind::kBattleStart,
        "battle-a"));
    EXPECT_TRUE(queue.containsLogicalEvent(
        Net::RudpReliableEventKind::kLootResolved,
        "loot-c"));
}

TEST(RudpReliableEventSendQueueTests, RemoveDropsPacketBytesAndEventMetadata) {
    Net::RudpReliableEventSendQueue queue;
    ASSERT_EQ(
        queue.track(
            descriptor(Net::RudpReliableEventKind::kBattleStart, "battle-a"),
            10,
            packetBytes(0x10),
            timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);
    ASSERT_EQ(
        queue.track(
            descriptor(Net::RudpReliableEventKind::kMonsterDeath, "monster-b"),
            11,
            packetBytes(0x11),
            timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);

    EXPECT_TRUE(queue.remove(10));
    EXPECT_FALSE(queue.remove(10));
    EXPECT_EQ(queue.pendingCount(), 1U);
    EXPECT_EQ(queue.pendingSequences(), (std::vector<uint32_t>{11}));
    EXPECT_EQ(queue.packetBytes(10), nullptr);
    EXPECT_FALSE(queue.containsLogicalEvent(
        Net::RudpReliableEventKind::kBattleStart,
        "battle-a"));
    EXPECT_TRUE(queue.containsLogicalEvent(
        Net::RudpReliableEventKind::kMonsterDeath,
        "monster-b"));
}

TEST(RudpReliableEventSendQueueTests, ConsumeAckBitsRemovesMultipleEventMetadata) {
    Net::RudpReliableEventSendQueue queue;
    ASSERT_EQ(
        queue.track(
            descriptor(Net::RudpReliableEventKind::kBattleStart, "battle-a"),
            10,
            packetBytes(0x10),
            timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);
    ASSERT_EQ(
        queue.track(
            descriptor(Net::RudpReliableEventKind::kMonsterDeath, "monster-b"),
            11,
            packetBytes(0x11),
            timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);
    ASSERT_EQ(
        queue.track(
            descriptor(Net::RudpReliableEventKind::kLootResolved, "loot-c"),
            12,
            packetBytes(0x12),
            timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);
    ASSERT_EQ(
        queue.track(
            descriptor(Net::RudpReliableEventKind::kMetaResponse, "settlement-d"),
            13,
            packetBytes(0x13),
            timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);

    EXPECT_EQ(queue.consumeAck(12, 0x00000003U), 3U);

    EXPECT_EQ(queue.pendingCount(), 1U);
    EXPECT_EQ(queue.pendingSequences(), (std::vector<uint32_t>{13}));
    EXPECT_FALSE(queue.containsLogicalEvent(
        Net::RudpReliableEventKind::kBattleStart,
        "battle-a"));
    EXPECT_FALSE(queue.containsLogicalEvent(
        Net::RudpReliableEventKind::kMonsterDeath,
        "monster-b"));
    EXPECT_FALSE(queue.containsLogicalEvent(
        Net::RudpReliableEventKind::kLootResolved,
        "loot-c"));
    EXPECT_TRUE(queue.containsLogicalEvent(
        Net::RudpReliableEventKind::kMetaResponse,
        "settlement-d"));
}

TEST(RudpReliableEventSendQueueTests, MissingAckAndOutsideWindowKeepPendingEvents) {
    Net::RudpReliableEventSendQueue missingSequenceQueue;
    ASSERT_EQ(
        missingSequenceQueue.track(
            descriptor(Net::RudpReliableEventKind::kBattleStart, "old-battle"),
            9,
            packetBytes(0x09),
            timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);
    ASSERT_EQ(
        missingSequenceQueue.track(
            descriptor(Net::RudpReliableEventKind::kMonsterDeath, "new-monster"),
            13,
            packetBytes(0x13),
            timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);

    EXPECT_EQ(missingSequenceQueue.consumeAck(12, 0x00000003U), 0U);
    EXPECT_EQ(
        missingSequenceQueue.pendingSequences(),
        (std::vector<uint32_t>{9, 13}));

    Net::RudpReliableEventSendQueue outsideWindowQueue;
    ASSERT_EQ(
        outsideWindowQueue.track(
            descriptor(Net::RudpReliableEventKind::kBattleStart, "outside-battle"),
            79,
            packetBytes(0x79),
            timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);
    ASSERT_EQ(
        outsideWindowQueue.track(
            descriptor(Net::RudpReliableEventKind::kMonsterDeath, "future-monster"),
            113,
            packetBytes(0x13),
            timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);

    EXPECT_EQ(outsideWindowQueue.consumeAck(112, 0xFFFFFFFFU), 0U);
    EXPECT_EQ(
        outsideWindowQueue.pendingSequences(),
        (std::vector<uint32_t>{79, 113}));
}

TEST(RudpReliableEventSendQueueTests, AckConsumedLogicalEventCanBeTrackedAgain) {
    Net::RudpReliableEventSendQueue queue;
    const Net::RudpReliableEventDescriptor event =
        descriptor(Net::RudpReliableEventKind::kBattleStart, "battle-a");
    ASSERT_EQ(
        queue.track(event, 20, packetBytes(0x20), timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);
    ASSERT_EQ(queue.consumeAck(20, 0), 1U);

    EXPECT_FALSE(queue.containsSequence(20));
    EXPECT_FALSE(queue.containsLogicalEvent(
        Net::RudpReliableEventKind::kBattleStart,
        "battle-a"));
    EXPECT_EQ(
        queue.track(event, 21, packetBytes(0x21), timeAt(1200)),
        Net::RudpReliableEventTrackResult::kTracked);
    EXPECT_EQ(queue.pendingSequences(), (std::vector<uint32_t>{21}));
}

TEST(RudpReliableEventSendQueueTests, RetransmissionTimeoutMarksDueEventSequences) {
    Net::RudpReliableEventSendQueue queue;
    const auto sentAt = timeAt(1000);
    ASSERT_EQ(
        queue.track(
            descriptor(Net::RudpReliableEventKind::kLootResolved, "loot-a"),
            30,
            packetBytes(0x30),
            sentAt),
        Net::RudpReliableEventTrackResult::kTracked);

    EXPECT_TRUE(queue.dueForRetransmission(sentAt).empty());
    EXPECT_TRUE(queue.dueForRetransmission(timeAt(1199)).empty());
    EXPECT_TRUE(queue.expiredSequences(timeAt(1200)).empty());
    EXPECT_EQ(
        queue.dueForRetransmission(timeAt(1200)),
        (std::vector<uint32_t>{30}));
}

TEST(RudpReliableEventSendQueueTests, MarkRetransmittedUpdatesCountAndLastSentAt) {
    Net::RudpReliableEventSendQueue queue;
    const auto sentAt = timeAt(1000);
    const auto resentAt = timeAt(1200);
    ASSERT_EQ(
        queue.track(
            descriptor(Net::RudpReliableEventKind::kMetaResponse, "settlement-a"),
            40,
            packetBytes(0x40),
            sentAt),
        Net::RudpReliableEventTrackResult::kTracked);

    ASSERT_NE(queue.lastSentAt(40), nullptr);
    EXPECT_EQ(*queue.lastSentAt(40), sentAt);
    EXPECT_EQ(
        queue.dueForRetransmission(resentAt),
        (std::vector<uint32_t>{40}));
    EXPECT_TRUE(queue.markRetransmitted(40, resentAt));

    EXPECT_EQ(queue.retransmissionCount(40), 1U);
    ASSERT_NE(queue.lastSentAt(40), nullptr);
    EXPECT_EQ(*queue.lastSentAt(40), resentAt);
    EXPECT_TRUE(queue.dueForRetransmission(timeAt(1399)).empty());
    EXPECT_EQ(
        queue.dueForRetransmission(timeAt(1400)),
        (std::vector<uint32_t>{40}));
}

TEST(RudpReliableEventSendQueueTests, MaxRetransmissionsMoveEventToExpiredSequences) {
    Net::RudpReliableEventSendQueue queue;
    auto lastSentAt = timeAt(1000);
    ASSERT_EQ(
        queue.track(
            descriptor(Net::RudpReliableEventKind::kBattleStart, "battle-max-retry"),
            50,
            packetBytes(0x50),
            lastSentAt),
        Net::RudpReliableEventTrackResult::kTracked);

    for (uint32_t retry = 0;
         retry < Net::RudpReliableSendQueue::kDefaultMaxRetransmissions;
         ++retry) {
        const auto dueAt =
            lastSentAt + Net::RudpReliableSendQueue::kDefaultRetransmissionTimeout;
        EXPECT_EQ(
            queue.dueForRetransmission(dueAt),
            (std::vector<uint32_t>{50}));
        EXPECT_TRUE(queue.expiredSequences(dueAt).empty());

        ASSERT_TRUE(queue.markRetransmitted(50, dueAt));
        EXPECT_EQ(queue.retransmissionCount(50), retry + 1);
        lastSentAt = dueAt;
    }

    const auto expiredAt =
        lastSentAt + Net::RudpReliableSendQueue::kDefaultRetransmissionTimeout;
    EXPECT_TRUE(queue.dueForRetransmission(expiredAt).empty());
    EXPECT_EQ(queue.expiredSequences(expiredAt), (std::vector<uint32_t>{50}));
}

TEST(RudpReliableEventSendQueueTests, AckConsumedEventIsNotRetransmissionCandidate) {
    Net::RudpReliableEventSendQueue queue;
    ASSERT_EQ(
        queue.track(
            descriptor(Net::RudpReliableEventKind::kMonsterDeath, "monster-a"),
            60,
            packetBytes(0x60),
            timeAt(1000)),
        Net::RudpReliableEventTrackResult::kTracked);
    ASSERT_EQ(queue.consumeAck(60, 0), 1U);

    EXPECT_TRUE(queue.dueForRetransmission(timeAt(1200)).empty());
    EXPECT_TRUE(queue.expiredSequences(timeAt(1200)).empty());
    EXPECT_EQ(queue.packetBytes(60), nullptr);
    EXPECT_EQ(queue.lastSentAt(60), nullptr);
}
