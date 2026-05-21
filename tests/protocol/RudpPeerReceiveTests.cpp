#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "Net/RudpDatagramReceiver.hpp"
#include "Net/RudpPacket.hpp"
#include "Net/RudpPeer.hpp"

namespace {
Net::RudpReceivedDatagram datagramWithHeader(Net::RudpPacketHeader header) {
    Net::RudpReceivedDatagram datagram;
    datagram.header = header;
    datagram.payload.resize(header.payloadLen);
    return datagram;
}

Net::RudpPacketHeader reliableHeader(uint32_t sequence) {
    Net::RudpPacketHeader header;
    header.flags = Net::kRudpFlagReliable;
    header.channelId = static_cast<uint8_t>(Net::RudpChannelId::kEvent);
    header.packetType = static_cast<uint16_t>(Net::RudpPacketType::kGameEvent);
    header.sequence = sequence;
    header.payloadLen = 1;
    return header;
}

Net::RudpPacketHeader ackOnlyHeader(uint32_t ack, uint32_t ackBits) {
    Net::RudpPacketHeader header;
    header.flags = Net::kRudpFlagAckOnly;
    header.channelId = static_cast<uint8_t>(Net::RudpChannelId::kControl);
    header.packetType = static_cast<uint16_t>(Net::RudpPacketType::kHello);
    header.sequence = 100;
    header.ack = ack;
    header.ackBits = ackBits;
    return header;
}

Net::RudpPacketHeader unreliableHeader(uint32_t sequence) {
    Net::RudpPacketHeader header;
    header.flags = 0;
    header.channelId = static_cast<uint8_t>(Net::RudpChannelId::kSnapshot);
    header.packetType =
        static_cast<uint16_t>(Net::RudpPacketType::kStateSnapshot);
    header.sequence = sequence;
    header.payloadLen = 1;
    return header;
}
}  // namespace

TEST(RudpPeerReceiveTests, ReliablePacketUpdatesReceiveWindowAndDelivers) {
    Net::RudpPeer peer;

    EXPECT_EQ(
        peer.receive(datagramWithHeader(reliableHeader(10))),
        Net::RudpPeerReceiveResult::kDeliver);
    EXPECT_TRUE(peer.receiveWindow().hasAck());
    EXPECT_EQ(peer.receiveWindow().ack(), 10U);
    EXPECT_EQ(peer.receiveWindow().ackBits(), 0U);
}

TEST(RudpPeerReceiveTests, DuplicateReliablePacketIsNotDelivered) {
    Net::RudpPeer peer;
    const Net::RudpReceivedDatagram datagram =
        datagramWithHeader(reliableHeader(10));

    EXPECT_EQ(peer.receive(datagram), Net::RudpPeerReceiveResult::kDeliver);
    EXPECT_EQ(peer.receive(datagram), Net::RudpPeerReceiveResult::kDuplicate);
    EXPECT_EQ(peer.receiveWindow().ack(), 10U);
    EXPECT_EQ(peer.receiveWindow().ackBits(), 0U);
}

TEST(RudpPeerReceiveTests, TooOldReliablePacketIsNotDelivered) {
    Net::RudpPeer peer;

    EXPECT_EQ(
        peer.receive(datagramWithHeader(reliableHeader(100))),
        Net::RudpPeerReceiveResult::kDeliver);
    EXPECT_EQ(
        peer.receive(datagramWithHeader(reliableHeader(67))),
        Net::RudpPeerReceiveResult::kTooOld);
    EXPECT_EQ(peer.receiveWindow().ack(), 100U);
}

TEST(RudpPeerReceiveTests, OutOfOrderReliablePacketInsideWindowDelivers) {
    Net::RudpPeer peer;

    EXPECT_EQ(
        peer.receive(datagramWithHeader(reliableHeader(10))),
        Net::RudpPeerReceiveResult::kDeliver);
    EXPECT_EQ(
        peer.receive(datagramWithHeader(reliableHeader(12))),
        Net::RudpPeerReceiveResult::kDeliver);
    EXPECT_EQ(
        peer.receive(datagramWithHeader(reliableHeader(11))),
        Net::RudpPeerReceiveResult::kDeliver);
    EXPECT_EQ(peer.receiveWindow().ack(), 12U);
    EXPECT_EQ(peer.receiveWindow().ackBits() & 0x03U, 0x03U);
}

TEST(RudpPeerReceiveTests, AckOnlyConsumesPendingReliableQueueWithoutDelivery) {
    Net::RudpPeer peer;
    ASSERT_TRUE(peer.reliableSendQueue().track(20, {0x01}));

    EXPECT_EQ(
        peer.receive(datagramWithHeader(ackOnlyHeader(20, 0))),
        Net::RudpPeerReceiveResult::kAckOnly);
    EXPECT_FALSE(peer.reliableSendQueue().contains(20));
    EXPECT_FALSE(peer.receiveWindow().hasAck());
}

TEST(RudpPeerReceiveTests, NormalPacketConsumesAckBitsFromReliableQueue) {
    Net::RudpPeer peer;
    ASSERT_TRUE(peer.reliableSendQueue().track(43, {0x01}));
    ASSERT_TRUE(peer.reliableSendQueue().track(44, {0x02}));
    ASSERT_TRUE(peer.reliableSendQueue().track(45, {0x03}));

    Net::RudpPacketHeader header = reliableHeader(10);
    header.ack = 44;
    header.ackBits = 0x01U;

    EXPECT_EQ(
        peer.receive(datagramWithHeader(header)),
        Net::RudpPeerReceiveResult::kDeliver);
    EXPECT_FALSE(peer.reliableSendQueue().contains(43));
    EXPECT_FALSE(peer.reliableSendQueue().contains(44));
    EXPECT_TRUE(peer.reliableSendQueue().contains(45));
}

TEST(RudpPeerReceiveTests, UnreliablePacketDeliversWithoutReceiveWindowFilter) {
    Net::RudpPeer peer;
    ASSERT_TRUE(peer.reliableSendQueue().track(30, {0x01}));

    Net::RudpPacketHeader header = unreliableHeader(7);
    header.ack = 30;

    EXPECT_EQ(
        peer.receive(datagramWithHeader(header)),
        Net::RudpPeerReceiveResult::kDeliver);
    EXPECT_EQ(
        peer.receive(datagramWithHeader(header)),
        Net::RudpPeerReceiveResult::kDeliver);
    EXPECT_FALSE(peer.reliableSendQueue().contains(30));
    EXPECT_FALSE(peer.receiveWindow().hasAck());
}
