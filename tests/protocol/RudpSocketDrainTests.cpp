#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

#include "Net/RudpPacket.hpp"
#include "Net/RudpSocketDrain.hpp"
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

Net::RudpPacketHeader ackOnlyHeader(uint32_t ack) {
    Net::RudpPacketHeader header;
    header.flags = Net::kRudpFlagAckOnly;
    header.channelId = static_cast<uint8_t>(Net::RudpChannelId::kControl);
    header.packetType = static_cast<uint16_t>(Net::RudpPacketType::kHello);
    header.sequence = 30;
    header.ack = ack;
    return header;
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

bool hasActivity(const Net::RudpSocketDrainSummary& summary) {
    return summary.delivered > 0 ||
        summary.malformed > 0 ||
        summary.invalidEndpoint > 0 ||
        summary.ackOnly > 0 ||
        summary.duplicate > 0 ||
        summary.tooOld > 0 ||
        summary.socketErrors > 0 ||
        summary.stoppedByMaxPackets ||
        summary.stoppedBySocketError;
}

void mergeSummary(
    Net::RudpSocketDrainSummary& target,
    Net::RudpSocketDrainSummary source) {
    target.attempted += source.attempted;
    target.delivered += source.delivered;
    target.malformed += source.malformed;
    target.invalidEndpoint += source.invalidEndpoint;
    target.ackOnly += source.ackOnly;
    target.duplicate += source.duplicate;
    target.tooOld += source.tooOld;
    target.socketErrors += source.socketErrors;
    target.stoppedByWouldBlock =
        target.stoppedByWouldBlock || source.stoppedByWouldBlock;
    target.stoppedByMaxPackets =
        target.stoppedByMaxPackets || source.stoppedByMaxPackets;
    target.stoppedBySocketError =
        target.stoppedBySocketError || source.stoppedBySocketError;
    for (auto& delivery : source.deliveries) {
        target.deliveries.push_back(std::move(delivery));
    }
    for (auto& delivery : source.ackOnlyDeliveries) {
        target.ackOnlyDeliveries.push_back(std::move(delivery));
    }
}

Net::RudpSocketDrainSummary drainWithWaitForActivity(
    Net::UdpSocket& socket,
    Net::RudpPeerRegistry& registry,
    Util::TimePoint now,
    size_t maxPackets) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    Net::RudpSocketDrainSummary lastSummary;
    while (std::chrono::steady_clock::now() < deadline) {
        lastSummary = Net::drainRudpSocket(socket, registry, now, maxPackets);
        if (hasActivity(lastSummary)) {
            return lastSummary;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return lastSummary;
}

template <typename Predicate>
Net::RudpSocketDrainSummary drainUntil(
    Net::UdpSocket& socket,
    Net::RudpPeerRegistry& registry,
    Util::TimePoint now,
    size_t maxPackets,
    Predicate done) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    Net::RudpSocketDrainSummary total;
    while (std::chrono::steady_clock::now() < deadline) {
        Net::RudpSocketDrainSummary summary =
            Net::drainRudpSocket(socket, registry, now, maxPackets);
        mergeSummary(total, std::move(summary));
        if (done(total) || total.stoppedBySocketError) {
            return total;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return total;
}
}  // namespace

TEST(RudpSocketDrainTests, MaxPacketsZeroDoesNotReadSocket) {
    Net::UdpSocket socket;
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));

    const Net::RudpSocketDrainSummary summary =
        Net::drainRudpSocket(socket, registry, timeAt(0), 0);

    EXPECT_EQ(summary.attempted, 0U);
    EXPECT_EQ(summary.delivered, 0U);
    EXPECT_TRUE(summary.deliveries.empty());
    EXPECT_FALSE(summary.stoppedByWouldBlock);
    EXPECT_FALSE(summary.stoppedByMaxPackets);
    EXPECT_FALSE(summary.stoppedBySocketError);
    EXPECT_EQ(registry.size(), 0U);
}

TEST(RudpSocketDrainTests, EmptySocketStopsOnWouldBlock) {
    Net::UdpSocket socket;
    ASSERT_TRUE(socket.open(0));
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));

    const Net::RudpSocketDrainSummary summary =
        Net::drainRudpSocket(socket, registry, timeAt(0), 8);

    EXPECT_EQ(summary.attempted, 1U);
    EXPECT_EQ(summary.delivered, 0U);
    EXPECT_TRUE(summary.deliveries.empty());
    EXPECT_TRUE(summary.stoppedByWouldBlock);
    EXPECT_FALSE(summary.stoppedByMaxPackets);
    EXPECT_FALSE(summary.stoppedBySocketError);
    EXPECT_EQ(registry.size(), 0U);
}

TEST(RudpSocketDrainTests, ValidReliablePacketIsCollectedAsDelivery) {
    Net::UdpSocket receiver;
    Net::UdpSocket sender;
    ASSERT_TRUE(receiver.open(0));
    ASSERT_TRUE(sender.open(0));
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));

    const std::vector<uint8_t> payload{0x01, 0x02};
    Net::RudpPacketHeader header = reliableHeader(10);
    header.payloadLen = static_cast<uint16_t>(payload.size());
    sendPacket(sender, serializePacket(header, payload), receiver.boundPort());

    const Net::RudpSocketDrainSummary summary =
        drainWithWaitForActivity(receiver, registry, timeAt(0), 1);

    ASSERT_EQ(summary.delivered, 1U);
    ASSERT_EQ(summary.deliveries.size(), 1U);
    EXPECT_EQ(summary.deliveries[0].header.sequence, 10U);
    EXPECT_EQ(summary.deliveries[0].payload, payload);
    EXPECT_NE(summary.deliveries[0].peer, nullptr);
    EXPECT_EQ(summary.attempted, 1U);
    EXPECT_TRUE(summary.stoppedByMaxPackets);
    EXPECT_FALSE(summary.stoppedByWouldBlock);
    EXPECT_EQ(registry.size(), 1U);
}

TEST(RudpSocketDrainTests, NonDeliveryDatagramsAreCountedWithoutDelivery) {
    Net::UdpSocket receiver;
    Net::UdpSocket sender;
    ASSERT_TRUE(receiver.open(0));
    ASSERT_TRUE(sender.open(0));
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));

    Net::RudpPeer* peer =
        registry.findOrCreate(loopbackEndpoint(sender.boundPort()), timeAt(0));
    ASSERT_NE(peer, nullptr);
    ASSERT_TRUE(peer->reliableSendQueue().track(50, {0x01}));

    sendPacket(
        sender,
        serializePacket(reliableHeader(10), {0x01}),
        receiver.boundPort());

    std::vector<uint8_t> malformed =
        serializePacket(reliableHeader(20), {0x02});
    malformed[0] = 0x00;
    sendPacket(sender, malformed, receiver.boundPort());

    sendPacket(
        sender,
        serializePacket(reliableHeader(10), {0x01}),
        receiver.boundPort());
    sendPacket(
        sender,
        serializePacket(ackOnlyHeader(50), {}),
        receiver.boundPort());
    sendPacket(
        sender,
        serializePacket(reliableHeader(11), {0x03}),
        receiver.boundPort());

    const Net::RudpSocketDrainSummary summary = drainUntil(
        receiver,
        registry,
        timeAt(1),
        10,
        [](const Net::RudpSocketDrainSummary& total) {
            return total.delivered >= 2 &&
                total.malformed >= 1 &&
                total.duplicate >= 1 &&
                total.ackOnly >= 1;
        });

    EXPECT_EQ(summary.delivered, 2U);
    EXPECT_EQ(summary.deliveries.size(), 2U);
    EXPECT_EQ(summary.malformed, 1U);
    EXPECT_EQ(summary.duplicate, 1U);
    EXPECT_EQ(summary.ackOnly, 1U);
    ASSERT_EQ(summary.ackOnlyDeliveries.size(), 1U);
    EXPECT_EQ(summary.ackOnlyDeliveries[0].header.ack, 50U);
    EXPECT_TRUE(summary.ackOnlyDeliveries[0].payload.empty());
    EXPECT_FALSE(peer->reliableSendQueue().contains(50));
    EXPECT_EQ(registry.size(), 1U);
}

TEST(RudpSocketDrainTests, MaxPacketsCapLeavesDatagramForNextDrain) {
    Net::UdpSocket receiver;
    Net::UdpSocket sender;
    ASSERT_TRUE(receiver.open(0));
    ASSERT_TRUE(sender.open(0));
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));

    sendPacket(
        sender,
        serializePacket(reliableHeader(100), {0x01}),
        receiver.boundPort());
    sendPacket(
        sender,
        serializePacket(reliableHeader(101), {0x02}),
        receiver.boundPort());

    const Net::RudpSocketDrainSummary first =
        drainWithWaitForActivity(receiver, registry, timeAt(0), 1);
    const Net::RudpSocketDrainSummary second =
        drainWithWaitForActivity(receiver, registry, timeAt(1), 1);

    ASSERT_EQ(first.delivered, 1U);
    ASSERT_EQ(second.delivered, 1U);
    EXPECT_TRUE(first.stoppedByMaxPackets);
    EXPECT_TRUE(second.stoppedByMaxPackets);

    std::vector<uint32_t> sequences{
        first.deliveries[0].header.sequence,
        second.deliveries[0].header.sequence,
    };
    std::sort(sequences.begin(), sequences.end());
    EXPECT_EQ(sequences, (std::vector<uint32_t>{100, 101}));
    EXPECT_EQ(registry.size(), 1U);
}

TEST(RudpSocketDrainTests, ClosedSocketStopsOnSocketError) {
    Net::UdpSocket socket;
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));

    const Net::RudpSocketDrainSummary summary =
        Net::drainRudpSocket(socket, registry, timeAt(0), 8);

    EXPECT_EQ(summary.attempted, 1U);
    EXPECT_EQ(summary.socketErrors, 1U);
    EXPECT_TRUE(summary.stoppedBySocketError);
    EXPECT_FALSE(summary.stoppedByWouldBlock);
    EXPECT_FALSE(summary.stoppedByMaxPackets);
    EXPECT_TRUE(summary.deliveries.empty());
    EXPECT_EQ(registry.size(), 0U);
}
