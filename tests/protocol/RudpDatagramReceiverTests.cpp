#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "Net/RudpDatagramReceiver.hpp"
#include "Net/RudpPacket.hpp"
#include "Net/UdpSocket.hpp"

namespace {
Net::UdpEndpoint loopbackEndpoint(uint16_t port) {
    Net::UdpEndpoint endpoint;
    auto* addr = reinterpret_cast<sockaddr_in6*>(&endpoint.addr);
    addr->sin6_family = AF_INET6;
    addr->sin6_addr = in6addr_loopback;
    addr->sin6_port = htons(port);
    endpoint.addrLen = sizeof(sockaddr_in6);
    return endpoint;
}

Net::RudpPacketHeader reliableHeader(uint32_t sequence = 10) {
    Net::RudpPacketHeader header;
    header.flags = Net::kRudpFlagReliable;
    header.channelId = static_cast<uint8_t>(Net::RudpChannelId::kEvent);
    header.packetType = static_cast<uint16_t>(Net::RudpPacketType::kGameEvent);
    header.sequence = sequence;
    header.ack = 7;
    header.ackBits = 0x00000003U;
    return header;
}

Net::RudpPacketHeader ackOnlyHeader() {
    Net::RudpPacketHeader header;
    header.flags = Net::kRudpFlagAckOnly;
    header.channelId = static_cast<uint8_t>(Net::RudpChannelId::kControl);
    header.packetType = static_cast<uint16_t>(Net::RudpPacketType::kHello);
    header.sequence = 30;
    header.ack = 29;
    header.ackBits = 0x0000000FU;
    return header;
}

std::vector<uint8_t> serializePacket(
    const Net::RudpPacketHeader& header,
    const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> packet;
    EXPECT_TRUE(Net::serializeRudpPacket(header, payload, packet));
    return packet;
}

Net::RudpReceiveResult receiveWithWait(
    Net::UdpSocket& socket,
    Net::RudpReceivedDatagram& datagram) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        const Net::RudpReceiveResult result =
            Net::receiveRudpDatagram(socket, datagram);
        if (result != Net::RudpReceiveResult::kWouldBlock) {
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return Net::RudpReceiveResult::kWouldBlock;
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
}  // namespace

TEST(RudpDatagramReceiverTests, EmptySocketReturnsWouldBlock) {
    Net::UdpSocket socket;
    ASSERT_TRUE(socket.open(0));

    Net::RudpReceivedDatagram datagram;
    EXPECT_EQ(
        Net::receiveRudpDatagram(socket, datagram),
        Net::RudpReceiveResult::kWouldBlock);
}

TEST(RudpDatagramReceiverTests, ClosedSocketReturnsSocketError) {
    Net::UdpSocket socket;

    Net::RudpReceivedDatagram datagram;
    EXPECT_EQ(
        Net::receiveRudpDatagram(socket, datagram),
        Net::RudpReceiveResult::kSocketError);
}

TEST(RudpDatagramReceiverTests, ReceivesValidReliablePacket) {
    Net::UdpSocket receiver;
    Net::UdpSocket sender;
    ASSERT_TRUE(receiver.open(0));
    ASSERT_TRUE(sender.open(0));

    const std::vector<uint8_t> payload{0x00, 0x01, 0x00, 0x00};
    const Net::RudpPacketHeader header = reliableHeader();
    sendPacket(sender, serializePacket(header, payload), receiver.boundPort());

    Net::RudpReceivedDatagram datagram;
    ASSERT_EQ(
        receiveWithWait(receiver, datagram),
        Net::RudpReceiveResult::kReceived);
    EXPECT_EQ(datagram.header.flags, header.flags);
    EXPECT_EQ(datagram.header.channelId, header.channelId);
    EXPECT_EQ(datagram.header.packetType, header.packetType);
    EXPECT_EQ(datagram.header.sequence, header.sequence);
    EXPECT_EQ(datagram.header.ack, header.ack);
    EXPECT_EQ(datagram.header.ackBits, header.ackBits);
    EXPECT_EQ(datagram.header.payloadLen, payload.size());
    EXPECT_EQ(datagram.payload, payload);
    EXPECT_NE(Net::endpointToString(datagram.endpoint), "unknown");
}

TEST(RudpDatagramReceiverTests, ReceivesValidAckOnlyPacket) {
    Net::UdpSocket receiver;
    Net::UdpSocket sender;
    ASSERT_TRUE(receiver.open(0));
    ASSERT_TRUE(sender.open(0));

    const Net::RudpPacketHeader header = ackOnlyHeader();
    sendPacket(sender, serializePacket(header, {}), receiver.boundPort());

    Net::RudpReceivedDatagram datagram;
    ASSERT_EQ(
        receiveWithWait(receiver, datagram),
        Net::RudpReceiveResult::kReceived);
    EXPECT_EQ(datagram.header.flags, header.flags);
    EXPECT_EQ(datagram.header.channelId, header.channelId);
    EXPECT_EQ(datagram.header.packetType, header.packetType);
    EXPECT_EQ(datagram.header.sequence, header.sequence);
    EXPECT_EQ(datagram.header.payloadLen, 0);
    EXPECT_TRUE(datagram.payload.empty());
}

TEST(RudpDatagramReceiverTests, InvalidMagicReturnsMalformed) {
    Net::UdpSocket receiver;
    Net::UdpSocket sender;
    ASSERT_TRUE(receiver.open(0));
    ASSERT_TRUE(sender.open(0));

    std::vector<uint8_t> packet = serializePacket(reliableHeader(), {0x01});
    packet[0] = 0x00;
    sendPacket(sender, packet, receiver.boundPort());

    Net::RudpReceivedDatagram datagram;
    datagram.payload = {0xAA};
    EXPECT_EQ(
        receiveWithWait(receiver, datagram),
        Net::RudpReceiveResult::kMalformed);
    EXPECT_TRUE(datagram.payload.empty());
}

TEST(RudpDatagramReceiverTests, ChecksumMismatchReturnsMalformed) {
    Net::UdpSocket receiver;
    Net::UdpSocket sender;
    ASSERT_TRUE(receiver.open(0));
    ASSERT_TRUE(sender.open(0));

    std::vector<uint8_t> packet = serializePacket(reliableHeader(), {0x01});
    packet.back() ^= 0xFF;
    sendPacket(sender, packet, receiver.boundPort());

    Net::RudpReceivedDatagram datagram;
    EXPECT_EQ(
        receiveWithWait(receiver, datagram),
        Net::RudpReceiveResult::kMalformed);
    EXPECT_TRUE(datagram.payload.empty());
}

TEST(RudpDatagramReceiverTests, MalformedDatagramDoesNotBlockNextValidPacket) {
    Net::UdpSocket receiver;
    Net::UdpSocket sender;
    ASSERT_TRUE(receiver.open(0));
    ASSERT_TRUE(sender.open(0));

    std::vector<uint8_t> malformed = serializePacket(reliableHeader(40), {0x01});
    malformed[0] = 0x00;
    const std::vector<uint8_t> payload{0x00, 0x02};
    const Net::RudpPacketHeader validHeader = reliableHeader(41);

    sendPacket(sender, malformed, receiver.boundPort());
    sendPacket(sender, serializePacket(validHeader, payload), receiver.boundPort());

    Net::RudpReceivedDatagram datagram;
    EXPECT_EQ(
        receiveWithWait(receiver, datagram),
        Net::RudpReceiveResult::kMalformed);
    ASSERT_EQ(
        receiveWithWait(receiver, datagram),
        Net::RudpReceiveResult::kReceived);
    EXPECT_EQ(datagram.header.sequence, validHeader.sequence);
    EXPECT_EQ(datagram.payload, payload);
}
