#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

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

TEST(UdpSocketTests, OpenBindsEphemeralPortAndEmptyReceiveWouldBlock) {
    Net::UdpSocket socket;

    ASSERT_TRUE(socket.open(0));
    EXPECT_GE(socket.fd(), 0);
    EXPECT_GT(socket.boundPort(), 0);

    std::array<uint8_t, 16> buffer{};
    Net::UdpEndpoint endpoint;
    EXPECT_EQ(socket.receiveFrom(buffer.data(), buffer.size(), endpoint), 0);

    socket.close();
    EXPECT_EQ(socket.fd(), -1);
    EXPECT_EQ(socket.boundPort(), 0);
}

TEST(UdpSocketTests, SendsAndReceivesDatagramOverLoopback) {
    Net::UdpSocket receiver;
    Net::UdpSocket sender;
    ASSERT_TRUE(receiver.open(0));
    ASSERT_TRUE(sender.open(0));

    const std::vector<uint8_t> payload{0x4C, 0x4F, 0x01, 0x02, 0x03};
    const Net::UdpEndpoint receiverEndpoint =
        loopbackEndpoint(receiver.boundPort());
    ASSERT_TRUE(sender.sendTo(payload.data(), payload.size(), receiverEndpoint));

    std::array<uint8_t, 64> buffer{};
    Net::UdpEndpoint senderEndpoint;
    const ssize_t received = receiveWithWait(
        receiver,
        buffer.data(),
        buffer.size(),
        senderEndpoint);

    ASSERT_EQ(received, static_cast<ssize_t>(payload.size()));
    EXPECT_EQ(
        std::vector<uint8_t>(buffer.begin(), buffer.begin() + received),
        payload);

    const std::string endpointText = Net::endpointToString(senderEndpoint);
    EXPECT_NE(endpointText, "unknown");
    EXPECT_FALSE(endpointText.empty());
}

TEST(UdpSocketTests, ClosedSocketRejectsSendAndReceive) {
    Net::UdpSocket socket;
    const std::vector<uint8_t> payload{0x01};
    const Net::UdpEndpoint endpoint = loopbackEndpoint(9);
    std::array<uint8_t, 16> buffer{};
    Net::UdpEndpoint receivedEndpoint;

    EXPECT_FALSE(socket.sendTo(payload.data(), payload.size(), endpoint));
    EXPECT_EQ(
        socket.receiveFrom(buffer.data(), buffer.size(), receivedEndpoint),
        -1);
}
