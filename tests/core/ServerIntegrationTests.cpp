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
#include <string>
#include <thread>
#include <vector>

#include "Core/Server.hpp"
#include "Net/RudpBattleStartPayload.hpp"
#include "Net/RudpGameEventPayload.hpp"
#include "Net/RudpHelloPayload.hpp"
#include "Net/RudpInputCommandPayload.hpp"
#include "Net/RudpMetaResponsePayload.hpp"
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

    static RudpServerReliableEventTrackResult trackRudpReliableEvent(
        Server& server,
        uint64_t sessionId,
        const Net::RudpReliableEventDescriptor& descriptor,
        uint32_t sequence,
        const std::vector<uint8_t>& packetBytes,
        Util::TimePoint now) {
        return server.trackRudpReliableEventForSession(
            sessionId,
            descriptor,
            sequence,
            packetBytes,
            now);
    }

    static void clearRudpReliableEventsForSession(
        Server& server,
        uint64_t sessionId) {
        server.clearRudpReliableEventsForSession(sessionId);
    }

    static bool broadcastBattleStart(
        Server& server,
        const Game::RoomCommandResult& result,
        std::vector<int>& disconnectedClients) {
        return server.broadcastBattleStart(result, disconnectedClients);
    }

    static bool broadcastMonsterDeath(
        Server& server,
        const Game::RoomCommandResult& result,
        std::vector<int>& disconnectedClients) {
        return server.broadcastMonsterDeath(result, disconnectedClients);
    }

    static bool broadcastLootResolved(
        Server& server,
        const Game::RoomCommandResult& result,
        std::vector<int>& disconnectedClients) {
        return server.broadcastLootResolved(result, disconnectedClients);
    }

    static bool observeRudpMetaResponse(
        Server& server,
        uint64_t sessionId,
        const Net::RudpMetaResponsePayload& payload,
        Util::TimePoint now) {
        return server.observeRudpMetaResponseForSession(
            sessionId,
            payload,
            now);
    }

    static size_t rudpMetaResponseCompletionCount(Server& server) {
        return server.rudpMetaResponseIdempotencyTracker_.completionCount();
    }

    static size_t rudpMetaResponseRetryCount(Server& server) {
        return server.rudpMetaResponseIdempotencyTracker_.retryCount();
    }

    static size_t rudpReliableEventSessionQueueCount(Server& server) {
        return server.rudpReliableEventQueues_.size();
    }

    static size_t rudpReliableEventSequenceAllocatorCount(Server& server) {
        return server.rudpReliableEventNextSequenceBySession_.size();
    }

    static const Net::RudpReliableEventPendingEntry*
    rudpReliableEventPendingEntry(
        Server& server,
        uint64_t sessionId,
        uint32_t sequence) {
        const auto queueIt = server.rudpReliableEventQueues_.find(sessionId);
        if (queueIt == server.rudpReliableEventQueues_.end()) {
            return nullptr;
        }
        return queueIt->second.pendingEntry(sequence);
    }

    static const std::vector<uint8_t>* rudpReliableEventPacketBytes(
        Server& server,
        uint64_t sessionId,
        uint32_t sequence) {
        const auto queueIt = server.rudpReliableEventQueues_.find(sessionId);
        if (queueIt == server.rudpReliableEventQueues_.end()) {
            return nullptr;
        }
        return queueIt->second.packetBytes(sequence);
    }
};
}  // namespace Core

namespace {
Util::TimePoint timeAt(int64_t milliseconds) {
    return Util::TimePoint{std::chrono::milliseconds(milliseconds)};
}

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

std::vector<uint8_t> reliableEventPacketBytesForTest(uint8_t marker) {
    return {0x4C, 0x4F, marker};
}

Net::RudpReliableEventDescriptor reliableEventDescriptorForTest(
    Net::RudpReliableEventKind kind,
    const std::string& logicalKey) {
    switch (kind) {
    case Net::RudpReliableEventKind::kBattleStart:
        return Net::RudpReliableEventDescriptor{
            kind,
            logicalKey,
            static_cast<uint16_t>(Net::RudpPacketType::kBattleStart),
            static_cast<uint8_t>(Net::RudpChannelId::kEvent)};
    case Net::RudpReliableEventKind::kMonsterDeath:
    case Net::RudpReliableEventKind::kLootResolved:
        return Net::RudpReliableEventDescriptor{
            kind,
            logicalKey,
            static_cast<uint16_t>(Net::RudpPacketType::kGameEvent),
            static_cast<uint8_t>(Net::RudpChannelId::kEvent)};
    case Net::RudpReliableEventKind::kMetaResponse:
        return Net::RudpReliableEventDescriptor{
            kind,
            logicalKey,
            static_cast<uint16_t>(Net::RudpPacketType::kMetaResponse),
            static_cast<uint8_t>(Net::RudpChannelId::kControl)};
    }

    return Net::RudpReliableEventDescriptor{
        kind,
        logicalKey,
        static_cast<uint16_t>(Net::RudpPacketType::kError),
        static_cast<uint8_t>(Net::RudpChannelId::kControl)};
}

Net::RudpMetaResponsePayload metaResponsePayloadForTest(
    const std::string& settlementId,
    Net::RudpMetaResponseOp op,
    Net::RudpMetaResponseStatus status,
    uint32_t retryAfterMs = 0) {
    return Net::RudpMetaResponsePayload{op, settlementId, status, retryAfterMs};
}

Game::RoomCommandResult battleStartResultForTest(
    uint32_t roomId,
    std::vector<uint64_t> playerSessionIds) {
    const uint16_t playerCount =
        static_cast<uint16_t>(playerSessionIds.size());
    return Game::RoomCommandResult(
        true,
        Game::RoomCommandError::kNone,
        Game::RoomSummary(roomId, playerCount, 2, playerCount, true),
        std::move(playerSessionIds),
        true);
}

Game::RoomCommandResult monsterDeathResultForTest(
    uint32_t roomId,
    uint32_t monsterId,
    std::vector<uint64_t> playerSessionIds) {
    const uint16_t playerCount =
        static_cast<uint16_t>(playerSessionIds.size());
    return Game::RoomCommandResult(
        true,
        Game::RoomCommandError::kNone,
        Game::RoomSummary(roomId, playerCount, 2, playerCount, true, false),
        std::move(playerSessionIds),
        false,
        false,
        true,
        Game::Monster{monsterId, 2001, 10, false});
}

Game::RoomCommandResult lootResolvedResultForTest(
    uint32_t roomId,
    uint32_t dropId,
    uint64_t winnerSessionId,
    uint32_t itemId,
    uint16_t quantity,
    std::vector<uint64_t> playerSessionIds) {
    const uint16_t playerCount =
        static_cast<uint16_t>(playerSessionIds.size());
    Game::RoomCommandResult result(
        true,
        Game::RoomCommandError::kNone,
        Game::RoomSummary(roomId, playerCount, 2, playerCount, true, false),
        std::move(playerSessionIds));
    result.lootJustClaimed = true;
    result.winnerSessionId = winnerSessionId;
    result.drop = Game::Drop{dropId, itemId, quantity};
    return result;
}

void expectBattleStartPending(
    Core::Server& server,
    uint64_t sessionId,
    uint32_t sequence,
    const std::string& expectedLogicalKey,
    uint32_t expectedRoomId,
    uint64_t expectedPlayerA,
    uint64_t expectedPlayerB) {
    const Net::RudpReliableEventPendingEntry* entry =
        Core::ServerTestAccess::rudpReliableEventPendingEntry(
            server,
            sessionId,
            sequence);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->descriptor.kind, Net::RudpReliableEventKind::kBattleStart);
    EXPECT_EQ(entry->descriptor.logicalKey, expectedLogicalKey);
    EXPECT_EQ(
        entry->descriptor.packetType,
        static_cast<uint16_t>(Net::RudpPacketType::kBattleStart));
    EXPECT_EQ(
        entry->descriptor.channelId,
        static_cast<uint8_t>(Net::RudpChannelId::kEvent));

    const std::vector<uint8_t>* packetBytes =
        Core::ServerTestAccess::rudpReliableEventPacketBytes(
            server,
            sessionId,
            sequence);
    ASSERT_NE(packetBytes, nullptr);

    Net::RudpBattleStartPayload payload;
    ASSERT_TRUE(Net::parseRudpBattleStartPayload(
        packetBytes->data(),
        packetBytes->size(),
        payload));
    EXPECT_EQ(payload.roomId, expectedRoomId);
    EXPECT_EQ(payload.playerASessionId, expectedPlayerA);
    EXPECT_EQ(payload.playerBSessionId, expectedPlayerB);
}

void expectMonsterDeathPending(
    Core::Server& server,
    uint64_t sessionId,
    uint32_t sequence,
    const std::string& expectedLogicalKey,
    uint32_t expectedRoomId,
    uint32_t expectedMonsterId) {
    const Net::RudpReliableEventPendingEntry* entry =
        Core::ServerTestAccess::rudpReliableEventPendingEntry(
            server,
            sessionId,
            sequence);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->descriptor.kind, Net::RudpReliableEventKind::kMonsterDeath);
    EXPECT_EQ(entry->descriptor.logicalKey, expectedLogicalKey);
    EXPECT_EQ(
        entry->descriptor.packetType,
        static_cast<uint16_t>(Net::RudpPacketType::kGameEvent));
    EXPECT_EQ(
        entry->descriptor.channelId,
        static_cast<uint8_t>(Net::RudpChannelId::kEvent));

    const std::vector<uint8_t>* packetBytes =
        Core::ServerTestAccess::rudpReliableEventPacketBytes(
            server,
            sessionId,
            sequence);
    ASSERT_NE(packetBytes, nullptr);

    Net::RudpMonsterDeathGameEventPayload payload;
    ASSERT_TRUE(Net::parseRudpMonsterDeathGameEventPayload(
        packetBytes->data(),
        packetBytes->size(),
        payload));
    EXPECT_EQ(payload.roomId, expectedRoomId);
    EXPECT_EQ(payload.monsterId, expectedMonsterId);
}

void expectLootResolvedPending(
    Core::Server& server,
    uint64_t sessionId,
    uint32_t sequence,
    const std::string& expectedLogicalKey,
    uint32_t expectedRoomId,
    uint32_t expectedDropId,
    uint64_t expectedWinnerSessionId,
    uint32_t expectedItemId,
    uint16_t expectedQuantity) {
    const Net::RudpReliableEventPendingEntry* entry =
        Core::ServerTestAccess::rudpReliableEventPendingEntry(
            server,
            sessionId,
            sequence);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->descriptor.kind, Net::RudpReliableEventKind::kLootResolved);
    EXPECT_EQ(entry->descriptor.logicalKey, expectedLogicalKey);
    EXPECT_EQ(
        entry->descriptor.packetType,
        static_cast<uint16_t>(Net::RudpPacketType::kGameEvent));
    EXPECT_EQ(
        entry->descriptor.channelId,
        static_cast<uint8_t>(Net::RudpChannelId::kEvent));

    const std::vector<uint8_t>* packetBytes =
        Core::ServerTestAccess::rudpReliableEventPacketBytes(
            server,
            sessionId,
            sequence);
    ASSERT_NE(packetBytes, nullptr);

    Net::RudpLootResolvedGameEventPayload payload;
    ASSERT_TRUE(Net::parseRudpLootResolvedGameEventPayload(
        packetBytes->data(),
        packetBytes->size(),
        payload));
    EXPECT_EQ(payload.roomId, expectedRoomId);
    EXPECT_EQ(payload.dropId, expectedDropId);
    EXPECT_EQ(payload.winnerSessionId, expectedWinnerSessionId);
    EXPECT_EQ(payload.itemId, expectedItemId);
    EXPECT_EQ(payload.quantity, expectedQuantity);
}

void expectMetaResponsePending(
    Core::Server& server,
    uint64_t sessionId,
    uint32_t sequence,
    const std::string& expectedSettlementId,
    Net::RudpMetaResponseOp expectedOp,
    Net::RudpMetaResponseStatus expectedStatus,
    uint32_t expectedRetryAfterMs) {
    const Net::RudpReliableEventPendingEntry* entry =
        Core::ServerTestAccess::rudpReliableEventPendingEntry(
            server,
            sessionId,
            sequence);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->descriptor.kind, Net::RudpReliableEventKind::kMetaResponse);
    EXPECT_EQ(entry->descriptor.logicalKey, expectedSettlementId);
    EXPECT_EQ(
        entry->descriptor.packetType,
        static_cast<uint16_t>(Net::RudpPacketType::kMetaResponse));
    EXPECT_EQ(
        entry->descriptor.channelId,
        static_cast<uint8_t>(Net::RudpChannelId::kControl));

    const std::vector<uint8_t>* packetBytes =
        Core::ServerTestAccess::rudpReliableEventPacketBytes(
            server,
            sessionId,
            sequence);
    ASSERT_NE(packetBytes, nullptr);

    Net::RudpMetaResponsePayload payload;
    ASSERT_TRUE(Net::parseRudpMetaResponsePayload(
        packetBytes->data(),
        packetBytes->size(),
        payload));
    EXPECT_EQ(payload.op, expectedOp);
    EXPECT_EQ(payload.settlementId, expectedSettlementId);
    EXPECT_EQ(payload.status, expectedStatus);
    EXPECT_EQ(payload.retryAfterMs, expectedRetryAfterMs);
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

TEST(ServerIntegrationTests, RudpReliableEventTracksValidBattleStartDescriptor) {
    Core::Server server(0);

    EXPECT_EQ(
        Core::ServerTestAccess::trackRudpReliableEvent(
            server,
            1001,
            reliableEventDescriptorForTest(
                Net::RudpReliableEventKind::kBattleStart,
                "room-42:1001:1002"),
            10,
            reliableEventPacketBytesForTest(0x10),
            timeAt(1000)),
        Core::RudpServerReliableEventTrackResult::kTracked);

    const Core::RudpServerReliableEventStats stats =
        server.rudpReliableEventStats();
    EXPECT_EQ(stats.tracked, 1U);
    EXPECT_EQ(stats.duplicateSequence, 0U);
    EXPECT_EQ(stats.duplicateLogicalEvent, 0U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 1U);
    EXPECT_EQ(Core::ServerTestAccess::rudpReliableEventSessionQueueCount(server), 1U);
}

TEST(ServerIntegrationTests, RudpReliableEventRejectsDuplicateSequence) {
    Core::Server server(0);
    ASSERT_EQ(
        Core::ServerTestAccess::trackRudpReliableEvent(
            server,
            1001,
            reliableEventDescriptorForTest(
                Net::RudpReliableEventKind::kBattleStart,
                "battle-a"),
            10,
            reliableEventPacketBytesForTest(0x10),
            timeAt(1000)),
        Core::RudpServerReliableEventTrackResult::kTracked);

    EXPECT_EQ(
        Core::ServerTestAccess::trackRudpReliableEvent(
            server,
            1001,
            reliableEventDescriptorForTest(
                Net::RudpReliableEventKind::kMonsterDeath,
                "monster-b"),
            10,
            reliableEventPacketBytesForTest(0x20),
            timeAt(1000)),
        Core::RudpServerReliableEventTrackResult::kDuplicateSequence);

    const Core::RudpServerReliableEventStats stats =
        server.rudpReliableEventStats();
    EXPECT_EQ(stats.tracked, 1U);
    EXPECT_EQ(stats.duplicateSequence, 1U);
    EXPECT_EQ(stats.duplicateLogicalEvent, 0U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 1U);
}

TEST(ServerIntegrationTests, RudpReliableEventRejectsDuplicateLogicalEventPerSession) {
    Core::Server server(0);
    const Net::RudpReliableEventDescriptor descriptor =
        reliableEventDescriptorForTest(
            Net::RudpReliableEventKind::kLootResolved,
            "room-42:drop-77");
    ASSERT_EQ(
        Core::ServerTestAccess::trackRudpReliableEvent(
            server,
            1001,
            descriptor,
            20,
            reliableEventPacketBytesForTest(0x20),
            timeAt(1000)),
        Core::RudpServerReliableEventTrackResult::kTracked);

    EXPECT_EQ(
        Core::ServerTestAccess::trackRudpReliableEvent(
            server,
            1001,
            descriptor,
            21,
            reliableEventPacketBytesForTest(0x21),
            timeAt(1000)),
        Core::RudpServerReliableEventTrackResult::kDuplicateLogicalEvent);

    const Core::RudpServerReliableEventStats stats =
        server.rudpReliableEventStats();
    EXPECT_EQ(stats.tracked, 1U);
    EXPECT_EQ(stats.duplicateLogicalEvent, 1U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 1U);
}

TEST(ServerIntegrationTests, RudpReliableEventAllowsSameLogicalKeyAcrossSessions) {
    Core::Server server(0);
    const Net::RudpReliableEventDescriptor descriptor =
        reliableEventDescriptorForTest(
            Net::RudpReliableEventKind::kMetaResponse,
            "settlement-1");

    EXPECT_EQ(
        Core::ServerTestAccess::trackRudpReliableEvent(
            server,
            1001,
            descriptor,
            30,
            reliableEventPacketBytesForTest(0x30),
            timeAt(1000)),
        Core::RudpServerReliableEventTrackResult::kTracked);
    EXPECT_EQ(
        Core::ServerTestAccess::trackRudpReliableEvent(
            server,
            1002,
            descriptor,
            30,
            reliableEventPacketBytesForTest(0x31),
            timeAt(1000)),
        Core::RudpServerReliableEventTrackResult::kTracked);

    const Core::RudpServerReliableEventStats stats =
        server.rudpReliableEventStats();
    EXPECT_EQ(stats.tracked, 2U);
    EXPECT_EQ(stats.duplicateSequence, 0U);
    EXPECT_EQ(stats.duplicateLogicalEvent, 0U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 2U);
    EXPECT_EQ(Core::ServerTestAccess::rudpReliableEventSessionQueueCount(server), 2U);
}

TEST(ServerIntegrationTests, RudpMetaResponseFirstTerminalEnqueuesCompletion) {
    Core::Server server(0);
    const std::string settlementId = "room-42-session-1001-finish-1";

    EXPECT_TRUE(Core::ServerTestAccess::observeRudpMetaResponse(
        server,
        1001,
        metaResponsePayloadForTest(
            settlementId,
            Net::RudpMetaResponseOp::kApplied,
            Net::RudpMetaResponseStatus::kApplied),
        timeAt(1000)));

    const Core::RudpServerMetaResponseStats metaStats =
        server.rudpMetaResponseStats();
    EXPECT_EQ(metaStats.completedFirst, 1U);
    EXPECT_EQ(metaStats.completionDuplicate, 0U);
    EXPECT_EQ(metaStats.retryObserved, 0U);
    EXPECT_EQ(metaStats.invalidPayload, 0U);
    EXPECT_EQ(metaStats.enqueued, 1U);

    const Core::RudpServerReliableEventStats eventStats =
        server.rudpReliableEventStats();
    EXPECT_EQ(eventStats.tracked, 1U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 1U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMetaResponseCompletionCount(server), 1U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMetaResponseRetryCount(server), 0U);
    expectMetaResponsePending(
        server,
        1001,
        1,
        settlementId,
        Net::RudpMetaResponseOp::kApplied,
        Net::RudpMetaResponseStatus::kApplied,
        0);
}

TEST(
    ServerIntegrationTests,
    RudpMetaResponseTerminalDuplicateDoesNotGrowPending) {
    Core::Server server(0);
    const std::string settlementId = "room-42-session-1001-finish-1";
    ASSERT_TRUE(Core::ServerTestAccess::observeRudpMetaResponse(
        server,
        1001,
        metaResponsePayloadForTest(
            settlementId,
            Net::RudpMetaResponseOp::kApplied,
            Net::RudpMetaResponseStatus::kApplied),
        timeAt(1000)));

    EXPECT_FALSE(Core::ServerTestAccess::observeRudpMetaResponse(
        server,
        1001,
        metaResponsePayloadForTest(
            settlementId,
            Net::RudpMetaResponseOp::kRejected,
            Net::RudpMetaResponseStatus::kRejected),
        timeAt(1100)));

    const Core::RudpServerMetaResponseStats metaStats =
        server.rudpMetaResponseStats();
    EXPECT_EQ(metaStats.completedFirst, 1U);
    EXPECT_EQ(metaStats.completionDuplicate, 1U);
    EXPECT_EQ(metaStats.enqueued, 1U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 1U);
    EXPECT_EQ(
        Core::ServerTestAccess::rudpReliableEventPendingEntry(
            server,
            1001,
            2),
        nullptr);
    EXPECT_EQ(Core::ServerTestAccess::rudpMetaResponseCompletionCount(server), 1U);
}

TEST(ServerIntegrationTests, RudpMetaResponseRetryLaterEnqueuesOnce) {
    Core::Server server(0);
    const std::string settlementId = "room-42-session-1001-finish-1";

    ASSERT_TRUE(Core::ServerTestAccess::observeRudpMetaResponse(
        server,
        1001,
        metaResponsePayloadForTest(
            settlementId,
            Net::RudpMetaResponseOp::kRetryLater,
            Net::RudpMetaResponseStatus::kRetryLater,
            250),
        timeAt(1000)));
    EXPECT_FALSE(Core::ServerTestAccess::observeRudpMetaResponse(
        server,
        1001,
        metaResponsePayloadForTest(
            settlementId,
            Net::RudpMetaResponseOp::kRetryLater,
            Net::RudpMetaResponseStatus::kRetryLater,
            500),
        timeAt(1100)));

    const Core::RudpServerMetaResponseStats metaStats =
        server.rudpMetaResponseStats();
    EXPECT_EQ(metaStats.completedFirst, 0U);
    EXPECT_EQ(metaStats.retryObserved, 1U);
    EXPECT_EQ(metaStats.retryDuplicate, 1U);
    EXPECT_EQ(metaStats.enqueued, 1U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 1U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMetaResponseCompletionCount(server), 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMetaResponseRetryCount(server), 1U);
    expectMetaResponsePending(
        server,
        1001,
        1,
        settlementId,
        Net::RudpMetaResponseOp::kRetryLater,
        Net::RudpMetaResponseStatus::kRetryLater,
        250);
}

TEST(
    ServerIntegrationTests,
    RudpMetaResponseRetryAfterCompletionIsIgnored) {
    Core::Server server(0);
    const std::string settlementId = "room-42-session-1001-finish-1";
    ASSERT_TRUE(Core::ServerTestAccess::observeRudpMetaResponse(
        server,
        1001,
        metaResponsePayloadForTest(
            settlementId,
            Net::RudpMetaResponseOp::kApplied,
            Net::RudpMetaResponseStatus::kApplied),
        timeAt(1000)));

    EXPECT_FALSE(Core::ServerTestAccess::observeRudpMetaResponse(
        server,
        1001,
        metaResponsePayloadForTest(
            settlementId,
            Net::RudpMetaResponseOp::kRetryLater,
            Net::RudpMetaResponseStatus::kRetryLater,
            250),
        timeAt(1100)));

    const Core::RudpServerMetaResponseStats metaStats =
        server.rudpMetaResponseStats();
    EXPECT_EQ(metaStats.completedFirst, 1U);
    EXPECT_EQ(metaStats.retryIgnoredAfterCompletion, 1U);
    EXPECT_EQ(metaStats.enqueued, 1U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 1U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMetaResponseCompletionCount(server), 1U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMetaResponseRetryCount(server), 0U);
}

TEST(
    ServerIntegrationTests,
    RudpMetaResponseTerminalAfterRetryCompletesAfterPendingCleanup) {
    Core::Server server(0);
    const std::string settlementId = "room-42-session-1001-finish-1";
    ASSERT_TRUE(Core::ServerTestAccess::observeRudpMetaResponse(
        server,
        1001,
        metaResponsePayloadForTest(
            settlementId,
            Net::RudpMetaResponseOp::kRetryLater,
            Net::RudpMetaResponseStatus::kRetryLater,
            250),
        timeAt(1000)));
    ASSERT_EQ(server.rudpReliableEventPendingCount(), 1U);
    ASSERT_EQ(Core::ServerTestAccess::rudpMetaResponseRetryCount(server), 1U);

    Core::ServerTestAccess::clearRudpReliableEventsForSession(server, 1001);
    ASSERT_EQ(server.rudpReliableEventPendingCount(), 0U);
    ASSERT_EQ(Core::ServerTestAccess::rudpMetaResponseRetryCount(server), 1U);

    EXPECT_TRUE(Core::ServerTestAccess::observeRudpMetaResponse(
        server,
        1001,
        metaResponsePayloadForTest(
            settlementId,
            Net::RudpMetaResponseOp::kApplied,
            Net::RudpMetaResponseStatus::kApplied),
        timeAt(1200)));

    const Core::RudpServerMetaResponseStats metaStats =
        server.rudpMetaResponseStats();
    EXPECT_EQ(metaStats.completedFirst, 1U);
    EXPECT_EQ(metaStats.retryObserved, 1U);
    EXPECT_EQ(metaStats.enqueued, 2U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 1U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMetaResponseCompletionCount(server), 1U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMetaResponseRetryCount(server), 0U);
    expectMetaResponsePending(
        server,
        1001,
        1,
        settlementId,
        Net::RudpMetaResponseOp::kApplied,
        Net::RudpMetaResponseStatus::kApplied,
        0);
}

TEST(ServerIntegrationTests, RudpMetaResponseRejectsInvalidPayload) {
    Core::Server server(0);
    EXPECT_FALSE(Core::ServerTestAccess::observeRudpMetaResponse(
        server,
        1001,
        metaResponsePayloadForTest(
            "",
            Net::RudpMetaResponseOp::kApplied,
            Net::RudpMetaResponseStatus::kApplied),
        timeAt(1000)));
    EXPECT_FALSE(Core::ServerTestAccess::observeRudpMetaResponse(
        server,
        1001,
        metaResponsePayloadForTest(
            "room-42-session-1001-finish-1",
            Net::RudpMetaResponseOp::kApplied,
            Net::RudpMetaResponseStatus::kRetryLater),
        timeAt(1100)));

    const Core::RudpServerMetaResponseStats metaStats =
        server.rudpMetaResponseStats();
    EXPECT_EQ(metaStats.completedFirst, 0U);
    EXPECT_EQ(metaStats.retryObserved, 0U);
    EXPECT_EQ(metaStats.invalidPayload, 2U);
    EXPECT_EQ(metaStats.enqueued, 0U);

    const Core::RudpServerReliableEventStats eventStats =
        server.rudpReliableEventStats();
    EXPECT_EQ(eventStats.tracked, 0U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpReliableEventSessionQueueCount(server), 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMetaResponseCompletionCount(server), 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMetaResponseRetryCount(server), 0U);
}

TEST(ServerIntegrationTests, RudpReliableEventRejectsInvalidInputsWithoutPendingState) {
    Core::Server server(0);
    const Net::RudpReliableEventDescriptor validDescriptor =
        reliableEventDescriptorForTest(
            Net::RudpReliableEventKind::kBattleStart,
            "battle-a");
    const Net::RudpReliableEventDescriptor invalidDescriptor{
        Net::RudpReliableEventKind::kBattleStart,
        "wrong-packet",
        static_cast<uint16_t>(Net::RudpPacketType::kGameEvent),
        static_cast<uint8_t>(Net::RudpChannelId::kEvent)};

    EXPECT_EQ(
        Core::ServerTestAccess::trackRudpReliableEvent(
            server,
            0,
            validDescriptor,
            40,
            reliableEventPacketBytesForTest(0x40),
            timeAt(1000)),
        Core::RudpServerReliableEventTrackResult::kInvalidSession);
    EXPECT_EQ(
        Core::ServerTestAccess::trackRudpReliableEvent(
            server,
            1001,
            invalidDescriptor,
            41,
            reliableEventPacketBytesForTest(0x41),
            timeAt(1000)),
        Core::RudpServerReliableEventTrackResult::kInvalidDescriptor);
    EXPECT_EQ(
        Core::ServerTestAccess::trackRudpReliableEvent(
            server,
            1001,
            validDescriptor,
            42,
            {},
            timeAt(1000)),
        Core::RudpServerReliableEventTrackResult::kInvalidPacketBytes);

    const Core::RudpServerReliableEventStats stats =
        server.rudpReliableEventStats();
    EXPECT_EQ(stats.tracked, 0U);
    EXPECT_EQ(stats.invalidSession, 1U);
    EXPECT_EQ(stats.invalidDescriptor, 1U);
    EXPECT_EQ(stats.invalidPacketBytes, 1U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpReliableEventSessionQueueCount(server), 0U);
}

TEST(ServerIntegrationTests, RudpReliableEventCleanupRemovesSessionPendingQueue) {
    Core::Server server(0);
    const Net::RudpReliableEventDescriptor descriptor =
        reliableEventDescriptorForTest(
            Net::RudpReliableEventKind::kBattleStart,
            "battle-a");
    ASSERT_EQ(
        Core::ServerTestAccess::trackRudpReliableEvent(
            server,
            1001,
            descriptor,
            50,
            reliableEventPacketBytesForTest(0x50),
            timeAt(1000)),
        Core::RudpServerReliableEventTrackResult::kTracked);
    ASSERT_EQ(
        Core::ServerTestAccess::trackRudpReliableEvent(
            server,
            1002,
            descriptor,
            50,
            reliableEventPacketBytesForTest(0x51),
            timeAt(1000)),
        Core::RudpServerReliableEventTrackResult::kTracked);
    ASSERT_EQ(server.rudpReliableEventPendingCount(), 2U);

    Core::ServerTestAccess::clearRudpReliableEventsForSession(server, 1001);

    EXPECT_EQ(server.rudpReliableEventPendingCount(), 1U);
    EXPECT_EQ(Core::ServerTestAccess::rudpReliableEventSessionQueueCount(server), 1U);

    Core::ServerTestAccess::clearRudpReliableEventsForSession(server, 1002);

    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpReliableEventSessionQueueCount(server), 0U);
}

TEST(ServerIntegrationTests, RudpBattleStartReliableEventEnqueuedForRoomPlayers) {
    Core::Server server(0);
    std::vector<int> disconnectedClients;
    const Game::RoomCommandResult result =
        battleStartResultForTest(42, {1002, 1001});

    EXPECT_TRUE(Core::ServerTestAccess::broadcastBattleStart(
        server,
        result,
        disconnectedClients));

    const Core::RudpServerReliableEventStats stats =
        server.rudpReliableEventStats();
    EXPECT_EQ(stats.tracked, 2U);
    EXPECT_EQ(stats.duplicateLogicalEvent, 0U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 2U);
    EXPECT_EQ(Core::ServerTestAccess::rudpReliableEventSessionQueueCount(server), 2U);
    EXPECT_EQ(
        Core::ServerTestAccess::rudpReliableEventSequenceAllocatorCount(server),
        2U);

    expectBattleStartPending(
        server,
        1001,
        1,
        "BattleStart:42:1001:1002",
        42,
        1001,
        1002);
    expectBattleStartPending(
        server,
        1002,
        1,
        "BattleStart:42:1001:1002",
        42,
        1001,
        1002);
    EXPECT_TRUE(disconnectedClients.empty());
}

TEST(
    ServerIntegrationTests,
    RudpBattleStartReliableEventDuplicateObservationDoesNotGrowPending) {
    Core::Server server(0);
    std::vector<int> disconnectedClients;
    const Game::RoomCommandResult result =
        battleStartResultForTest(42, {1001, 1002});

    ASSERT_TRUE(Core::ServerTestAccess::broadcastBattleStart(
        server,
        result,
        disconnectedClients));
    ASSERT_TRUE(Core::ServerTestAccess::broadcastBattleStart(
        server,
        result,
        disconnectedClients));

    const Core::RudpServerReliableEventStats stats =
        server.rudpReliableEventStats();
    EXPECT_EQ(stats.tracked, 2U);
    EXPECT_EQ(stats.duplicateSequence, 0U);
    EXPECT_EQ(stats.duplicateLogicalEvent, 2U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 2U);
    EXPECT_EQ(
        Core::ServerTestAccess::rudpReliableEventPendingEntry(
            server,
            1001,
            2),
        nullptr);
    EXPECT_EQ(
        Core::ServerTestAccess::rudpReliableEventPendingEntry(
            server,
            1002,
            2),
        nullptr);
}

TEST(ServerIntegrationTests, RudpBattleStartReliableEventCleanupRemovesSequenceState) {
    Core::Server server(0);
    std::vector<int> disconnectedClients;
    const Game::RoomCommandResult result =
        battleStartResultForTest(42, {1001, 1002});

    ASSERT_TRUE(Core::ServerTestAccess::broadcastBattleStart(
        server,
        result,
        disconnectedClients));
    ASSERT_EQ(
        Core::ServerTestAccess::rudpReliableEventSequenceAllocatorCount(server),
        2U);

    Core::ServerTestAccess::clearRudpReliableEventsForSession(server, 1001);

    EXPECT_EQ(server.rudpReliableEventPendingCount(), 1U);
    EXPECT_EQ(Core::ServerTestAccess::rudpReliableEventSessionQueueCount(server), 1U);
    EXPECT_EQ(
        Core::ServerTestAccess::rudpReliableEventSequenceAllocatorCount(server),
        1U);

    Core::ServerTestAccess::clearRudpReliableEventsForSession(server, 1002);

    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpReliableEventSessionQueueCount(server), 0U);
    EXPECT_EQ(
        Core::ServerTestAccess::rudpReliableEventSequenceAllocatorCount(server),
        0U);
}

TEST(ServerIntegrationTests, RudpBattleStartReliableEventSkipsInvalidShape) {
    Core::Server server(0);
    std::vector<int> disconnectedClients;

    EXPECT_TRUE(Core::ServerTestAccess::broadcastBattleStart(
        server,
        battleStartResultForTest(0, {1001, 1002}),
        disconnectedClients));
    EXPECT_TRUE(Core::ServerTestAccess::broadcastBattleStart(
        server,
        battleStartResultForTest(42, {1001, 1001}),
        disconnectedClients));
    EXPECT_TRUE(Core::ServerTestAccess::broadcastBattleStart(
        server,
        battleStartResultForTest(42, {1001}),
        disconnectedClients));

    const Core::RudpServerReliableEventStats stats =
        server.rudpReliableEventStats();
    EXPECT_EQ(stats.tracked, 0U);
    EXPECT_EQ(stats.duplicateSequence, 0U);
    EXPECT_EQ(stats.duplicateLogicalEvent, 0U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpReliableEventSessionQueueCount(server), 0U);
    EXPECT_EQ(
        Core::ServerTestAccess::rudpReliableEventSequenceAllocatorCount(server),
        0U);
}

TEST(ServerIntegrationTests, RudpGameEventMonsterDeathEnqueuedForRoomPlayers) {
    Core::Server server(0);
    std::vector<int> disconnectedClients;
    const Game::RoomCommandResult result =
        monsterDeathResultForTest(42, 7, {1001, 1002});

    EXPECT_TRUE(Core::ServerTestAccess::broadcastMonsterDeath(
        server,
        result,
        disconnectedClients));

    const Core::RudpServerReliableEventStats stats =
        server.rudpReliableEventStats();
    EXPECT_EQ(stats.tracked, 2U);
    EXPECT_EQ(stats.duplicateLogicalEvent, 0U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 2U);
    EXPECT_EQ(Core::ServerTestAccess::rudpReliableEventSessionQueueCount(server), 2U);

    expectMonsterDeathPending(
        server,
        1001,
        1,
        "MonsterDeath:42:7",
        42,
        7);
    expectMonsterDeathPending(
        server,
        1002,
        1,
        "MonsterDeath:42:7",
        42,
        7);
    EXPECT_TRUE(disconnectedClients.empty());
}

TEST(ServerIntegrationTests, RudpGameEventLootResolvedEnqueuedForRoomPlayers) {
    Core::Server server(0);
    std::vector<int> disconnectedClients;
    const Game::RoomCommandResult result =
        lootResolvedResultForTest(42, 77, 1001, 3001, 2, {1001, 1002});

    EXPECT_TRUE(Core::ServerTestAccess::broadcastLootResolved(
        server,
        result,
        disconnectedClients));

    const Core::RudpServerReliableEventStats stats =
        server.rudpReliableEventStats();
    EXPECT_EQ(stats.tracked, 2U);
    EXPECT_EQ(stats.duplicateLogicalEvent, 0U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 2U);
    EXPECT_EQ(Core::ServerTestAccess::rudpReliableEventSessionQueueCount(server), 2U);

    expectLootResolvedPending(
        server,
        1001,
        1,
        "LootResolved:42:77",
        42,
        77,
        1001,
        3001,
        2);
    expectLootResolvedPending(
        server,
        1002,
        1,
        "LootResolved:42:77",
        42,
        77,
        1001,
        3001,
        2);
    EXPECT_TRUE(disconnectedClients.empty());
}

TEST(
    ServerIntegrationTests,
    RudpGameEventDuplicateObservationDoesNotGrowPending) {
    Core::Server server(0);
    std::vector<int> disconnectedClients;
    const Game::RoomCommandResult monsterDeath =
        monsterDeathResultForTest(42, 7, {1001, 1002});
    const Game::RoomCommandResult lootResolved =
        lootResolvedResultForTest(42, 77, 1001, 3001, 2, {1001, 1002});

    ASSERT_TRUE(Core::ServerTestAccess::broadcastMonsterDeath(
        server,
        monsterDeath,
        disconnectedClients));
    ASSERT_TRUE(Core::ServerTestAccess::broadcastLootResolved(
        server,
        lootResolved,
        disconnectedClients));
    ASSERT_TRUE(Core::ServerTestAccess::broadcastMonsterDeath(
        server,
        monsterDeath,
        disconnectedClients));
    ASSERT_TRUE(Core::ServerTestAccess::broadcastLootResolved(
        server,
        lootResolved,
        disconnectedClients));

    const Core::RudpServerReliableEventStats stats =
        server.rudpReliableEventStats();
    EXPECT_EQ(stats.tracked, 4U);
    EXPECT_EQ(stats.duplicateSequence, 0U);
    EXPECT_EQ(stats.duplicateLogicalEvent, 4U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 4U);
    EXPECT_EQ(
        Core::ServerTestAccess::rudpReliableEventPendingEntry(
            server,
            1001,
            3),
        nullptr);
    EXPECT_EQ(
        Core::ServerTestAccess::rudpReliableEventPendingEntry(
            server,
            1002,
            4),
        nullptr);
}

TEST(
    ServerIntegrationTests,
    RudpGameEventDistinctKindsUseDistinctKeysAndSequences) {
    Core::Server server(0);
    std::vector<int> disconnectedClients;

    ASSERT_TRUE(Core::ServerTestAccess::broadcastMonsterDeath(
        server,
        monsterDeathResultForTest(42, 7, {1001, 1002}),
        disconnectedClients));
    ASSERT_TRUE(Core::ServerTestAccess::broadcastLootResolved(
        server,
        lootResolvedResultForTest(42, 7, 1001, 3001, 2, {1001, 1002}),
        disconnectedClients));

    const Core::RudpServerReliableEventStats stats =
        server.rudpReliableEventStats();
    EXPECT_EQ(stats.tracked, 4U);
    EXPECT_EQ(stats.duplicateLogicalEvent, 0U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 4U);

    expectMonsterDeathPending(
        server,
        1001,
        1,
        "MonsterDeath:42:7",
        42,
        7);
    expectLootResolvedPending(
        server,
        1001,
        2,
        "LootResolved:42:7",
        42,
        7,
        1001,
        3001,
        2);
}

TEST(ServerIntegrationTests, RudpGameEventSkipsInvalidShape) {
    Core::Server server(0);
    std::vector<int> disconnectedClients;

    EXPECT_TRUE(Core::ServerTestAccess::broadcastMonsterDeath(
        server,
        monsterDeathResultForTest(0, 7, {1001, 1002}),
        disconnectedClients));
    EXPECT_TRUE(Core::ServerTestAccess::broadcastMonsterDeath(
        server,
        monsterDeathResultForTest(42, 0, {1001, 1002}),
        disconnectedClients));
    EXPECT_TRUE(Core::ServerTestAccess::broadcastMonsterDeath(
        server,
        monsterDeathResultForTest(42, 7, {}),
        disconnectedClients));
    EXPECT_TRUE(Core::ServerTestAccess::broadcastLootResolved(
        server,
        lootResolvedResultForTest(0, 77, 1001, 3001, 2, {1001, 1002}),
        disconnectedClients));
    EXPECT_TRUE(Core::ServerTestAccess::broadcastLootResolved(
        server,
        lootResolvedResultForTest(42, 0, 1001, 3001, 2, {1001, 1002}),
        disconnectedClients));
    EXPECT_TRUE(Core::ServerTestAccess::broadcastLootResolved(
        server,
        lootResolvedResultForTest(42, 77, 1001, 3001, 2, {}),
        disconnectedClients));

    const Core::RudpServerReliableEventStats stats =
        server.rudpReliableEventStats();
    EXPECT_EQ(stats.tracked, 0U);
    EXPECT_EQ(stats.duplicateSequence, 0U);
    EXPECT_EQ(stats.duplicateLogicalEvent, 0U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpReliableEventSessionQueueCount(server), 0U);
    EXPECT_EQ(
        Core::ServerTestAccess::rudpReliableEventSequenceAllocatorCount(server),
        0U);
}

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
