#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cstdint>

#include "Net/RudpPeerRegistry.hpp"
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

Net::UdpEndpoint ipv4Endpoint(const char* address, uint16_t port) {
    Net::UdpEndpoint endpoint;
    auto* addr = reinterpret_cast<sockaddr_in*>(&endpoint.addr);
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, address, &addr->sin_addr);
    addr->sin_port = htons(port);
    endpoint.addrLen = sizeof(sockaddr_in);
    return endpoint;
}
}  // namespace

TEST(RudpPeerRegistryTests, SameEndpointReusesPeer) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));
    const Net::UdpEndpoint endpoint = ipv6LoopbackEndpoint(30000);

    Net::RudpPeer* first = registry.findOrCreate(endpoint, timeAt(0));
    Net::RudpPeer* second = registry.findOrCreate(endpoint, timeAt(1));

    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first, second);
    EXPECT_EQ(registry.size(), 1U);
}

TEST(RudpPeerRegistryTests, DifferentPortOrAddressCreatesDifferentPeers) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));

    Net::RudpPeer* first =
        registry.findOrCreate(ipv6LoopbackEndpoint(30000), timeAt(0));
    Net::RudpPeer* differentPort =
        registry.findOrCreate(ipv6LoopbackEndpoint(30001), timeAt(0));
    Net::RudpPeer* differentAddress =
        registry.findOrCreate(ipv4Endpoint("127.0.0.1", 30000), timeAt(0));

    ASSERT_NE(first, nullptr);
    EXPECT_NE(first, differentPort);
    EXPECT_NE(first, differentAddress);
    EXPECT_EQ(registry.size(), 3U);
}

TEST(RudpPeerRegistryTests, FindMissingEndpointDoesNotCreatePeer) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));

    EXPECT_EQ(registry.find(ipv6LoopbackEndpoint(30000)), nullptr);
    EXPECT_EQ(registry.size(), 0U);
}

TEST(RudpPeerRegistryTests, RemoveErasesOnlyMatchingEndpoint) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));
    const Net::UdpEndpoint firstEndpoint = ipv6LoopbackEndpoint(30000);
    const Net::UdpEndpoint secondEndpoint = ipv6LoopbackEndpoint(30001);
    ASSERT_NE(registry.findOrCreate(firstEndpoint, timeAt(0)), nullptr);
    ASSERT_NE(registry.findOrCreate(secondEndpoint, timeAt(0)), nullptr);

    EXPECT_TRUE(registry.remove(firstEndpoint));
    EXPECT_EQ(registry.find(firstEndpoint), nullptr);
    EXPECT_NE(registry.find(secondEndpoint), nullptr);
    EXPECT_EQ(registry.size(), 1U);
}

TEST(RudpPeerRegistryTests, TouchRefreshesPeerLifetime) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(10));
    const Net::UdpEndpoint endpoint = ipv6LoopbackEndpoint(30000);
    ASSERT_NE(registry.findOrCreate(endpoint, timeAt(0)), nullptr);

    EXPECT_TRUE(registry.touch(endpoint, timeAt(8)));
    registry.tick(timeAt(18));
    EXPECT_NE(registry.find(endpoint), nullptr);

    registry.tick(timeAt(19));
    EXPECT_EQ(registry.find(endpoint), nullptr);
}

TEST(RudpPeerRegistryTests, TickRemovesOnlyExpiredPeers) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(10));
    const Net::UdpEndpoint oldEndpoint = ipv6LoopbackEndpoint(30000);
    const Net::UdpEndpoint freshEndpoint = ipv6LoopbackEndpoint(30001);
    ASSERT_NE(registry.findOrCreate(oldEndpoint, timeAt(0)), nullptr);
    ASSERT_NE(registry.findOrCreate(freshEndpoint, timeAt(8)), nullptr);

    registry.tick(timeAt(11));

    EXPECT_EQ(registry.find(oldEndpoint), nullptr);
    EXPECT_NE(registry.find(freshEndpoint), nullptr);
    EXPECT_EQ(registry.size(), 1U);
}

TEST(RudpPeerRegistryTests, InvalidEndpointDoesNotCreatePeer) {
    Net::RudpPeerRegistry registry(std::chrono::milliseconds(100));
    Net::UdpEndpoint endpoint;

    EXPECT_EQ(registry.findOrCreate(endpoint, timeAt(0)), nullptr);
    EXPECT_EQ(registry.find(endpoint), nullptr);
    EXPECT_FALSE(registry.touch(endpoint, timeAt(1)));
    EXPECT_FALSE(registry.remove(endpoint));
    EXPECT_EQ(registry.size(), 0U);
}
