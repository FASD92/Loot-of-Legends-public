#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include "Net/RudpBattleStartPayload.hpp"
#include "Net/RudpGameEventPayload.hpp"
#include "Net/RudpGameplayEventIdempotencyTracker.hpp"
#include "Net/RudpMetaResponseIdempotencyTracker.hpp"
#include "Net/RudpMetaResponsePayload.hpp"
#include "Net/RudpPacket.hpp"
#include "Net/RudpReceivePipeline.hpp"
#include "Util/Time.hpp"

namespace {
Util::TimePoint timeAt(int64_t milliseconds) {
    return Util::TimePoint{std::chrono::milliseconds(milliseconds)};
}

Net::UdpEndpoint loopbackEndpoint(uint16_t port) {
    Net::UdpEndpoint endpoint;
    auto* addr = reinterpret_cast<sockaddr_in6*>(&endpoint.addr);
    addr->sin6_family = AF_INET6;
    addr->sin6_addr = in6addr_loopback;
    addr->sin6_port = htons(port);
    endpoint.addrLen = sizeof(sockaddr_in6);
    return endpoint;
}

Net::RudpPacketHeader reliableHeader(
    uint32_t sequence,
    Net::RudpChannelId channelId,
    Net::RudpPacketType packetType,
    size_t payloadSize) {
    Net::RudpPacketHeader header;
    header.flags = Net::kRudpFlagReliable;
    header.channelId = static_cast<uint8_t>(channelId);
    header.packetType = static_cast<uint16_t>(packetType);
    header.sequence = sequence;
    header.payloadLen = static_cast<uint16_t>(payloadSize);
    return header;
}

Net::RudpReceivedDatagram datagram(
    const Net::UdpEndpoint& endpoint,
    uint32_t sequence,
    Net::RudpChannelId channelId,
    Net::RudpPacketType packetType,
    const std::vector<uint8_t>& payload) {
    Net::RudpReceivedDatagram datagram;
    datagram.endpoint = endpoint;
    datagram.header = reliableHeader(
        sequence,
        channelId,
        packetType,
        payload.size());
    datagram.payload = payload;
    return datagram;
}

std::vector<uint8_t> battleStartPayload(
    uint32_t roomId,
    uint64_t playerASessionId,
    uint64_t playerBSessionId) {
    std::vector<uint8_t> bytes;
    EXPECT_TRUE(Net::serializeRudpBattleStartPayload(
        Net::RudpBattleStartPayload{
            roomId,
            playerASessionId,
            playerBSessionId},
        bytes));
    return bytes;
}

std::vector<uint8_t> monsterDeathPayload(uint32_t roomId, uint32_t monsterId) {
    std::vector<uint8_t> bytes;
    EXPECT_TRUE(Net::serializeRudpMonsterDeathGameEventPayload(
        Net::RudpMonsterDeathGameEventPayload{roomId, monsterId},
        bytes));
    return bytes;
}

std::vector<uint8_t> lootResolvedPayload(uint32_t roomId, uint32_t dropId) {
    std::vector<uint8_t> bytes;
    EXPECT_TRUE(Net::serializeRudpLootResolvedGameEventPayload(
        Net::RudpLootResolvedGameEventPayload{
            roomId,
            dropId,
            1001,
            5001,
            2},
        bytes));
    return bytes;
}

std::vector<uint8_t> metaResponsePayload(const char* settlementId) {
    std::vector<uint8_t> bytes;
    EXPECT_TRUE(Net::serializeRudpMetaResponsePayload(
        Net::RudpMetaResponsePayload{
            Net::RudpMetaResponseOp::kApplied,
            settlementId,
            Net::RudpMetaResponseStatus::kApplied,
            0},
        bytes));
    return bytes;
}

Net::RudpGameplayEventIdempotencyResult recordBattleStart(
    Net::RudpGameplayEventIdempotencyTracker& tracker,
    const std::vector<uint8_t>& payload) {
    Net::RudpBattleStartPayload parsed;
    EXPECT_TRUE(Net::parseRudpBattleStartPayload(
        payload.data(),
        payload.size(),
        parsed));
    return tracker.record(Net::RudpGameplayEventKey::battleStart(
        parsed.roomId,
        parsed.playerASessionId,
        parsed.playerBSessionId));
}

Net::RudpGameplayEventIdempotencyResult recordMonsterDeath(
    Net::RudpGameplayEventIdempotencyTracker& tracker,
    const std::vector<uint8_t>& payload) {
    Net::RudpMonsterDeathGameEventPayload parsed;
    EXPECT_TRUE(Net::parseRudpMonsterDeathGameEventPayload(
        payload.data(),
        payload.size(),
        parsed));
    return tracker.record(Net::RudpGameplayEventKey::monsterDeath(
        parsed.roomId,
        parsed.monsterId));
}

Net::RudpGameplayEventIdempotencyResult recordLootResolved(
    Net::RudpGameplayEventIdempotencyTracker& tracker,
    const std::vector<uint8_t>& payload) {
    Net::RudpLootResolvedGameEventPayload parsed;
    EXPECT_TRUE(Net::parseRudpLootResolvedGameEventPayload(
        payload.data(),
        payload.size(),
        parsed));
    return tracker.record(Net::RudpGameplayEventKey::lootResolved(
        parsed.roomId,
        parsed.dropId));
}

Net::RudpMetaResponseIdempotencyResult recordMetaResponse(
    Net::RudpMetaResponseIdempotencyTracker& tracker,
    const std::vector<uint8_t>& payload) {
    Net::RudpMetaResponsePayload parsed;
    EXPECT_TRUE(Net::parseRudpMetaResponsePayload(
        payload.data(),
        payload.size(),
        parsed));
    return tracker.record(parsed.settlementId, parsed.status);
}
}  // namespace

TEST(
    RudpReliableEventDeliveryGuardTests,
    DuplicateReliableSequenceStopsBeforeIdempotencyRecord) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));
    Net::RudpGameplayEventIdempotencyTracker tracker;
    const Net::UdpEndpoint endpoint = loopbackEndpoint(30000);
    const std::vector<uint8_t> payload = battleStartPayload(42, 1001, 1002);
    const Net::RudpReceivedDatagram packet = datagram(
        endpoint,
        10,
        Net::RudpChannelId::kEvent,
        Net::RudpPacketType::kBattleStart,
        payload);
    Net::RudpPacketDelivery delivery;

    ASSERT_EQ(
        Net::processRudpPacket(packet, registry, timeAt(0), delivery),
        Net::RudpReceivePipelineResult::kDeliver);
    EXPECT_EQ(
        recordBattleStart(tracker, delivery.payload),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);

    EXPECT_EQ(
        Net::processRudpPacket(packet, registry, timeAt(1), delivery),
        Net::RudpReceivePipelineResult::kDuplicate);
    EXPECT_EQ(delivery.peer, nullptr);
    EXPECT_TRUE(delivery.payload.empty());
    EXPECT_EQ(tracker.size(), 1U);
}

TEST(
    RudpReliableEventDeliveryGuardTests,
    NewerBattleStartLogicalDuplicateIsSuppressedByTracker) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));
    Net::RudpGameplayEventIdempotencyTracker tracker;
    const Net::UdpEndpoint endpoint = loopbackEndpoint(30001);
    const std::vector<uint8_t> payload = battleStartPayload(42, 1001, 1002);
    Net::RudpPacketDelivery delivery;

    ASSERT_EQ(
        Net::processRudpPacket(
            datagram(
                endpoint,
                20,
                Net::RudpChannelId::kEvent,
                Net::RudpPacketType::kBattleStart,
                payload),
            registry,
            timeAt(0),
            delivery),
        Net::RudpReceivePipelineResult::kDeliver);
    EXPECT_EQ(
        recordBattleStart(tracker, delivery.payload),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);

    ASSERT_EQ(
        Net::processRudpPacket(
            datagram(
                endpoint,
                21,
                Net::RudpChannelId::kEvent,
                Net::RudpPacketType::kBattleStart,
                payload),
            registry,
            timeAt(1),
            delivery),
        Net::RudpReceivePipelineResult::kDeliver);
    EXPECT_EQ(
        recordBattleStart(tracker, delivery.payload),
        Net::RudpGameplayEventIdempotencyResult::kDuplicate);
    EXPECT_EQ(tracker.size(), 1U);
}

TEST(
    RudpReliableEventDeliveryGuardTests,
    NewerGameEventLogicalDuplicatesAreSuppressedByTracker) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));
    Net::RudpGameplayEventIdempotencyTracker tracker;
    const Net::UdpEndpoint endpoint = loopbackEndpoint(30002);
    const std::vector<uint8_t> monsterPayload = monsterDeathPayload(42, 7);
    const std::vector<uint8_t> lootPayload = lootResolvedPayload(42, 77);
    Net::RudpPacketDelivery delivery;

    ASSERT_EQ(
        Net::processRudpPacket(
            datagram(
                endpoint,
                30,
                Net::RudpChannelId::kEvent,
                Net::RudpPacketType::kGameEvent,
                monsterPayload),
            registry,
            timeAt(0),
            delivery),
        Net::RudpReceivePipelineResult::kDeliver);
    EXPECT_EQ(
        recordMonsterDeath(tracker, delivery.payload),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);

    ASSERT_EQ(
        Net::processRudpPacket(
            datagram(
                endpoint,
                31,
                Net::RudpChannelId::kEvent,
                Net::RudpPacketType::kGameEvent,
                monsterPayload),
            registry,
            timeAt(1),
            delivery),
        Net::RudpReceivePipelineResult::kDeliver);
    EXPECT_EQ(
        recordMonsterDeath(tracker, delivery.payload),
        Net::RudpGameplayEventIdempotencyResult::kDuplicate);

    ASSERT_EQ(
        Net::processRudpPacket(
            datagram(
                endpoint,
                32,
                Net::RudpChannelId::kEvent,
                Net::RudpPacketType::kGameEvent,
                lootPayload),
            registry,
            timeAt(2),
            delivery),
        Net::RudpReceivePipelineResult::kDeliver);
    EXPECT_EQ(
        recordLootResolved(tracker, delivery.payload),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);

    ASSERT_EQ(
        Net::processRudpPacket(
            datagram(
                endpoint,
                33,
                Net::RudpChannelId::kEvent,
                Net::RudpPacketType::kGameEvent,
                lootPayload),
            registry,
            timeAt(3),
            delivery),
        Net::RudpReceivePipelineResult::kDeliver);
    EXPECT_EQ(
        recordLootResolved(tracker, delivery.payload),
        Net::RudpGameplayEventIdempotencyResult::kDuplicate);
    EXPECT_EQ(tracker.size(), 2U);
}

TEST(
    RudpReliableEventDeliveryGuardTests,
    NewerMetaResponseTerminalDuplicateCompletesOnce) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));
    Net::RudpMetaResponseIdempotencyTracker tracker;
    const Net::UdpEndpoint endpoint = loopbackEndpoint(30003);
    const std::vector<uint8_t> payload =
        metaResponsePayload("room-42-session-1001-finish-1");
    Net::RudpPacketDelivery delivery;

    ASSERT_EQ(
        Net::processRudpPacket(
            datagram(
                endpoint,
                40,
                Net::RudpChannelId::kControl,
                Net::RudpPacketType::kMetaResponse,
                payload),
            registry,
            timeAt(0),
            delivery),
        Net::RudpReceivePipelineResult::kDeliver);
    EXPECT_EQ(
        recordMetaResponse(tracker, delivery.payload),
        Net::RudpMetaResponseIdempotencyResult::kCompletedFirst);

    ASSERT_EQ(
        Net::processRudpPacket(
            datagram(
                endpoint,
                41,
                Net::RudpChannelId::kControl,
                Net::RudpPacketType::kMetaResponse,
                payload),
            registry,
            timeAt(1),
            delivery),
        Net::RudpReceivePipelineResult::kDeliver);
    EXPECT_EQ(
        recordMetaResponse(tracker, delivery.payload),
        Net::RudpMetaResponseIdempotencyResult::kCompletionDuplicate);
    EXPECT_EQ(tracker.completionCount(), 1U);
    EXPECT_EQ(tracker.retryCount(), 0U);
}

TEST(
    RudpReliableEventDeliveryGuardTests,
    DifferentLogicalKeysAreAcceptedAsIndependentEvents) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));
    Net::RudpGameplayEventIdempotencyTracker gameplayTracker;
    Net::RudpMetaResponseIdempotencyTracker metaTracker;
    const Net::UdpEndpoint endpoint = loopbackEndpoint(30004);
    Net::RudpPacketDelivery delivery;

    ASSERT_EQ(
        Net::processRudpPacket(
            datagram(
                endpoint,
                50,
                Net::RudpChannelId::kEvent,
                Net::RudpPacketType::kBattleStart,
                battleStartPayload(42, 1001, 1002)),
            registry,
            timeAt(0),
            delivery),
        Net::RudpReceivePipelineResult::kDeliver);
    EXPECT_EQ(
        recordBattleStart(gameplayTracker, delivery.payload),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);

    ASSERT_EQ(
        Net::processRudpPacket(
            datagram(
                endpoint,
                51,
                Net::RudpChannelId::kEvent,
                Net::RudpPacketType::kBattleStart,
                battleStartPayload(43, 1001, 1002)),
            registry,
            timeAt(1),
            delivery),
        Net::RudpReceivePipelineResult::kDeliver);
    EXPECT_EQ(
        recordBattleStart(gameplayTracker, delivery.payload),
        Net::RudpGameplayEventIdempotencyResult::kAcceptedFirst);

    ASSERT_EQ(
        Net::processRudpPacket(
            datagram(
                endpoint,
                52,
                Net::RudpChannelId::kControl,
                Net::RudpPacketType::kMetaResponse,
                metaResponsePayload("room-42-session-1001-finish-1")),
            registry,
            timeAt(2),
            delivery),
        Net::RudpReceivePipelineResult::kDeliver);
    EXPECT_EQ(
        recordMetaResponse(metaTracker, delivery.payload),
        Net::RudpMetaResponseIdempotencyResult::kCompletedFirst);

    ASSERT_EQ(
        Net::processRudpPacket(
            datagram(
                endpoint,
                53,
                Net::RudpChannelId::kControl,
                Net::RudpPacketType::kMetaResponse,
                metaResponsePayload("room-43-session-1001-finish-1")),
            registry,
            timeAt(3),
            delivery),
        Net::RudpReceivePipelineResult::kDeliver);
    EXPECT_EQ(
        recordMetaResponse(metaTracker, delivery.payload),
        Net::RudpMetaResponseIdempotencyResult::kCompletedFirst);

    EXPECT_EQ(gameplayTracker.size(), 2U);
    EXPECT_EQ(metaTracker.completionCount(), 2U);
}
