#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "Net/RudpRetransmissionScan.hpp"
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

std::vector<std::string> dueKeys(const Net::RudpRetransmissionScanResult& result) {
    std::vector<std::string> keys;
    keys.reserve(result.due.size());
    for (const Net::RudpDueRetransmission& due : result.due) {
        keys.push_back(Net::endpointToString(due.endpoint) + "#" +
            std::to_string(due.sequence));
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}
}  // namespace

TEST(RudpRetransmissionScanTests, PendingPacketBeforeTimeoutIsNotDueOrExpired) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(1000));
    Net::RudpPeer* peer =
        registry.findOrCreate(ipv6LoopbackEndpoint(30000), timeAt(0));
    ASSERT_NE(peer, nullptr);
    ASSERT_TRUE(peer->reliableSendQueue().track(10, bytes(0x10), timeAt(1000)));

    const Net::RudpRetransmissionScanResult result =
        Net::scanRudpRetransmissions(registry, timeAt(1199));

    EXPECT_TRUE(result.due.empty());
    EXPECT_TRUE(result.expired.empty());
}

TEST(RudpRetransmissionScanTests, TimedOutPacketWithRetryBudgetIsDue) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(1000));
    const Net::UdpEndpoint endpoint = ipv6LoopbackEndpoint(30000);
    Net::RudpPeer* peer = registry.findOrCreate(endpoint, timeAt(0));
    ASSERT_NE(peer, nullptr);
    ASSERT_TRUE(peer->reliableSendQueue().track(10, bytes(0x10), timeAt(1000)));

    const Net::RudpRetransmissionScanResult result =
        Net::scanRudpRetransmissions(registry, timeAt(1200));

    EXPECT_TRUE(result.expired.empty());
    ASSERT_EQ(result.due.size(), 1U);
    EXPECT_EQ(Net::endpointToString(result.due[0].endpoint), Net::endpointToString(endpoint));
    EXPECT_EQ(result.due[0].sequence, 10U);
    ASSERT_NE(result.due[0].packetBytes, nullptr);
    EXPECT_EQ(*result.due[0].packetBytes, bytes(0x10));
}

TEST(RudpRetransmissionScanTests, MaxRetryPacketIsExpiredNotDue) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(1000));
    const Net::UdpEndpoint endpoint = ipv6LoopbackEndpoint(30000);
    Net::RudpPeer* peer = registry.findOrCreate(endpoint, timeAt(0));
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

    const Net::RudpRetransmissionScanResult result =
        Net::scanRudpRetransmissions(
            registry,
            lastSentAt + Net::RudpReliableSendQueue::kDefaultRetransmissionTimeout);

    EXPECT_TRUE(result.due.empty());
    ASSERT_EQ(result.expired.size(), 1U);
    EXPECT_EQ(
        Net::endpointToString(result.expired[0].endpoint),
        Net::endpointToString(endpoint));
    EXPECT_EQ(result.expired[0].sequence, 20U);
}

TEST(RudpRetransmissionScanTests, AckConsumedPacketIsExcluded) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(1000));
    Net::RudpPeer* peer =
        registry.findOrCreate(ipv6LoopbackEndpoint(30000), timeAt(0));
    ASSERT_NE(peer, nullptr);
    ASSERT_TRUE(peer->reliableSendQueue().track(30, bytes(0x30), timeAt(1000)));
    EXPECT_EQ(peer->reliableSendQueue().consumeAck(30, 0), 1U);

    const Net::RudpRetransmissionScanResult result =
        Net::scanRudpRetransmissions(registry, timeAt(1200));

    EXPECT_TRUE(result.due.empty());
    EXPECT_TRUE(result.expired.empty());
}

TEST(RudpRetransmissionScanTests, MultipleEndpointsRemainSeparated) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(1000));
    const Net::UdpEndpoint firstEndpoint = ipv6LoopbackEndpoint(30000);
    const Net::UdpEndpoint secondEndpoint = ipv6LoopbackEndpoint(30001);
    Net::RudpPeer* first = registry.findOrCreate(firstEndpoint, timeAt(0));
    Net::RudpPeer* second = registry.findOrCreate(secondEndpoint, timeAt(0));
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(first->reliableSendQueue().track(40, bytes(0x40), timeAt(1000)));
    ASSERT_TRUE(second->reliableSendQueue().track(41, bytes(0x41), timeAt(1000)));

    const Net::RudpRetransmissionScanResult result =
        Net::scanRudpRetransmissions(registry, timeAt(1200));

    EXPECT_TRUE(result.expired.empty());
    EXPECT_EQ(
        dueKeys(result),
        (std::vector<std::string>{
            Net::endpointToString(firstEndpoint) + "#40",
            Net::endpointToString(secondEndpoint) + "#41"}));
}

TEST(RudpRetransmissionScanTests, StalePeerRemovedByRegistryTickIsNotScanned) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(10));
    const Net::UdpEndpoint staleEndpoint = ipv6LoopbackEndpoint(30000);
    const Net::UdpEndpoint liveEndpoint = ipv6LoopbackEndpoint(30001);
    Net::RudpPeer* stale = registry.findOrCreate(staleEndpoint, timeAt(0));
    Net::RudpPeer* live = registry.findOrCreate(liveEndpoint, timeAt(205));
    ASSERT_NE(stale, nullptr);
    ASSERT_NE(live, nullptr);
    ASSERT_TRUE(stale->reliableSendQueue().track(50, bytes(0x50), timeAt(0)));
    ASSERT_TRUE(live->reliableSendQueue().track(51, bytes(0x51), timeAt(0)));

    registry.tick(timeAt(211));
    const Net::RudpRetransmissionScanResult result =
        Net::scanRudpRetransmissions(registry, timeAt(211));

    EXPECT_TRUE(result.expired.empty());
    EXPECT_EQ(
        dueKeys(result),
        (std::vector<std::string>{Net::endpointToString(liveEndpoint) + "#51"}));
}
