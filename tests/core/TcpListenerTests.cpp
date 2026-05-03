#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <thread>

#include "Net/TcpListener.hpp"

namespace {
int connectToLocalhost(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }

    return fd;
}
}  // namespace

TEST(TcpListenerTests, OpenProvidesBoundPortAndWouldBlockOnEmptyQueue) {
    Net::TcpListener listener;
    ASSERT_TRUE(listener.open(0));

    EXPECT_GT(listener.boundPort(), 0);

    int clientFd = -1;
    Net::TcpEndpoint endpoint;
    EXPECT_EQ(listener.acceptClient(clientFd, endpoint), Net::AcceptStatus::kWouldBlock);

    listener.close();
}

TEST(TcpListenerTests, AcceptsConnectedClient) {
    Net::TcpListener listener;
    ASSERT_TRUE(listener.open(0));

    int outboundFd = connectToLocalhost(listener.boundPort());
    ASSERT_GE(outboundFd, 0);

    int acceptedFd = -1;
    Net::TcpEndpoint endpoint;
    Net::AcceptStatus status = Net::AcceptStatus::kWouldBlock;
    for (int attempt = 0; attempt < 50; ++attempt) {
        status = listener.acceptClient(acceptedFd, endpoint);
        if (status == Net::AcceptStatus::kAccepted) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    ASSERT_EQ(status, Net::AcceptStatus::kAccepted);
    ASSERT_GE(acceptedFd, 0);
    EXPECT_FALSE(Net::endpointToString(endpoint).empty());

    listener.closeClient(acceptedFd);
    ::close(outboundFd);
    listener.close();
}
