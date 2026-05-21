#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "Net/RudpPacket.hpp"
#include "Net/RudpReceivePipeline.hpp"
#include "Net/UdpSocket.hpp"
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
    header.sequence = 30;
    header.ack = ack;
    header.ackBits = ackBits;
    return header;
}

Net::RudpReceivedDatagram datagramWithHeader(
    Net::UdpEndpoint endpoint,
    Net::RudpPacketHeader header) {
    Net::RudpReceivedDatagram datagram;
    datagram.endpoint = endpoint;
    datagram.header = header;
    datagram.payload.resize(header.payloadLen);
    return datagram;
}

std::vector<uint8_t> serializePacket(
    const Net::RudpPacketHeader& header,
    const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> packet;
    EXPECT_TRUE(Net::serializeRudpPacket(header, payload, packet));
    return packet;
}

void sendPacket(
    Net::UdpSocket& sender,
    const std::vector<uint8_t>& packet,
    uint16_t receiverPort) {
    ASSERT_TRUE(sender.sendTo(
        packet.data(),
        packet.size(),
        loopbackEndpoint(receiverPort)));
}

Net::RudpReceivePipelineResult receiveWithWait(
    Net::UdpSocket& socket,
    Net::RudpPeerRegistry& registry,
    Util::TimePoint now,
    Net::RudpPacketDelivery& delivery) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        const Net::RudpReceivePipelineResult result =
            Net::receiveRudpPacket(socket, registry, now, delivery);
        if (result != Net::RudpReceivePipelineResult::kWouldBlock) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return Net::RudpReceivePipelineResult::kWouldBlock;
}
}  // namespace

TEST(RudpReceivePipelineTests, EmptySocketReturnsWouldBlockWithoutPeer) {
    Net::UdpSocket socket;
    ASSERT_TRUE(socket.open(0));
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));
    Net::RudpPacketDelivery delivery;

    EXPECT_EQ(
        Net::receiveRudpPacket(socket, registry, timeAt(0), delivery),
        Net::RudpReceivePipelineResult::kWouldBlock);
    EXPECT_EQ(registry.size(), 0U);
    EXPECT_EQ(delivery.peer, nullptr);
}

TEST(RudpReceivePipelineTests, MalformedUdpDatagramDoesNotCreatePeer) {
    Net::UdpSocket receiver;
    Net::UdpSocket sender;
    ASSERT_TRUE(receiver.open(0));
    ASSERT_TRUE(sender.open(0));
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));

    std::vector<uint8_t> packet = serializePacket(reliableHeader(10), {0x01});
    packet[0] = 0x00;
    sendPacket(sender, packet, receiver.boundPort());

    Net::RudpPacketDelivery delivery;
    EXPECT_EQ(
        receiveWithWait(receiver, registry, timeAt(0), delivery),
        Net::RudpReceivePipelineResult::kMalformed);
    EXPECT_EQ(registry.size(), 0U);
    EXPECT_EQ(delivery.peer, nullptr);
}

TEST(RudpReceivePipelineTests, ValidReliableUdpDatagramDeliversPacket) {
    Net::UdpSocket receiver;
    Net::UdpSocket sender;
    ASSERT_TRUE(receiver.open(0));
    ASSERT_TRUE(sender.open(0));
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));

    const std::vector<uint8_t> payload{0x01, 0x02};
    Net::RudpPacketHeader header = reliableHeader(10);
    header.payloadLen = static_cast<uint16_t>(payload.size());
    sendPacket(sender, serializePacket(header, payload), receiver.boundPort());

    Net::RudpPacketDelivery delivery;
    ASSERT_EQ(
        receiveWithWait(receiver, registry, timeAt(0), delivery),
        Net::RudpReceivePipelineResult::kDeliver);
    ASSERT_NE(delivery.peer, nullptr);
    EXPECT_EQ(delivery.header.sequence, header.sequence);
    EXPECT_EQ(delivery.header.payloadLen, payload.size());
    EXPECT_EQ(delivery.payload, payload);
    EXPECT_EQ(registry.size(), 1U);
}

TEST(RudpReceivePipelineTests, DuplicateReliableDatagramDoesNotDeliver) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));
    const Net::UdpEndpoint endpoint = loopbackEndpoint(30000);
    const Net::RudpReceivedDatagram datagram =
        datagramWithHeader(endpoint, reliableHeader(10));
    Net::RudpPacketDelivery delivery;

    EXPECT_EQ(
        Net::processRudpPacket(datagram, registry, timeAt(0), delivery),
        Net::RudpReceivePipelineResult::kDeliver);
    EXPECT_NE(delivery.peer, nullptr);

    EXPECT_EQ(
        Net::processRudpPacket(datagram, registry, timeAt(1), delivery),
        Net::RudpReceivePipelineResult::kDuplicate);
    EXPECT_EQ(delivery.peer, nullptr);
    EXPECT_TRUE(delivery.payload.empty());
    EXPECT_EQ(registry.size(), 1U);
}

TEST(RudpReceivePipelineTests, AckOnlyConsumesPendingQueueWithoutDelivery) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));
    const Net::UdpEndpoint endpoint = loopbackEndpoint(30000);
    Net::RudpPeer* peer = registry.findOrCreate(endpoint, timeAt(0));
    ASSERT_NE(peer, nullptr);
    ASSERT_TRUE(peer->reliableSendQueue().track(20, {0x01}));

    const Net::RudpReceivedDatagram datagram =
        datagramWithHeader(endpoint, ackOnlyHeader(20, 0));
    Net::RudpPacketDelivery delivery;

    EXPECT_EQ(
        Net::processRudpPacket(datagram, registry, timeAt(1), delivery),
        Net::RudpReceivePipelineResult::kAckOnly);
    EXPECT_FALSE(peer->reliableSendQueue().contains(20));
    EXPECT_EQ(delivery.peer, nullptr);
}

TEST(RudpReceivePipelineTests, InvalidEndpointReturnsInvalidEndpoint) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));
    Net::UdpEndpoint invalidEndpoint;
    const Net::RudpReceivedDatagram datagram =
        datagramWithHeader(invalidEndpoint, reliableHeader(10));
    Net::RudpPacketDelivery delivery;

    EXPECT_EQ(
        Net::processRudpPacket(datagram, registry, timeAt(0), delivery),
        Net::RudpReceivePipelineResult::kInvalidEndpoint);
    EXPECT_EQ(registry.size(), 0U);
    EXPECT_EQ(delivery.peer, nullptr);
}
