#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "Net/RudpRetransmissionFlush.hpp"
#include "Net/UdpSocket.hpp"
#include "Util/Time.hpp"

namespace {
Util::TimePoint timeAt(int64_t milliseconds) {
    return Util::TimePoint{std::chrono::milliseconds(milliseconds)};
}

Net::UdpEndpoint ipv6LoopbackEndpoint(uint16_t port) {
    Net::UdpEndpoint endpoint;
    auto* addr = reinterpret_cast<sockaddr_in6*>(&endpoint.addr);
    addr->sin6_family = AF_INET6;
    addr->sin6_addr = in6addr_loopback;
    addr->sin6_port = htons(port);
    endpoint.addrLen = sizeof(sockaddr_in6);
    return endpoint;
}

std::vector<uint8_t> bytes(uint8_t marker) {
    return {0x4C, 0x4F, marker};
}

ssize_t receiveWithWait(
    Net::UdpSocket& socket,
    uint8_t* buffer,
    size_t bufferSize,
    Net::UdpEndpoint& endpoint) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        const ssize_t received = socket.receiveFrom(buffer, bufferSize, endpoint);
        if (received != 0) {
            return received;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return 0;
}
}  // namespace

TEST(RudpRetransmissionFlushTests, DuePacketIsResentAndMarked) {
    Net::UdpSocket sender;
    Net::UdpSocket receiver;
    ASSERT_TRUE(sender.open(0));
    ASSERT_TRUE(receiver.open(0));

    Net::RudpPeerRegistry registry(std::chrono::milliseconds(1000));
    const Net::UdpEndpoint receiverEndpoint =
        ipv6LoopbackEndpoint(receiver.boundPort());
    Net::RudpPeer* peer = registry.findOrCreate(receiverEndpoint, timeAt(0));
    ASSERT_NE(peer, nullptr);
    const std::vector<uint8_t> packet = bytes(0x10);
    ASSERT_TRUE(peer->reliableSendQueue().track(10, packet, timeAt(1000)));

    const Net::RudpRetransmissionFlushSummary summary =
        Net::flushRudpRetransmissions(sender, registry, timeAt(1200));

    EXPECT_EQ(summary.expired, 0U);
    EXPECT_EQ(summary.due, 1U);
    EXPECT_EQ(summary.resent, 1U);
    EXPECT_EQ(summary.sendErrors, 0U);
    EXPECT_EQ(summary.droppedPeers, 0U);
    EXPECT_EQ(peer->reliableSendQueue().retransmissionCount(10), 1U);
    EXPECT_EQ(registry.size(), 1U);

    std::array<uint8_t, 16> buffer{};
    Net::UdpEndpoint receivedEndpoint;
    const ssize_t received = receiveWithWait(
        receiver,
        buffer.data(),
        buffer.size(),
        receivedEndpoint);
    ASSERT_EQ(received, static_cast<ssize_t>(packet.size()));
    EXPECT_EQ(
        std::vector<uint8_t>(buffer.begin(), buffer.begin() + received),
        packet);
}

TEST(RudpRetransmissionFlushTests, ExpiredPacketIsCountedNotSentAndDropsPeer) {
    Net::UdpSocket sender;
    Net::UdpSocket receiver;
    ASSERT_TRUE(sender.open(0));
    ASSERT_TRUE(receiver.open(0));

    Net::RudpPeerRegistry registry(std::chrono::milliseconds(1000));
    Net::RudpPeer* peer =
        registry.findOrCreate(ipv6LoopbackEndpoint(receiver.boundPort()), timeAt(0));
    ASSERT_NE(peer, nullptr);

    Net::RudpReliableSendQueue& queue = peer->reliableSendQueue();
    auto lastSentAt = timeAt(1000);
    ASSERT_TRUE(queue.track(20, bytes(0x20), lastSentAt));
    for (uint32_t retry = 0;
         retry < Net::RudpReliableSendQueue::kDefaultMaxRetransmissions;
         ++retry) {
        const auto dueAt =
            lastSentAt + Net::RudpReliableSendQueue::kDefaultRetransmissionTimeout;
        ASSERT_TRUE(queue.markRetransmitted(20, dueAt));
        lastSentAt = dueAt;
    }

    const Net::RudpRetransmissionFlushSummary summary =
        Net::flushRudpRetransmissions(
            sender,
            registry,
            lastSentAt + Net::RudpReliableSendQueue::kDefaultRetransmissionTimeout);

    EXPECT_EQ(summary.expired, 1U);
    EXPECT_EQ(summary.due, 0U);
    EXPECT_EQ(summary.resent, 0U);
    EXPECT_EQ(summary.sendErrors, 0U);
    EXPECT_EQ(summary.droppedPeers, 1U);
    EXPECT_EQ(registry.size(), 0U);

    std::array<uint8_t, 16> buffer{};
    Net::UdpEndpoint receivedEndpoint;
    EXPECT_EQ(receiver.receiveFrom(buffer.data(), buffer.size(), receivedEndpoint), 0);
}

TEST(RudpRetransmissionFlushTests, MultipleExpiredSequencesDropSamePeerOnce) {
    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));

    Net::RudpPeerRegistry registry(std::chrono::milliseconds(1000));
    Net::RudpPeer* peer =
        registry.findOrCreate(ipv6LoopbackEndpoint(30000), timeAt(0));
    ASSERT_NE(peer, nullptr);

    Net::RudpReliableSendQueue& queue = peer->reliableSendQueue();
    auto lastSentAt = timeAt(1000);
    ASSERT_TRUE(queue.track(21, bytes(0x21), lastSentAt));
    ASSERT_TRUE(queue.track(22, bytes(0x22), lastSentAt));
    for (uint32_t retry = 0;
         retry < Net::RudpReliableSendQueue::kDefaultMaxRetransmissions;
         ++retry) {
        const auto dueAt =
            lastSentAt + Net::RudpReliableSendQueue::kDefaultRetransmissionTimeout;
        ASSERT_TRUE(queue.markRetransmitted(21, dueAt));
        ASSERT_TRUE(queue.markRetransmitted(22, dueAt));
        lastSentAt = dueAt;
    }

    const Net::RudpRetransmissionFlushSummary summary =
        Net::flushRudpRetransmissions(
            sender,
            registry,
            lastSentAt + Net::RudpReliableSendQueue::kDefaultRetransmissionTimeout);

    EXPECT_EQ(summary.expired, 2U);
    EXPECT_EQ(summary.due, 0U);
    EXPECT_EQ(summary.resent, 0U);
    EXPECT_EQ(summary.sendErrors, 0U);
    EXPECT_EQ(summary.droppedPeers, 1U);
    EXPECT_EQ(registry.size(), 0U);
}

TEST(RudpRetransmissionFlushTests, ClosedSocketCountsSendErrorAndLeavesPacketPending) {
    Net::UdpSocket closedSender;
    Net::UdpSocket receiver;
    ASSERT_TRUE(receiver.open(0));

    Net::RudpPeerRegistry registry(std::chrono::milliseconds(1000));
    Net::RudpPeer* peer =
        registry.findOrCreate(ipv6LoopbackEndpoint(receiver.boundPort()), timeAt(0));
    ASSERT_NE(peer, nullptr);
    ASSERT_TRUE(peer->reliableSendQueue().track(30, bytes(0x30), timeAt(1000)));

    const Net::RudpRetransmissionFlushSummary summary =
        Net::flushRudpRetransmissions(closedSender, registry, timeAt(1200));

    EXPECT_EQ(summary.expired, 0U);
    EXPECT_EQ(summary.due, 1U);
    EXPECT_EQ(summary.resent, 0U);
    EXPECT_EQ(summary.sendErrors, 1U);
    EXPECT_EQ(summary.droppedPeers, 0U);
    EXPECT_TRUE(peer->reliableSendQueue().contains(30));
    EXPECT_EQ(peer->reliableSendQueue().retransmissionCount(30), 0U);
    EXPECT_EQ(registry.size(), 1U);
}

TEST(RudpRetransmissionFlushTests, AckConsumedPacketIsExcluded) {
    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));

    Net::RudpPeerRegistry registry(std::chrono::milliseconds(1000));
    Net::RudpPeer* peer =
        registry.findOrCreate(ipv6LoopbackEndpoint(30000), timeAt(0));
    ASSERT_NE(peer, nullptr);
    ASSERT_TRUE(peer->reliableSendQueue().track(40, bytes(0x40), timeAt(1000)));
    EXPECT_EQ(peer->reliableSendQueue().consumeAck(40, 0), 1U);

    const Net::RudpRetransmissionFlushSummary summary =
        Net::flushRudpRetransmissions(sender, registry, timeAt(1200));

    EXPECT_EQ(summary.expired, 0U);
    EXPECT_EQ(summary.due, 0U);
    EXPECT_EQ(summary.resent, 0U);
    EXPECT_EQ(summary.sendErrors, 0U);
    EXPECT_EQ(summary.droppedPeers, 0U);
    EXPECT_EQ(registry.size(), 1U);
}

TEST(RudpRetransmissionFlushTests, StalePeerRemovedByRegistryTickIsNotFlushed) {
    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));

    Net::RudpPeerRegistry registry(std::chrono::milliseconds(10));
    Net::RudpPeer* peer =
        registry.findOrCreate(ipv6LoopbackEndpoint(30000), timeAt(0));
    ASSERT_NE(peer, nullptr);
    ASSERT_TRUE(peer->reliableSendQueue().track(50, bytes(0x50), timeAt(0)));

    registry.tick(timeAt(211));
    const Net::RudpRetransmissionFlushSummary summary =
        Net::flushRudpRetransmissions(sender, registry, timeAt(211));

    EXPECT_EQ(summary.expired, 0U);
    EXPECT_EQ(summary.due, 0U);
    EXPECT_EQ(summary.resent, 0U);
    EXPECT_EQ(summary.sendErrors, 0U);
    EXPECT_EQ(summary.droppedPeers, 0U);
    EXPECT_EQ(registry.size(), 0U);
}
