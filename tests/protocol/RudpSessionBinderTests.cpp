#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include "Net/RudpSessionBinder.hpp"

namespace {
Net::UdpEndpoint ipv6Endpoint(uint16_t port) {
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

TEST(RudpSessionBinderTests, BindsEndpointToSession) {
    Net::RudpSessionBinder binder;
    const Net::UdpEndpoint endpoint = ipv6Endpoint(30000);

    EXPECT_EQ(
        binder.bind(endpoint, 1001),
        Net::RudpSessionBindResult::kBoundNew);
    ASSERT_TRUE(binder.findSessionId(endpoint).has_value());
    EXPECT_EQ(*binder.findSessionId(endpoint), 1001U);
    EXPECT_EQ(binder.size(), 1U);
}

TEST(RudpSessionBinderTests, SameEndpointAndSessionRefreshes) {
    Net::RudpSessionBinder binder;
    const Net::UdpEndpoint endpoint = ipv6Endpoint(30000);
    ASSERT_EQ(
        binder.bind(endpoint, 1001),
        Net::RudpSessionBindResult::kBoundNew);

    EXPECT_EQ(
        binder.bind(endpoint, 1001),
        Net::RudpSessionBindResult::kRefreshed);
    EXPECT_EQ(binder.size(), 1U);
}

TEST(RudpSessionBinderTests, SameEndpointDifferentSessionIsConflict) {
    Net::RudpSessionBinder binder;
    const Net::UdpEndpoint endpoint = ipv6Endpoint(30000);
    ASSERT_EQ(
        binder.bind(endpoint, 1001),
        Net::RudpSessionBindResult::kBoundNew);

    EXPECT_EQ(
        binder.bind(endpoint, 1002),
        Net::RudpSessionBindResult::kEndpointConflict);
    ASSERT_TRUE(binder.findSessionId(endpoint).has_value());
    EXPECT_EQ(*binder.findSessionId(endpoint), 1001U);
    EXPECT_EQ(binder.size(), 1U);
}

TEST(RudpSessionBinderTests, SameSessionDifferentEndpointIsConflict) {
    Net::RudpSessionBinder binder;
    const Net::UdpEndpoint first = ipv6Endpoint(30000);
    const Net::UdpEndpoint second = ipv4Endpoint("127.0.0.1", 30001);
    ASSERT_EQ(
        binder.bind(first, 1001),
        Net::RudpSessionBindResult::kBoundNew);

    EXPECT_EQ(
        binder.bind(second, 1001),
        Net::RudpSessionBindResult::kSessionConflict);
    EXPECT_FALSE(binder.findSessionId(second).has_value());
    EXPECT_EQ(binder.size(), 1U);
}

TEST(RudpSessionBinderTests, RemoveByEndpointAndSession) {
    Net::RudpSessionBinder binder;
    const Net::UdpEndpoint first = ipv6Endpoint(30000);
    const Net::UdpEndpoint second = ipv6Endpoint(30001);
    ASSERT_EQ(
        binder.bind(first, 1001),
        Net::RudpSessionBindResult::kBoundNew);
    ASSERT_EQ(
        binder.bind(second, 1002),
        Net::RudpSessionBindResult::kBoundNew);

    EXPECT_TRUE(binder.removeByEndpoint(first));
    EXPECT_FALSE(binder.findSessionId(first).has_value());
    EXPECT_EQ(binder.size(), 1U);

    EXPECT_TRUE(binder.removeBySessionId(1002));
    EXPECT_FALSE(binder.findSessionId(second).has_value());
    EXPECT_EQ(binder.size(), 0U);
}

TEST(RudpSessionBinderTests, RejectsInvalidEndpointAndZeroSession) {
    Net::RudpSessionBinder binder;
    Net::UdpEndpoint invalid;
    const Net::UdpEndpoint valid = ipv6Endpoint(30000);

    EXPECT_EQ(
        binder.bind(invalid, 1001),
        Net::RudpSessionBindResult::kInvalidEndpoint);
    EXPECT_EQ(
        binder.bind(valid, 0),
        Net::RudpSessionBindResult::kInvalidEndpoint);
    EXPECT_FALSE(binder.removeByEndpoint(invalid));
    EXPECT_FALSE(binder.removeBySessionId(1001));
    EXPECT_EQ(binder.size(), 0U);
}
