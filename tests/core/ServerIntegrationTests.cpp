#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

#include "Core/Server.hpp"
#include "Net/TcpPacket.hpp"

namespace {
int connectToServer(uint16_t port) {
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

bool recvAll(int fd, uint8_t* buffer, size_t size) {
    size_t receivedTotal = 0;
    while (receivedTotal < size) {
        ssize_t received = ::recv(fd, buffer + receivedTotal, size - receivedTotal, 0);
        if (received <= 0) {
            return false;
        }
        receivedTotal += static_cast<size_t>(received);
    }
    return true;
}

bool recvWelcomePacket(int fd, uint64_t& outSessionId) {
    std::array<uint8_t, Net::kWelcomePacketSize> packet{};
    if (!recvAll(fd, packet.data(), packet.size())) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseWelcomePacket(packet.data(), packet.size(), header, outSessionId);
}

bool recvPacket(int fd, std::vector<uint8_t>& outPacket) {
    std::array<uint8_t, Net::kTcpHeaderSize> headerBytes{};
    if (!recvAll(fd, headerBytes.data(), headerBytes.size())) {
        return false;
    }

    Net::TcpPacketHeader header;
    if (!Net::peekTcpPacketHeader(headerBytes.data(), headerBytes.size(), header)) {
        return false;
    }

    outPacket.assign(header.size, 0);
    std::copy(headerBytes.begin(), headerBytes.end(), outPacket.begin());
    if (header.size == Net::kTcpHeaderSize) {
        return true;
    }

    return recvAll(
        fd,
        outPacket.data() + Net::kTcpHeaderSize,
        header.size - Net::kTcpHeaderSize);
}

bool recvClientListSnapshotPacket(int fd, std::vector<uint64_t>& outSessionIds) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseClientListSnapshotPacket(packet.data(), packet.size(), header, outSessionIds);
}

bool waitUntil(
    const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate(); // while을 빠져나왔다는 것은 deadline에 거의 도달한 것인데 정확히 마지막 순간에 true가 됐을 수도 있으니 마지막으로 한 번 더 체크.
}

bool expectConnectionAlive(int fd) {
    uint8_t byte = 0;
    const ssize_t result = ::recv(fd, &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
    if (result == 0) {
        return false;
    }
    if (result < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
    return true;
}

bool setReceiveTimeout(int fd, std::chrono::milliseconds timeout) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(timeout - seconds);

    timeval value{};
    value.tv_sec = static_cast<decltype(value.tv_sec)>(seconds.count());
    value.tv_usec = static_cast<decltype(value.tv_usec)>(micros.count());
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) == 0;
}

void expectSnapshotEquals(
    const std::vector<uint64_t>& actual,
    const std::vector<uint64_t>& expected) {
    EXPECT_EQ(actual, expected);
}

struct RunningServer {
    explicit RunningServer(uint16_t port) : server(port) {}

    ~RunningServer() {
        server.requestStop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    Core::Server server;
    std::thread thread;
};
}  // namespace

TEST(ServerIntegrationTests, SendsWelcomePacketOnConnect) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    int clientFd = connectToServer(port);
    ASSERT_GE(clientFd, 0);
    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(500)));

    uint64_t sessionId = 0;
    ASSERT_TRUE(recvWelcomePacket(clientFd, sessionId));
    EXPECT_GT(sessionId, 0u);

    std::vector<uint64_t> sessionIds;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientFd, sessionIds));
    expectSnapshotEquals(sessionIds, {sessionId});

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 1u &&
                   runningServer.server.sessionCount() == 1u;
        }));
    EXPECT_TRUE(expectConnectionAlive(clientFd));

    ::close(clientFd);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerIntegrationTests, AssignsUniqueSessionIdsToMultipleClients) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::vector<uint64_t> sessionIds(5, 0);
    std::vector<int> clientFds(sessionIds.size(), -1);

    for (size_t i = 0; i < sessionIds.size(); ++i) {
        clientFds[i] = connectToServer(port);
        ASSERT_GE(clientFds[i], 0);
        ASSERT_TRUE(setReceiveTimeout(clientFds[i], std::chrono::milliseconds(500)));
        ASSERT_TRUE(recvWelcomePacket(clientFds[i], sessionIds[i]));

        std::vector<uint64_t> snapshot;
        ASSERT_TRUE(recvClientListSnapshotPacket(clientFds[i], snapshot));
        EXPECT_EQ(snapshot.size(), i + 1);
    }

    EXPECT_TRUE(waitUntil(
        [&runningServer, expected = sessionIds.size()]() {
            return runningServer.server.activeConnectionCount() == expected &&
                   runningServer.server.sessionCount() == expected;
        }));

    for (int clientFd : clientFds) {
        EXPECT_TRUE(expectConnectionAlive(clientFd));
    }

    std::vector<uint64_t> sorted = sessionIds;
    std::sort(sorted.begin(), sorted.end());
    auto duplicateIt = std::adjacent_find(sorted.begin(), sorted.end());

    EXPECT_EQ(duplicateIt, sorted.end());
    EXPECT_GT(sorted.front(), 0u);

    for (int clientFd : clientFds) {
        ::close(clientFd);
    }

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerIntegrationTests, SynchronizesClientListSnapshotOnJoinAndLeave) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    int clientA = connectToServer(port);
    ASSERT_GE(clientA, 0);
    ASSERT_TRUE(setReceiveTimeout(clientA, std::chrono::milliseconds(500)));
    uint64_t sessionA = 0;
    ASSERT_TRUE(recvWelcomePacket(clientA, sessionA));
    std::vector<uint64_t> snapshotA;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientA, snapshotA));
    expectSnapshotEquals(snapshotA, {sessionA});

    int clientB = connectToServer(port);
    ASSERT_GE(clientB, 0);
    ASSERT_TRUE(setReceiveTimeout(clientB, std::chrono::milliseconds(500)));
    uint64_t sessionB = 0;
    ASSERT_TRUE(recvWelcomePacket(clientB, sessionB));
    std::vector<uint64_t> snapshotB;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientB, snapshotB));
    expectSnapshotEquals(snapshotB, {sessionA, sessionB});

    ASSERT_TRUE(recvClientListSnapshotPacket(clientA, snapshotA));
    expectSnapshotEquals(snapshotA, {sessionA, sessionB});

    int clientC = connectToServer(port);
    ASSERT_GE(clientC, 0);
    ASSERT_TRUE(setReceiveTimeout(clientC, std::chrono::milliseconds(500)));
    uint64_t sessionC = 0;
    ASSERT_TRUE(recvWelcomePacket(clientC, sessionC));
    std::vector<uint64_t> snapshotC;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientC, snapshotC));
    expectSnapshotEquals(snapshotC, {sessionA, sessionB, sessionC});

    ASSERT_TRUE(recvClientListSnapshotPacket(clientA, snapshotA));
    expectSnapshotEquals(snapshotA, {sessionA, sessionB, sessionC});

    ASSERT_TRUE(recvClientListSnapshotPacket(clientB, snapshotB));
    expectSnapshotEquals(snapshotB, {sessionA, sessionB, sessionC});

    ::close(clientC);

    ASSERT_TRUE(recvClientListSnapshotPacket(clientA, snapshotA));
    expectSnapshotEquals(snapshotA, {sessionA, sessionB});

    ASSERT_TRUE(recvClientListSnapshotPacket(clientB, snapshotB));
    expectSnapshotEquals(snapshotB, {sessionA, sessionB});

    ::close(clientB);

    ASSERT_TRUE(recvClientListSnapshotPacket(clientA, snapshotA));
    expectSnapshotEquals(snapshotA, {sessionA});

    ::close(clientA);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}
