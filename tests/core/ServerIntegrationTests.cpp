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
#include "Net/RudpHelloPayload.hpp"
#include "Net/RudpInputCommandPayload.hpp"
#include "Net/RudpPacket.hpp"
#include "Net/RudpReliableSendQueue.hpp"
#include "Net/TcpPacket.hpp"
#include "Net/UdpSocket.hpp"
#include "Util/Time.hpp"

namespace Core {
struct ServerTestAccess {
    static bool trackRudpReliablePacket(
        Server& server,
        const Net::UdpEndpoint& endpoint,
        uint32_t sequence,
        const std::vector<uint8_t>& packetBytes,
        Util::TimePoint sentAt) {
        Net::RudpPeer* peer = server.rudpPeerRegistry_.findOrCreate(endpoint, sentAt);
        if (peer == nullptr) {
            return false;
        }
        const bool tracked =
            peer->reliableSendQueue().track(sequence, packetBytes, sentAt);
        server.rudpPeerCountSnapshot_.store(
            server.rudpPeerRegistry_.size(),
            std::memory_order_relaxed);
        return tracked;
    }

    static size_t rudpInputCommandSequenceTrackerSize(Server& server) {
        return server.rudpInputCommandSequenceTracker_.size();
    }
};
}  // namespace Core

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

Net::UdpEndpoint udpLoopbackEndpoint(uint16_t port) {
    Net::UdpEndpoint endpoint;
    auto* addr = reinterpret_cast<sockaddr_in6*>(&endpoint.addr);
    addr->sin6_family = AF_INET6;
    addr->sin6_addr = in6addr_loopback;
    addr->sin6_port = htons(port);
    endpoint.addrLen = sizeof(sockaddr_in6);
    return endpoint;
}

Net::RudpPacketHeader reliableRudpHeader(uint32_t sequence) {
    Net::RudpPacketHeader header;
    header.flags = Net::kRudpFlagReliable;
    header.channelId = static_cast<uint8_t>(Net::RudpChannelId::kEvent);
    header.packetType = static_cast<uint16_t>(Net::RudpPacketType::kGameEvent);
    header.sequence = sequence;
    header.payloadLen = 1;
    return header;
}

Net::RudpPacketHeader inputCommandRudpHeader(uint32_t sequence) {
    Net::RudpPacketHeader header;
    header.channelId = static_cast<uint8_t>(Net::RudpChannelId::kInput);
    header.packetType = static_cast<uint16_t>(Net::RudpPacketType::kInputCommand);
    header.sequence = sequence;
    return header;
}

std::vector<uint8_t> readyInputCommandPayloadForTest(
    uint32_t playerId,
    uint32_t cmdSeq) {
    std::vector<uint8_t> payload(Net::kRudpInputCommandPrefixSize, 0);
    payload[0] = static_cast<uint8_t>((playerId >> 24) & 0xFF);
    payload[1] = static_cast<uint8_t>((playerId >> 16) & 0xFF);
    payload[2] = static_cast<uint8_t>((playerId >> 8) & 0xFF);
    payload[3] = static_cast<uint8_t>(playerId & 0xFF);
    payload[4] = static_cast<uint8_t>((cmdSeq >> 24) & 0xFF);
    payload[5] = static_cast<uint8_t>((cmdSeq >> 16) & 0xFF);
    payload[6] = static_cast<uint8_t>((cmdSeq >> 8) & 0xFF);
    payload[7] = static_cast<uint8_t>(cmdSeq & 0xFF);
    payload[8] = static_cast<uint8_t>(Net::RudpInputCommandOp::kReady);
    payload[9] = 0;
    return payload;
}

Net::RudpPacketHeader rudpHeaderForType(
    uint32_t sequence,
    Net::RudpPacketType packetType,
    Net::RudpChannelId channelId,
    uint8_t flags) {
    Net::RudpPacketHeader header;
    header.flags = flags;
    header.channelId = static_cast<uint8_t>(channelId);
    header.packetType = static_cast<uint16_t>(packetType);
    header.sequence = sequence;
    header.payloadLen = 1;
    return header;
}

Net::RudpPacketHeader helloRudpHeader(uint32_t sequence) {
    Net::RudpPacketHeader header;
    header.flags = Net::kRudpFlagReliable;
    header.channelId = static_cast<uint8_t>(Net::RudpChannelId::kControl);
    header.packetType = static_cast<uint16_t>(Net::RudpPacketType::kHello);
    header.sequence = sequence;
    header.payloadLen = Net::kRudpHelloPayloadSize;
    return header;
}

Net::RudpPacketHeader ackOnlyRudpHeader(uint32_t ack) {
    Net::RudpPacketHeader header;
    header.flags = Net::kRudpFlagAckOnly;
    header.channelId = static_cast<uint8_t>(Net::RudpChannelId::kControl);
    header.packetType = static_cast<uint16_t>(Net::RudpPacketType::kHello);
    header.sequence = 30;
    header.ack = ack;
    return header;
}

std::vector<uint8_t> serializeRudpPacketForTest(
    const Net::RudpPacketHeader& header,
    const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> packet;
    EXPECT_TRUE(Net::serializeRudpPacket(header, payload, packet));
    return packet;
}

std::vector<uint8_t> serializeRudpHelloPacketForTest(
    uint32_t sequence,
    uint16_t clientVersion,
    uint32_t clientId,
    uint64_t sessionId) {
    std::vector<uint8_t> payload;
    EXPECT_TRUE(Net::serializeRudpHelloPayload(
        Net::RudpHelloPayload{clientVersion, clientId, sessionId},
        payload));
    return serializeRudpPacketForTest(helloRudpHeader(sequence), payload);
}

void sendUdpPacket(
    Net::UdpSocket& sender,
    const std::vector<uint8_t>& packet,
    uint16_t receiverPort) {
    ASSERT_TRUE(sender.sendTo(
        packet.data(),
        packet.size(),
        udpLoopbackEndpoint(receiverPort)));
}

void expectSnapshotEquals(
    const std::vector<uint64_t>& actual,
    const std::vector<uint64_t>& expected) {
    EXPECT_EQ(actual, expected);
}

struct RunningServer {
    explicit RunningServer(uint16_t port) : server(port) {}
    RunningServer(uint16_t port, std::chrono::milliseconds rudpPeerTimeout)
        : server(port, rudpPeerTimeout) {}

    ~RunningServer() {
        server.requestStop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    Core::Server server;
    std::thread thread;
};

bool bindRudpSessionForTest(
    RunningServer& runningServer,
    Net::UdpSocket& sender,
    int& outClientFd,
    uint64_t& outSessionId) {
    const uint16_t tcpPort = runningServer.server.boundPort();
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    if (tcpPort == 0 || udpPort == 0) {
        return false;
    }

    outClientFd = connectToServer(tcpPort);
    if (outClientFd < 0) {
        return false;
    }
    if (!setReceiveTimeout(outClientFd, std::chrono::milliseconds(500))) {
        return false;
    }
    if (!recvWelcomePacket(outClientFd, outSessionId)) {
        return false;
    }

    std::vector<uint64_t> snapshot;
    if (!recvClientListSnapshotPacket(outClientFd, snapshot)) {
        return false;
    }

    const std::vector<uint8_t> helloPacket =
        serializeRudpHelloPacketForTest(100, 1, 77, outSessionId);
    if (!sender.sendTo(
            helloPacket.data(),
            helloPacket.size(),
            udpLoopbackEndpoint(udpPort))) {
        return false;
    }

    return waitUntil(
        [&runningServer]() {
            return runningServer.server.rudpBindingCount() == 1U;
        });
}
}  // namespace

TEST(ServerIntegrationTests, StartsAndStopsUdpSocketLifecycle) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    EXPECT_GT(runningServer.server.boundPort(), 0);
    EXPECT_GT(runningServer.server.udpBoundPort(), 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    runningServer.server.requestStop();
    ASSERT_TRUE(runningServer.thread.joinable());
    runningServer.thread.join();

    EXPECT_EQ(runningServer.server.udpBoundPort(), 0);
}

TEST(ServerIntegrationTests, DrainsValidReliableRudpDatagramInServerLoop) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));

    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(reliableRudpHeader(10), {0x01}),
        udpPort);

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerDrainStats stats =
                runningServer.server.rudpDrainStats();
            const Core::RudpServerBindingStats bindingStats =
                runningServer.server.rudpBindingStats();
            return stats.delivered >= 1U && bindingStats.ignoredNonHello >= 1U &&
                bindingStats.unsupportedPacketIgnored >= 1U &&
                runningServer.server.rudpPeerCount() >= 1U &&
                runningServer.server.rudpBindingCount() == 0U;
        }));

    const Core::RudpServerRetransmissionStats retransmissionStats =
        runningServer.server.rudpRetransmissionStats();
    EXPECT_EQ(retransmissionStats.expired, 0U);
    EXPECT_EQ(retransmissionStats.due, 0U);
    EXPECT_EQ(retransmissionStats.resent, 0U);
    EXPECT_EQ(retransmissionStats.sendErrors, 0U);
    EXPECT_EQ(retransmissionStats.droppedPeers, 0U);
}

TEST(ServerIntegrationTests, DropsRudpPeerAfterExpiredRetransmission) {
    RunningServer runningServer(0, std::chrono::milliseconds(3000));
    ASSERT_TRUE(runningServer.server.start());

    Net::UdpSocket receiver;
    ASSERT_TRUE(receiver.open(0));

    const std::vector<uint8_t> packet{0x4C, 0x4F, 0x90};
    ASSERT_TRUE(Core::ServerTestAccess::trackRudpReliablePacket(
        runningServer.server,
        udpLoopbackEndpoint(receiver.boundPort()),
        90,
        packet,
        Util::now() - Net::RudpReliableSendQueue::kDefaultRetransmissionTimeout));
    ASSERT_EQ(runningServer.server.rudpPeerCount(), 1U);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerRetransmissionStats stats =
                runningServer.server.rudpRetransmissionStats();
            return stats.expired >= 1U && stats.droppedPeers >= 1U &&
                runningServer.server.rudpPeerCount() == 0U;
        },
        std::chrono::milliseconds(2500)));
}

TEST(ServerIntegrationTests, RemovesIdleRudpPeerAfterTimeout) {
    RunningServer runningServer(0, std::chrono::milliseconds(50));
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));

    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(reliableRudpHeader(40), {0x04}),
        udpPort);

    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerDrainStats stats =
                runningServer.server.rudpDrainStats();
            return stats.delivered >= 1U && runningServer.server.rudpPeerCount() >= 1U;
        }));

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.rudpPeerCount() == 0U;
        },
        std::chrono::milliseconds(1000)));
}

TEST(ServerIntegrationTests, CountsMalformedRudpDatagramWithoutCreatingPeer) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));

    std::vector<uint8_t> malformed =
        serializeRudpPacketForTest(reliableRudpHeader(20), {0x02});
    ASSERT_FALSE(malformed.empty());
    malformed[0] = 0x00;
    sendUdpPacket(sender, malformed, udpPort);

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerDrainStats stats =
                runningServer.server.rudpDrainStats();
            return stats.malformed >= 1U;
        }));

    const Core::RudpServerDrainStats stats = runningServer.server.rudpDrainStats();
    EXPECT_EQ(stats.delivered, 0U);
    EXPECT_EQ(runningServer.server.rudpPeerCount(), 0U);
}

TEST(ServerIntegrationTests, CountsDuplicateReliableRudpDatagram) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));

    const std::vector<uint8_t> packet =
        serializeRudpPacketForTest(reliableRudpHeader(30), {0x03});
    sendUdpPacket(sender, packet, udpPort);
    sendUdpPacket(sender, packet, udpPort);

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerDrainStats stats =
                runningServer.server.rudpDrainStats();
            return stats.delivered >= 1U && stats.duplicate >= 1U;
        }));

    const Core::RudpServerDrainStats stats = runningServer.server.rudpDrainStats();
    EXPECT_EQ(stats.delivered, 1U);
    EXPECT_GE(runningServer.server.rudpPeerCount(), 1U);
}

TEST(ServerIntegrationTests, CountsAckOnlyRudpDatagramWithoutDelivery) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));

    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(ackOnlyRudpHeader(50), {}),
        udpPort);

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerDrainStats stats =
                runningServer.server.rudpDrainStats();
            return stats.ackOnly >= 1U;
        }));

    const Core::RudpServerDrainStats stats = runningServer.server.rudpDrainStats();
    EXPECT_EQ(stats.delivered, 0U);
}

TEST(ServerIntegrationTests, BindsRudpHelloToExistingTcpSession) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t tcpPort = runningServer.server.boundPort();
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(tcpPort, 0);
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    int clientFd = connectToServer(tcpPort);
    ASSERT_GE(clientFd, 0);
    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(500)));
    uint64_t sessionId = 0;
    ASSERT_TRUE(recvWelcomePacket(clientFd, sessionId));
    std::vector<uint64_t> snapshot;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientFd, snapshot));

    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));
    sendUdpPacket(
        sender,
        serializeRudpHelloPacketForTest(100, 1, 77, sessionId),
        udpPort);

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerBindingStats stats =
                runningServer.server.rudpBindingStats();
            return stats.helloReceived >= 1U && stats.bound >= 1U &&
                runningServer.server.rudpBindingCount() == 1U;
        }));

    ::close(clientFd);
}

TEST(ServerIntegrationTests, RefreshesDuplicateRudpHelloForSameEndpointAndSession) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t tcpPort = runningServer.server.boundPort();
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(tcpPort, 0);
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    int clientFd = connectToServer(tcpPort);
    ASSERT_GE(clientFd, 0);
    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(500)));
    uint64_t sessionId = 0;
    ASSERT_TRUE(recvWelcomePacket(clientFd, sessionId));
    std::vector<uint64_t> snapshot;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientFd, snapshot));

    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));
    sendUdpPacket(
        sender,
        serializeRudpHelloPacketForTest(100, 1, 77, sessionId),
        udpPort);
    sendUdpPacket(
        sender,
        serializeRudpHelloPacketForTest(101, 1, 77, sessionId),
        udpPort);

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerBindingStats stats =
                runningServer.server.rudpBindingStats();
            return stats.bound >= 1U && stats.refreshed >= 1U &&
                runningServer.server.rudpBindingCount() == 1U;
        }));

    ::close(clientFd);
}

TEST(ServerIntegrationTests, RejectsRudpHelloForUnknownSession) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));
    sendUdpPacket(
        sender,
        serializeRudpHelloPacketForTest(100, 1, 77, 9999),
        udpPort);

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerBindingStats stats =
                runningServer.server.rudpBindingStats();
            return stats.unknownSession >= 1U &&
                runningServer.server.rudpBindingCount() == 0U;
        }));
}

TEST(ServerIntegrationTests, RejectsRudpHelloEndpointSessionConflict) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t tcpPort = runningServer.server.boundPort();
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(tcpPort, 0);
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    int clientA = connectToServer(tcpPort);
    int clientB = connectToServer(tcpPort);
    ASSERT_GE(clientA, 0);
    ASSERT_GE(clientB, 0);
    ASSERT_TRUE(setReceiveTimeout(clientA, std::chrono::milliseconds(500)));
    ASSERT_TRUE(setReceiveTimeout(clientB, std::chrono::milliseconds(500)));
    uint64_t sessionA = 0;
    uint64_t sessionB = 0;
    ASSERT_TRUE(recvWelcomePacket(clientA, sessionA));
    std::vector<uint64_t> snapshotA;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientA, snapshotA));
    ASSERT_TRUE(recvWelcomePacket(clientB, sessionB));
    std::vector<uint64_t> snapshotB;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientB, snapshotB));

    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));
    sendUdpPacket(
        sender,
        serializeRudpHelloPacketForTest(100, 1, 77, sessionA),
        udpPort);
    sendUdpPacket(
        sender,
        serializeRudpHelloPacketForTest(101, 1, 77, sessionB),
        udpPort);

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerBindingStats stats =
                runningServer.server.rudpBindingStats();
            return stats.bound >= 1U && stats.conflicts >= 1U &&
                runningServer.server.rudpBindingCount() == 1U;
        }));

    ::close(clientA);
    ::close(clientB);
}

TEST(ServerIntegrationTests, RemovesRudpBindingWhenTcpSessionDisconnects) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t tcpPort = runningServer.server.boundPort();
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(tcpPort, 0);
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    int clientFd = connectToServer(tcpPort);
    ASSERT_GE(clientFd, 0);
    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(500)));
    uint64_t sessionId = 0;
    ASSERT_TRUE(recvWelcomePacket(clientFd, sessionId));
    std::vector<uint64_t> snapshot;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientFd, snapshot));

    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));
    sendUdpPacket(
        sender,
        serializeRudpHelloPacketForTest(100, 1, 77, sessionId),
        udpPort);

    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.rudpBindingCount() == 1U;
        }));

    ::close(clientFd);

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0U &&
                runningServer.server.sessionCount() == 0U &&
                runningServer.server.rudpBindingCount() == 0U;
        }));
}

TEST(ServerIntegrationTests, RejectsUnboundRudpInputCommandAtAdapterGate) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));
    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(inputCommandRudpHeader(100), {0x01}),
        udpPort);

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerBindingStats stats =
                runningServer.server.rudpBindingStats();
            return stats.unboundInputRejected >= 1U &&
                stats.inputCandidates == 0U &&
                stats.inputDecoded == 0U &&
                stats.inputDecodeFailed == 0U &&
                stats.inputSequenceAccepted == 0U &&
                stats.inputSequenceDuplicateRejected == 0U &&
                stats.inputSequenceStaleRejected == 0U &&
                stats.inputSequenceAmbiguousRejected == 0U &&
                runningServer.server.rudpBindingCount() == 0U;
        }));
}

TEST(ServerIntegrationTests, DecodesBoundRudpInputCommandAtAdapterGate) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t tcpPort = runningServer.server.boundPort();
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(tcpPort, 0);
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    int clientFd = connectToServer(tcpPort);
    ASSERT_GE(clientFd, 0);
    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(500)));
    uint64_t sessionId = 0;
    ASSERT_TRUE(recvWelcomePacket(clientFd, sessionId));
    std::vector<uint64_t> snapshot;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientFd, snapshot));

    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));
    sendUdpPacket(
        sender,
        serializeRudpHelloPacketForTest(100, 1, 77, sessionId),
        udpPort);

    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.rudpBindingCount() == 1U;
        }));

    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(
            inputCommandRudpHeader(101),
            readyInputCommandPayloadForTest(77, 1)),
        udpPort);

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerBindingStats stats =
                runningServer.server.rudpBindingStats();
            return stats.inputCandidates >= 1U &&
                stats.inputDecoded >= 1U &&
                stats.inputSequenceAccepted >= 1U &&
                stats.inputDecodeFailed == 0U &&
                stats.unboundInputRejected == 0U &&
                runningServer.server.rudpBindingCount() == 1U;
        }));

    ::close(clientFd);
}

TEST(ServerIntegrationTests, RejectsMalformedBoundRudpInputCommandAtAdapterGate) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t tcpPort = runningServer.server.boundPort();
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(tcpPort, 0);
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    int clientFd = connectToServer(tcpPort);
    ASSERT_GE(clientFd, 0);
    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(500)));
    uint64_t sessionId = 0;
    ASSERT_TRUE(recvWelcomePacket(clientFd, sessionId));
    std::vector<uint64_t> snapshot;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientFd, snapshot));

    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));
    sendUdpPacket(
        sender,
        serializeRudpHelloPacketForTest(100, 1, 77, sessionId),
        udpPort);

    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.rudpBindingCount() == 1U;
        }));

    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(inputCommandRudpHeader(101), {0x01}),
        udpPort);

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerBindingStats stats =
                runningServer.server.rudpBindingStats();
            return stats.inputCandidates >= 1U &&
                stats.inputDecodeFailed >= 1U &&
                stats.inputDecoded == 0U &&
                stats.inputSequenceAccepted == 0U &&
                stats.inputSequenceDuplicateRejected == 0U &&
                stats.inputSequenceStaleRejected == 0U &&
                stats.inputSequenceAmbiguousRejected == 0U &&
                runningServer.server.rudpBindingCount() == 1U;
        }));

    ::close(clientFd);
}

TEST(ServerIntegrationTests, IgnoresBoundServerOriginRudpPacketsAtAdapterGate) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t tcpPort = runningServer.server.boundPort();
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(tcpPort, 0);
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    int clientFd = connectToServer(tcpPort);
    ASSERT_GE(clientFd, 0);
    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(500)));
    uint64_t sessionId = 0;
    ASSERT_TRUE(recvWelcomePacket(clientFd, sessionId));
    std::vector<uint64_t> snapshot;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientFd, snapshot));

    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));
    sendUdpPacket(
        sender,
        serializeRudpHelloPacketForTest(100, 1, 77, sessionId),
        udpPort);

    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.rudpBindingCount() == 1U;
        }));

    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(
            rudpHeaderForType(
                101,
                Net::RudpPacketType::kBattleStart,
                Net::RudpChannelId::kEvent,
                Net::kRudpFlagReliable),
            {0x01}),
        udpPort);
    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(reliableRudpHeader(102), {0x01}),
        udpPort);
    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(
            rudpHeaderForType(
                103,
                Net::RudpPacketType::kMetaResponse,
                Net::RudpChannelId::kControl,
                Net::kRudpFlagReliable),
            {0x01}),
        udpPort);
    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(
            rudpHeaderForType(
                104,
                Net::RudpPacketType::kError,
                Net::RudpChannelId::kControl,
                Net::kRudpFlagReliable),
            {0x01}),
        udpPort);
    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(
            rudpHeaderForType(
                105,
                Net::RudpPacketType::kStateSnapshot,
                Net::RudpChannelId::kSnapshot,
                0),
            {0x01}),
        udpPort);

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerBindingStats stats =
                runningServer.server.rudpBindingStats();
            return stats.unsupportedPacketIgnored >= 5U &&
                stats.inputCandidates == 0U &&
                stats.inputSequenceAccepted == 0U &&
                runningServer.server.rudpBindingCount() == 1U;
        }));

    ::close(clientFd);
}

TEST(ServerIntegrationTests, RejectsDuplicateRudpInputCommandSequenceAtAdapterGate) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));
    int clientFd = -1;
    uint64_t sessionId = 0;
    ASSERT_TRUE(bindRudpSessionForTest(runningServer, sender, clientFd, sessionId));

    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(
            inputCommandRudpHeader(101),
            readyInputCommandPayloadForTest(77, 10)),
        udpPort);
    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.rudpBindingStats().inputSequenceAccepted >= 1U;
        }));

    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(
            inputCommandRudpHeader(102),
            readyInputCommandPayloadForTest(77, 10)),
        udpPort);

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerBindingStats stats =
                runningServer.server.rudpBindingStats();
            return stats.inputDecoded >= 2U &&
                stats.inputSequenceAccepted == 1U &&
                stats.inputSequenceDuplicateRejected >= 1U &&
                stats.inputSequenceStaleRejected == 0U &&
                stats.inputSequenceAmbiguousRejected == 0U;
        }));

    ::close(clientFd);
}

TEST(ServerIntegrationTests, AcceptsGapRudpInputCommandSequenceAtAdapterGate) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));
    int clientFd = -1;
    uint64_t sessionId = 0;
    ASSERT_TRUE(bindRudpSessionForTest(runningServer, sender, clientFd, sessionId));

    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(
            inputCommandRudpHeader(101),
            readyInputCommandPayloadForTest(77, 10)),
        udpPort);
    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(
            inputCommandRudpHeader(102),
            readyInputCommandPayloadForTest(77, 12)),
        udpPort);

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerBindingStats stats =
                runningServer.server.rudpBindingStats();
            return stats.inputDecoded >= 2U &&
                stats.inputSequenceAccepted >= 2U &&
                stats.inputSequenceDuplicateRejected == 0U &&
                stats.inputSequenceStaleRejected == 0U &&
                stats.inputSequenceAmbiguousRejected == 0U;
        }));

    ::close(clientFd);
}

TEST(ServerIntegrationTests, RejectsStaleRudpInputCommandSequenceAtAdapterGate) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));
    int clientFd = -1;
    uint64_t sessionId = 0;
    ASSERT_TRUE(bindRudpSessionForTest(runningServer, sender, clientFd, sessionId));

    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(
            inputCommandRudpHeader(101),
            readyInputCommandPayloadForTest(77, 10)),
        udpPort);
    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(
            inputCommandRudpHeader(102),
            readyInputCommandPayloadForTest(77, 12)),
        udpPort);
    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.rudpBindingStats().inputSequenceAccepted >= 2U;
        }));

    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(
            inputCommandRudpHeader(103),
            readyInputCommandPayloadForTest(77, 11)),
        udpPort);

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerBindingStats stats =
                runningServer.server.rudpBindingStats();
            return stats.inputDecoded >= 3U &&
                stats.inputSequenceAccepted == 2U &&
                stats.inputSequenceStaleRejected >= 1U &&
                stats.inputSequenceDuplicateRejected == 0U &&
                stats.inputSequenceAmbiguousRejected == 0U;
        }));

    ::close(clientFd);
}

TEST(ServerIntegrationTests, RejectsAmbiguousRudpInputCommandSequenceAtAdapterGate) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));
    int clientFd = -1;
    uint64_t sessionId = 0;
    ASSERT_TRUE(bindRudpSessionForTest(runningServer, sender, clientFd, sessionId));

    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(
            inputCommandRudpHeader(101),
            readyInputCommandPayloadForTest(77, 100)),
        udpPort);
    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.rudpBindingStats().inputSequenceAccepted >= 1U;
        }));

    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(
            inputCommandRudpHeader(102),
            readyInputCommandPayloadForTest(77, 100U + 0x80000000U)),
        udpPort);

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerBindingStats stats =
                runningServer.server.rudpBindingStats();
            return stats.inputDecoded >= 2U &&
                stats.inputSequenceAccepted == 1U &&
                stats.inputSequenceAmbiguousRejected >= 1U &&
                stats.inputSequenceDuplicateRejected == 0U &&
                stats.inputSequenceStaleRejected == 0U;
        }));

    ::close(clientFd);
}

TEST(ServerIntegrationTests, ClearsRudpInputCommandSequenceStateOnTcpDisconnect) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));
    int clientFd = -1;
    uint64_t sessionId = 0;
    ASSERT_TRUE(bindRudpSessionForTest(runningServer, sender, clientFd, sessionId));

    sendUdpPacket(
        sender,
        serializeRudpPacketForTest(
            inputCommandRudpHeader(101),
            readyInputCommandPayloadForTest(77, 10)),
        udpPort);

    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.rudpBindingStats().inputSequenceAccepted >= 1U &&
                Core::ServerTestAccess::rudpInputCommandSequenceTrackerSize(
                    runningServer.server) == 1U;
        }));

    ::close(clientFd);

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0U &&
                runningServer.server.sessionCount() == 0U &&
                runningServer.server.rudpBindingCount() == 0U &&
                Core::ServerTestAccess::rudpInputCommandSequenceTrackerSize(
                    runningServer.server) == 0U;
        }));
}

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
