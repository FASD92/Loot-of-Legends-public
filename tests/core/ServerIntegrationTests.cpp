#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Core/MetaSessionClaimClient.hpp"
#include "Core/Server.hpp"
#include "Net/RudpBattleStartPayload.hpp"
#include "Net/RudpBattleStartRosterPayload.hpp"
#include "Net/RudpGameEventPayload.hpp"
#include "Net/RudpHelloPayload.hpp"
#include "Net/RudpInputCommandPayload.hpp"
#include "Net/RudpMetaResponsePayload.hpp"
#include "Net/RudpMoveInputGuard.hpp"
#include "Net/RudpPacket.hpp"
#include "Net/RudpReliableSendQueue.hpp"
#include "Net/RudpStateSnapshotPayload.hpp"
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

    static std::optional<uint32_t> lastAcceptedRudpInputCmdSeq(
        Server& server,
        uint64_t sessionId) {
        return server.rudpInputCommandSequenceTracker_.lastAcceptedCmdSeq(sessionId);
    }

    static size_t rudpMoveInputGuardSize(Server& server) {
        return server.rudpMoveInputGuard_.size();
    }

    static size_t rudpMoveDispatchStateSize(Server& server) {
        return server.rudpMoveDispatchStateBySession_.size();
    }

    static Net::RudpSessionBindResult bindRudpEndpoint(
        Server& server,
        const Net::UdpEndpoint& endpoint,
        uint64_t sessionId) {
        const Net::RudpSessionBindResult result =
            server.rudpSessionBinder_.bind(endpoint, sessionId);
        server.rudpBindingCountSnapshot_.store(
            server.rudpSessionBinder_.size(),
            std::memory_order_relaxed);
        return result;
    }

    static bool bindRudpEndpointWithPeer(
        Server& server,
        const Net::UdpEndpoint& endpoint,
        uint64_t sessionId,
        Util::TimePoint now) {
        Net::RudpPeer* peer = server.rudpPeerRegistry_.findOrCreate(endpoint, now);
        if (peer == nullptr) {
            return false;
        }

        const Net::RudpSessionBindResult result =
            server.rudpSessionBinder_.bind(endpoint, sessionId);
        server.rudpPeerCountSnapshot_.store(
            server.rudpPeerRegistry_.size(),
            std::memory_order_relaxed);
        server.rudpBindingCountSnapshot_.store(
            server.rudpSessionBinder_.size(),
            std::memory_order_relaxed);
        return result == Net::RudpSessionBindResult::kBoundNew ||
            result == Net::RudpSessionBindResult::kRefreshed;
    }

    static void processRudpSocket(Server& server, Util::TimePoint now) {
        server.processRudpSocket(now);
    }

    static void broadcastStateSnapshots(
        Server& server,
        bool clientListChanged,
        bool roomListChanged,
        Util::TimePoint now) {
        server.broadcastStateSnapshots(clientListChanged, roomListChanged, now);
    }

    static void processPendingRoomListSnapshotBroadcast(
        Server& server,
        Util::TimePoint now) {
        server.processPendingRoomListSnapshotBroadcast(now);
    }

    static uint64_t createSession(
        Server& server,
        const std::string& remoteKey,
        Util::TimePoint now) {
        std::shared_ptr<Session> session =
            server.sessionManager_.findOrCreate(remoteKey, now);
        if (!session) {
            return 0;
        }
        server.sessionCountSnapshot_.store(
            server.sessionManager_.size(),
            std::memory_order_relaxed);
        return session->sessionId();
    }

    static Game::RoomCommandResult createRoom(Server& server, uint64_t sessionId) {
        return server.roomManager_.createRoom(sessionId);
    }

    static bool registerRoomForDispatch(Server& server, uint32_t roomId) {
        return server.roomEventDispatcher_.registerRoom(roomId);
    }

    static Game::RoomCommandResult spawnMonster(Server& server, uint32_t roomId) {
        return server.roomManager_.spawnMonster(roomId);
    }

    static Game::RoomCommandResult defeatMonster(
        Server& server,
        uint64_t sessionId,
        uint32_t monsterId) {
        return server.roomManager_.defeatMonster(sessionId, monsterId);
    }

    static Game::RoomCommandResult joinRoom(
        Server& server,
        uint64_t sessionId,
        uint32_t roomId) {
        return server.roomManager_.joinRoom(sessionId, roomId);
    }

    static Game::RoomCommandResult markReady(Server& server, uint64_t sessionId) {
        return server.roomManager_.markReady(sessionId);
    }

    static Game::RoomCommandResult hostStartBattle(
        Server& server,
        uint64_t sessionId) {
        return server.roomManager_.hostStartBattle(sessionId);
    }

    static Game::RoomCommandResult leaveRoom(Server& server, uint64_t sessionId) {
        return server.roomManager_.leaveRoom(sessionId);
    }

    static std::optional<Game::MovementPosition> movementPosition(
        Server& server,
        uint32_t roomId,
        uint64_t sessionId) {
        const Game::Room* room = server.roomManager_.findRoom(roomId);
        if (room == nullptr) {
            return std::nullopt;
        }

        const Game::MovementPosition* position = room->findMovementPosition(sessionId);
        if (position == nullptr) {
            return std::nullopt;
        }
        return *position;
    }

    static Game::RoomEventMetricsSnapshot roomEventMetrics(Server& server) {
        return server.roomEventMetrics_.snapshot();
    }

    static void processRudpInputCommandDelivery(
        Server& server,
        const Net::UdpEndpoint& endpoint,
        uint32_t sequence,
        const std::vector<uint8_t>& payload,
        Util::TimePoint now) {
        Net::RudpPacketDelivery delivery;
        delivery.endpoint = endpoint;
        delivery.header.channelId = static_cast<uint8_t>(Net::RudpChannelId::kInput);
        delivery.header.packetType =
            static_cast<uint16_t>(Net::RudpPacketType::kInputCommand);
        delivery.header.sequence = sequence;
        delivery.header.payloadLen = static_cast<uint16_t>(payload.size());
        delivery.payload = payload;
        std::vector<int> disconnectedClients;
        bool roomListChanged = false;
        server.processRudpAdapterGate(
            delivery,
            now,
            disconnectedClients,
            roomListChanged);

        bool roomChangedByDisconnect = false;
        for (int clientFd : disconnectedClients) {
            roomChangedByDisconnect =
                server.disconnectClient(clientFd) || roomChangedByDisconnect;
        }
        if (!disconnectedClients.empty() || roomListChanged || roomChangedByDisconnect) {
            server.broadcastStateSnapshots(
                !disconnectedClients.empty(),
                roomListChanged || roomChangedByDisconnect);
        }
    }

    static void processRudpHelloDelivery(
        Server& server,
        const Net::UdpEndpoint& endpoint,
        uint32_t sequence,
        const std::vector<uint8_t>& payload,
        Util::TimePoint now) {
        Net::RudpPeer* peer = server.rudpPeerRegistry_.findOrCreate(endpoint, now);
        ASSERT_NE(peer, nullptr);
        server.rudpPeerCountSnapshot_.store(
            server.rudpPeerRegistry_.size(),
            std::memory_order_relaxed);

        Net::RudpPacketDelivery delivery;
        delivery.endpoint = endpoint;
        delivery.header.channelId = static_cast<uint8_t>(Net::RudpChannelId::kControl);
        delivery.header.packetType =
            static_cast<uint16_t>(Net::RudpPacketType::kHello);
        delivery.header.sequence = sequence;
        delivery.header.payloadLen = static_cast<uint16_t>(payload.size());
        delivery.payload = payload;
        server.processRudpHelloDelivery(delivery, now);
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

    static void enqueueRudpBattleStartEvent(
        Server& server,
        const Game::RoomCommandResult& result,
        Util::TimePoint now) {
        server.enqueueRudpBattleStartEvent(result, now);
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
        return server.rudpOutboundNextSequenceBySession_.size();
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

    static std::optional<uint32_t> rudpReliableEventSequence(
        Server& server,
        uint64_t sessionId,
        Net::RudpReliableEventKind kind,
        const std::string& logicalKey) {
        const auto queueIt = server.rudpReliableEventQueues_.find(sessionId);
        if (queueIt == server.rudpReliableEventQueues_.end()) {
            return std::nullopt;
        }

        for (uint32_t sequence : queueIt->second.pendingSequences()) {
            const Net::RudpReliableEventPendingEntry* entry =
                queueIt->second.pendingEntry(sequence);
            if (entry != nullptr &&
                entry->descriptor.kind == kind &&
                entry->descriptor.logicalKey == logicalKey) {
                return sequence;
            }
        }
        return std::nullopt;
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

    static const Game::Room* findRoom(Server& server, uint32_t roomId) {
        return server.roomManager_.findRoom(roomId);
    }

    static bool completeBattleResultsThenBroadcastRoomList(
        Server& server,
        const std::vector<Game::BattleFinalRankingResult>& rankings) {
        std::vector<int> disconnectedClients;
        bool roomListChanged = false;
        bool allSucceeded = true;
        for (const Game::BattleFinalRankingResult& ranking : rankings) {
            allSucceeded =
                server.completeBattleResult(ranking, disconnectedClients, roomListChanged) &&
                allSucceeded;
        }
        if (roomListChanged) {
            allSucceeded =
                server.sendRoomListSnapshotToLobbySessions(disconnectedClients) &&
                allSucceeded;
        }
        return allSucceeded;
    }

    static size_t roomCount(Server& server) {
        return server.roomManager_.roomList().size();
    }

    static bool markRudpReliableEventRetransmitted(
        Server& server,
        uint64_t sessionId,
        uint32_t sequence,
        Util::TimePoint now) {
        const auto queueIt = server.rudpReliableEventQueues_.find(sessionId);
        if (queueIt == server.rudpReliableEventQueues_.end()) {
            return false;
        }
        return queueIt->second.markRetransmitted(sequence, now);
    }

    static void processRudpReliableEventRetransmissions(
        Server& server,
        Util::TimePoint now) {
        server.processRudpReliableEventRetransmissions(now);
    }

    static void processRudpMovementSnapshots(
        Server& server,
        Util::TimePoint now) {
        server.processRudpMovementSnapshots(now);
    }

    static void processRuntimeTimerMaintenanceForRelease1Metrics(
        Server& server,
        Util::TimePoint now) {
        server.processRuntimeTimerMaintenance(now);
    }

    static void processMetaSessionLivenessReports(
        Server& server,
        Util::TimePoint now) {
        server.processMetaSessionLivenessReports(now);
    }

    static size_t rudpSnapshotRoomStateSize(Server& server) {
        return server.rudpSnapshotStateByRoom_.size();
    }

    static int linuxWakeupFd(Server& server) {
        return server.linuxWakeupFd_.load(std::memory_order_acquire);
    }
};
}  // namespace Core

namespace {
Util::TimePoint timeAt(int64_t milliseconds) {
    return Util::TimePoint{std::chrono::milliseconds(milliseconds)};
}

uint64_t unixTimeMsForTest(int64_t offsetMs = 0) {
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    return static_cast<uint64_t>(nowMs + offsetMs);
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

class TestMetaSessionClaimClient final : public Core::IMetaSessionClaimClient {
public:
    explicit TestMetaSessionClaimClient(bool completeImmediately = true)
        : completeImmediately_(completeImmediately) {}

    void claimGameSessionAsync(
        const Core::MetaSessionClaimRequest& request,
        ClaimCallback callback) override {
        Core::MetaSessionClaimResult immediateResult;
        bool completeNow = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            claimRequests_.push_back(request);
            if (!completeImmediately_) {
                pendingClaims_.push_back(PendingClaim{request, std::move(callback)});
                return;
            }

            immediateResult.accepted = true;
            immediateResult.profile.accountId = nextAutoAccountId_++;
            immediateResult.profile.nickname =
                "Player" + std::to_string(immediateResult.profile.accountId);
            immediateResult.reservationExpiresAtUnixMs = unixTimeMsForTest(10'000);
            completeNow = true;
        }

        if (completeNow) {
            callback(std::move(immediateResult));
        }
    }

    void releaseGameSessionAsync(
        const Core::MetaSessionReleaseRequest& request) override {
        std::lock_guard<std::mutex> lock(mutex_);
        releaseRequests_.push_back(request);
    }

    void renewGameSessionAsync(
        const Core::MetaSessionRenewRequest& request) override {
        std::lock_guard<std::mutex> lock(mutex_);
        renewRequests_.push_back(request);
    }

    bool completeClaim(
        const std::string& token,
        Core::MetaSessionClaimResult result) {
        ClaimCallback callback;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto pendingIt = std::find_if(
                pendingClaims_.begin(),
                pendingClaims_.end(),
                [&token](const PendingClaim& pending) {
                    return pending.request.gameSessionToken == token;
                });
            if (pendingIt == pendingClaims_.end()) {
                return false;
            }

            callback = std::move(pendingIt->callback);
            pendingClaims_.erase(pendingIt);
        }

        callback(std::move(result));
        return true;
    }

    std::optional<ClaimCallback> takePendingClaimCallback(
        const std::string& token) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto pendingIt = std::find_if(
            pendingClaims_.begin(),
            pendingClaims_.end(),
            [&token](const PendingClaim& pending) {
                return pending.request.gameSessionToken == token;
            });
        if (pendingIt == pendingClaims_.end()) {
            return std::nullopt;
        }

        ClaimCallback callback = std::move(pendingIt->callback);
        pendingClaims_.erase(pendingIt);
        return callback;
    }

    bool hasPendingClaim(const std::string& token) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::any_of(
            pendingClaims_.begin(),
            pendingClaims_.end(),
            [&token](const PendingClaim& pending) {
                return pending.request.gameSessionToken == token;
            });
    }

    size_t pendingClaimCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return pendingClaims_.size();
    }

    size_t releaseCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return releaseRequests_.size();
    }

    std::vector<Core::MetaSessionReleaseRequest> releaseRequests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return releaseRequests_;
    }

    size_t renewCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return renewRequests_.size();
    }

    std::vector<Core::MetaSessionRenewRequest> renewRequests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return renewRequests_;
    }

private:
    struct PendingClaim {
        Core::MetaSessionClaimRequest request;
        ClaimCallback callback;
    };

    bool completeImmediately_;
    mutable std::mutex mutex_;
    uint64_t nextAutoAccountId_{1000};
    std::vector<Core::MetaSessionClaimRequest> claimRequests_;
    std::vector<PendingClaim> pendingClaims_;
    std::vector<Core::MetaSessionReleaseRequest> releaseRequests_;
    std::vector<Core::MetaSessionRenewRequest> renewRequests_;
};

Core::MetaSessionClaimResult acceptedClaim(uint64_t accountId, const std::string& nickname) {
    Core::MetaSessionClaimResult result;
    result.accepted = true;
    result.profile.accountId = accountId;
    result.profile.nickname = nickname;
    result.reservationExpiresAtUnixMs = unixTimeMsForTest(10'000);
    return result;
}

Core::MetaSessionClaimResult rejectedClaim() {
    Core::MetaSessionClaimResult result;
    result.accepted = false;
    return result;
}

class BlockingMetaSessionClaimClient final : public Core::IMetaSessionClaimClient {
public:
    void claimGameSessionAsync(
        const Core::MetaSessionClaimRequest& request,
        ClaimCallback callback) override {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            claimRequests_.push_back(request);
            ++startedClaims_;
            if (startedClaims_ == 1) {
                firstClaimStarted_ = true;
                condition_.notify_all();
                condition_.wait(lock, [this]() { return unblockFirstClaim_; });
            }
        }

        callback(rejectedClaim());
    }

    void releaseGameSessionAsync(
        const Core::MetaSessionReleaseRequest& request) override {
        std::lock_guard<std::mutex> lock(mutex_);
        releaseRequests_.push_back(request);
    }

    void renewGameSessionAsync(
        const Core::MetaSessionRenewRequest& /*request*/) override {}

    bool waitForFirstClaimStarted(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) {
        std::unique_lock<std::mutex> lock(mutex_);
        return condition_.wait_for(
            lock,
            timeout,
            [this]() { return firstClaimStarted_; });
    }

    void unblockFirstClaim() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            unblockFirstClaim_ = true;
        }
        condition_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool firstClaimStarted_{false};
    bool unblockFirstClaim_{false};
    size_t startedClaims_{0};
    std::vector<Core::MetaSessionClaimRequest> claimRequests_;
    std::vector<Core::MetaSessionReleaseRequest> releaseRequests_;
};

bool sendAll(int fd, const uint8_t* data, size_t size);

int connectRawToServer(uint16_t port) {
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

int connectToServer(uint16_t port) {
    const int fd = connectRawToServer(port);
    if (fd < 0) {
        return -1;
    }

    static std::atomic<uint64_t> nextTokenId{1};
    std::vector<uint8_t> packet;
    const std::string token =
        "integration-token-" + std::to_string(nextTokenId.fetch_add(1));
    if (!Net::serializeAuthenticateGameSessionPacket(token, packet) ||
        !sendAll(fd, packet.data(), packet.size())) {
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

bool sendAll(int fd, const uint8_t* data, size_t size) {
    size_t sentTotal = 0;
    while (sentTotal < size) {
        const ssize_t sent = ::send(fd, data + sentTotal, size - sentTotal, 0);
        if (sent <= 0) {
            return false;
        }
        sentTotal += static_cast<size_t>(sent);
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
    while (true) {
        std::vector<uint8_t> packet;
        if (!recvPacket(fd, packet)) {
            return false;
        }

        Net::TcpPacketHeader header;
        if (!Net::peekTcpPacketHeader(packet.data(), packet.size(), header)) {
            return false;
        }
        if (header.type == Net::TcpPacketType::kRoomListSnapshot) {
            continue;
        }
        return Net::parseClientListSnapshotPacket(
            packet.data(),
            packet.size(),
            header,
            outSessionIds);
    }
}

bool recvCreateRoomResponsePacket(int fd, uint32_t& outRoomId, uint16_t& outPlayerCount) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseCreateRoomResponsePacket(
        packet.data(),
        packet.size(),
        header,
        outRoomId,
        outPlayerCount);
}

bool recvJoinRoomResponsePacket(int fd, uint32_t& outRoomId, uint16_t& outPlayerCount) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseJoinRoomResponsePacket(
        packet.data(),
        packet.size(),
        header,
        outRoomId,
        outPlayerCount);
}

bool recvReadyRoomResponsePacket(
    int fd,
    uint32_t& outRoomId,
    uint16_t& outReadyPlayerCount,
    uint16_t& outTotalPlayerCount) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseReadyRoomResponsePacket(
        packet.data(),
        packet.size(),
        header,
        outRoomId,
        outReadyPlayerCount,
        outTotalPlayerCount);
}

bool recvRoomListSnapshotPacket(int fd, std::vector<Net::TcpRoomEntry>& outRooms) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseRoomListSnapshotPacket(packet.data(), packet.size(), header, outRooms);
}

bool recvRoomDetailStatePacket(int fd, Net::TcpRoomDetailState& outDetail) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseRoomDetailStatePacket(packet.data(), packet.size(), header, outDetail);
}

bool recvHostStartBattleResponsePacket(int fd, uint32_t& outRoomId) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseHostStartBattleResponsePacket(
        packet.data(),
        packet.size(),
        header,
        outRoomId);
}

bool recvBattleStartPacket(
    int fd,
    uint32_t& outRoomId,
    uint64_t& outPlayerASessionId,
    uint64_t& outPlayerBSessionId) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseBattleStartPacket(
        packet.data(),
        packet.size(),
        header,
        outRoomId,
        outPlayerASessionId,
        outPlayerBSessionId);
}

bool recvBattleLoadEntryPacket(
    int fd,
    uint32_t& outRoomId,
    uint64_t& outBattleInstanceId,
    std::vector<uint64_t>& outPlayerSessionIds) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseBattleLoadEntryPacket(
        packet.data(),
        packet.size(),
        header,
        outRoomId,
        outBattleInstanceId,
        outPlayerSessionIds);
}

bool recvArenaGameplayStartPacket(
    int fd,
    uint32_t& outRoomId,
    uint64_t& outBattleInstanceId) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseArenaGameplayStartPacket(
        packet.data(),
        packet.size(),
        header,
        outRoomId,
        outBattleInstanceId);
}

bool recvMonsterSpawnPacket(
    int fd,
    uint32_t& outRoomId,
    uint32_t& outMonsterId,
    uint32_t& outMonsterTypeId,
    uint16_t& outMaxHp) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseMonsterSpawnPacket(
        packet.data(),
        packet.size(),
        header,
        outRoomId,
        outMonsterId,
        outMonsterTypeId,
        outMaxHp);
}

bool recvDropListSnapshotPacket(
    int fd,
    uint32_t& outRoomId,
    std::vector<Net::TcpDropEntry>& outDrops) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseDropListSnapshotPacket(
        packet.data(),
        packet.size(),
        header,
        outRoomId,
        outDrops);
}

bool recvMonsterDeathPacket(
    int fd,
    uint32_t& outRoomId,
    uint32_t& outMonsterId) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseMonsterDeathPacket(
        packet.data(),
        packet.size(),
        header,
        outRoomId,
        outMonsterId);
}

bool recvMonsterHealthSnapshotPacket(
    int fd,
    uint32_t& outRoomId,
    uint32_t& outMonsterId,
    uint16_t& outCurrentHp,
    uint16_t& outMaxHp) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseMonsterHealthSnapshotPacket(
        packet.data(),
        packet.size(),
        header,
        outRoomId,
        outMonsterId,
        outCurrentHp,
        outMaxHp);
}

bool recvDropListSnapshotV2Packet(
    int fd,
    uint32_t& outRoomId,
    uint32_t& outScatterSeed,
    std::vector<Net::TcpDropEntryV2>& outDrops) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseDropListSnapshotV2Packet(
        packet.data(),
        packet.size(),
        header,
        outRoomId,
        outScatterSeed,
        outDrops);
}

bool recvLootResolvedPacket(
    int fd,
    uint32_t& outRoomId,
    uint32_t& outDropId,
    uint64_t& outWinnerSessionId,
    uint32_t& outItemId,
    uint16_t& outQuantity) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseLootResolvedPacket(
        packet.data(),
        packet.size(),
        header,
        outRoomId,
        outDropId,
        outWinnerSessionId,
        outItemId,
        outQuantity);
}

bool recvInventorySnapshotPacket(
    int fd,
    uint64_t& outSessionId,
    uint16_t& outCurrentWeight,
    uint16_t& outMaxWeight,
    std::vector<Net::TcpInventoryEntry>& outEntries) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseInventorySnapshotPacket(
        packet.data(),
        packet.size(),
        header,
        outSessionId,
        outCurrentWeight,
        outMaxWeight,
        outEntries);
}

bool recvBattleFinalRankingPacket(
    int fd,
    uint32_t& outRoomId,
    uint64_t& outBattleInstanceId,
    std::vector<Net::BattleFinalRankingEntry>& outRows) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseBattleFinalRankingPacket(
        packet.data(),
        packet.size(),
        header,
        outRoomId,
        outBattleInstanceId,
        outRows);
}

bool recvLobbyReturnVisibilityPacket(
    int fd,
    uint32_t& outPreviousRoomId,
    Net::TcpLobbyReturnReason& outReason) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseLobbyReturnVisibilityPacket(
        packet.data(),
        packet.size(),
        header,
        outPreviousRoomId,
        outReason);
}

bool recvErrorPacket(int fd, Net::TcpPacketType& outFailedType, Net::TcpErrorCode& outErrorCode) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseErrorPacket(
        packet.data(),
        packet.size(),
        header,
        outFailedType,
        outErrorCode);
}

bool sendCreateRoomRequestPacket(int fd) {
    std::vector<uint8_t> packet;
    if (!Net::serializeCreateRoomRequestPacket("Room", Game::Room::kDefaultMaxPlayers, packet)) {
        return false;
    }
    return sendAll(fd, packet.data(), packet.size());
}

bool sendAuthenticateGameSessionPacket(int fd, const std::string& token) {
    std::vector<uint8_t> packet;
    if (!Net::serializeAuthenticateGameSessionPacket(token, packet)) {
        return false;
    }
    return sendAll(fd, packet.data(), packet.size());
}

bool recvSessionReplacedPacket(int fd) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseTcpPacketHeader(packet.data(), packet.size(), header) &&
        header.type == Net::TcpPacketType::kSessionReplaced &&
        header.size == Net::kTcpHeaderSize;
}

bool sendJoinRoomRequestPacket(int fd, uint32_t roomId) {
    std::array<uint8_t, Net::kRoomIdPacketSize> packet{};
    if (!Net::serializeJoinRoomRequestPacket(roomId, packet)) {
        return false;
    }
    return sendAll(fd, packet.data(), packet.size());
}

bool sendReadyRoomRequestPacket(int fd) {
    std::array<uint8_t, Net::kTcpHeaderSize> packet{};
    if (!Net::serializeReadyRoomRequestPacket(packet)) {
        return false;
    }
    return sendAll(fd, packet.data(), packet.size());
}

bool sendHostStartBattleRequestPacket(int fd) {
    std::array<uint8_t, Net::kTcpHeaderSize> packet{};
    if (!Net::serializeHostStartBattleRequestPacket(packet)) {
        return false;
    }
    return sendAll(fd, packet.data(), packet.size());
}

bool sendArenaLoadCompletePacket(int fd, uint32_t roomId, uint64_t battleInstanceId) {
    std::array<uint8_t, Net::kArenaLoadCompletePacketSize> packet{};
    if (!Net::serializeArenaLoadCompletePacket(roomId, battleInstanceId, packet)) {
        return false;
    }
    return sendAll(fd, packet.data(), packet.size());
}

bool sendClickLootRequestPacket(int fd, uint32_t dropId) {
    std::array<uint8_t, Net::kClickLootRequestPacketSize> packet{};
    if (!Net::serializeClickLootRequestPacket(dropId, packet)) {
        return false;
    }
    return sendAll(fd, packet.data(), packet.size());
}

bool sendSmokeCreateCenterDropRequestPacket(int fd) {
    std::array<uint8_t, Net::kSmokeCreateCenterDropRequestPacketSize> packet{};
    if (!Net::serializeSmokeCreateCenterDropRequestPacket(packet)) {
        return false;
    }
    return sendAll(fd, packet.data(), packet.size());
}

bool sendSmokePlacePlayersAroundCenterDropRequestPacket(int fd) {
    std::array<uint8_t, Net::kSmokePlacePlayersAroundCenterDropRequestPacketSize> packet{};
    if (!Net::serializeSmokePlacePlayersAroundCenterDropRequestPacket(packet)) {
        return false;
    }
    return sendAll(fd, packet.data(), packet.size());
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

void writeU32BEForTest(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset] = static_cast<uint8_t>((value >> 24) & 0xFF);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    bytes[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    bytes[offset + 3] = static_cast<uint8_t>(value & 0xFF);
}

void writeU16BEForTest(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>((value >> 8) & 0xFF);
    bytes[offset + 1] = static_cast<uint8_t>(value & 0xFF);
}

void writeI16BEForTest(std::vector<uint8_t>& bytes, size_t offset, int16_t value) {
    writeU16BEForTest(bytes, offset, static_cast<uint16_t>(value));
}

std::vector<uint8_t> inputCommandPayloadForTest(
    uint32_t playerId,
    uint32_t cmdSeq,
    Net::RudpInputCommandOp op,
    uint32_t argValue) {
    const uint8_t argLen =
        (op == Net::RudpInputCommandOp::kReady ||
            op == Net::RudpInputCommandOp::kSpaceLoot)
            ? 0
            : 4;
    std::vector<uint8_t> payload(Net::kRudpInputCommandPrefixSize + argLen, 0);
    writeU32BEForTest(payload, 0, playerId);
    writeU32BEForTest(payload, 4, cmdSeq);
    payload[8] = static_cast<uint8_t>(op);
    payload[9] = argLen;
    if (argLen == 4) {
        writeU32BEForTest(payload, Net::kRudpInputCommandPrefixSize, argValue);
    }
    return payload;
}

std::vector<uint8_t> moveInputCommandPayloadForTest(
    uint32_t playerId,
    uint32_t cmdSeq,
    int16_t dirX,
    int16_t dirY,
    uint16_t inputFlags = 0) {
    constexpr uint8_t kMoveArgLen = 6;
    std::vector<uint8_t> payload(Net::kRudpInputCommandPrefixSize + kMoveArgLen, 0);
    writeU32BEForTest(payload, 0, playerId);
    writeU32BEForTest(payload, 4, cmdSeq);
    payload[8] = static_cast<uint8_t>(Net::RudpInputCommandOp::kMove);
    payload[9] = kMoveArgLen;
    writeI16BEForTest(payload, Net::kRudpInputCommandPrefixSize, dirX);
    writeI16BEForTest(payload, Net::kRudpInputCommandPrefixSize + 2, dirY);
    writeU16BEForTest(payload, Net::kRudpInputCommandPrefixSize + 4, inputFlags);
    return payload;
}

std::vector<uint8_t> readyInputCommandPayloadForTest(
    uint32_t playerId,
    uint32_t cmdSeq) {
    return inputCommandPayloadForTest(
        playerId,
        cmdSeq,
        Net::RudpInputCommandOp::kReady,
        0);
}

std::vector<uint8_t> monsterDeathInputCommandPayloadForTest(
    uint32_t playerId,
    uint32_t cmdSeq,
    uint32_t monsterId) {
    return inputCommandPayloadForTest(
        playerId,
        cmdSeq,
        Net::RudpInputCommandOp::kMonsterDeath,
        monsterId);
}

std::vector<uint8_t> attackInputCommandPayloadForTest(
    uint32_t playerId,
    uint32_t cmdSeq,
    uint32_t targetHintMonsterId) {
    return inputCommandPayloadForTest(
        playerId,
        cmdSeq,
        Net::RudpInputCommandOp::kAttack,
        targetHintMonsterId);
}

std::vector<uint8_t> clickLootInputCommandPayloadForTest(
    uint32_t playerId,
    uint32_t cmdSeq,
    uint32_t dropId) {
    return inputCommandPayloadForTest(
        playerId,
        cmdSeq,
        Net::RudpInputCommandOp::kClickLoot,
        dropId);
}

std::vector<uint8_t> spaceLootInputCommandPayloadForTest(
    uint32_t playerId,
    uint32_t cmdSeq) {
    return inputCommandPayloadForTest(
        playerId,
        cmdSeq,
        Net::RudpInputCommandOp::kSpaceLoot,
        0);
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
    case Net::RudpReliableEventKind::kBattleStartRoster:
        return Net::RudpReliableEventDescriptor{
            kind,
            logicalKey,
            static_cast<uint16_t>(Net::RudpPacketType::kBattleStartRoster),
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

    Net::RudpPacketHeader packetHeader;
    std::vector<uint8_t> payloadBytes;
    ASSERT_TRUE(Net::parseRudpPacket(
        packetBytes->data(),
        packetBytes->size(),
        packetHeader,
        payloadBytes));
    EXPECT_EQ(packetHeader.flags, Net::kRudpFlagReliable);
    EXPECT_EQ(packetHeader.channelId, static_cast<uint8_t>(Net::RudpChannelId::kEvent));
    EXPECT_EQ(
        packetHeader.packetType,
        static_cast<uint16_t>(Net::RudpPacketType::kBattleStart));
    EXPECT_EQ(packetHeader.sequence, sequence);
    EXPECT_EQ(packetHeader.ack, 0U);
    EXPECT_EQ(packetHeader.ackBits, 0U);

    Net::RudpBattleStartPayload payload;
    ASSERT_TRUE(Net::parseRudpBattleStartPayload(
        payloadBytes.data(),
        payloadBytes.size(),
        payload));
    EXPECT_EQ(payload.roomId, expectedRoomId);
    EXPECT_EQ(payload.playerASessionId, expectedPlayerA);
    EXPECT_EQ(payload.playerBSessionId, expectedPlayerB);
}

void expectBattleStartRosterPending(
    Core::Server& server,
    uint64_t sessionId,
    uint32_t sequence,
    const std::string& expectedLogicalKey,
    uint32_t expectedRoomId,
    const std::vector<uint64_t>& expectedPlayers) {
    const Net::RudpReliableEventPendingEntry* entry =
        Core::ServerTestAccess::rudpReliableEventPendingEntry(
            server,
            sessionId,
            sequence);
    ASSERT_NE(entry, nullptr);
    EXPECT_EQ(entry->descriptor.kind, Net::RudpReliableEventKind::kBattleStartRoster);
    EXPECT_EQ(entry->descriptor.logicalKey, expectedLogicalKey);
    EXPECT_EQ(
        entry->descriptor.packetType,
        static_cast<uint16_t>(Net::RudpPacketType::kBattleStartRoster));
    EXPECT_EQ(
        entry->descriptor.channelId,
        static_cast<uint8_t>(Net::RudpChannelId::kEvent));

    const std::vector<uint8_t>* packetBytes =
        Core::ServerTestAccess::rudpReliableEventPacketBytes(
            server,
            sessionId,
            sequence);
    ASSERT_NE(packetBytes, nullptr);

    Net::RudpPacketHeader packetHeader;
    std::vector<uint8_t> payloadBytes;
    ASSERT_TRUE(Net::parseRudpPacket(
        packetBytes->data(),
        packetBytes->size(),
        packetHeader,
        payloadBytes));
    EXPECT_EQ(packetHeader.flags, Net::kRudpFlagReliable);
    EXPECT_EQ(packetHeader.channelId, static_cast<uint8_t>(Net::RudpChannelId::kEvent));
    EXPECT_EQ(
        packetHeader.packetType,
        static_cast<uint16_t>(Net::RudpPacketType::kBattleStartRoster));
    EXPECT_EQ(packetHeader.sequence, sequence);
    EXPECT_EQ(packetHeader.ack, 0U);
    EXPECT_EQ(packetHeader.ackBits, 0U);

    Net::RudpBattleStartRosterPayload payload;
    ASSERT_TRUE(Net::parseRudpBattleStartRosterPayload(
        payloadBytes.data(),
        payloadBytes.size(),
        payload));
    EXPECT_EQ(payload.roomId, expectedRoomId);
    EXPECT_EQ(payload.playerSessionIds, expectedPlayers);
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

    Net::RudpPacketHeader packetHeader;
    std::vector<uint8_t> payloadBytes;
    ASSERT_TRUE(Net::parseRudpPacket(
        packetBytes->data(),
        packetBytes->size(),
        packetHeader,
        payloadBytes));
    EXPECT_EQ(packetHeader.flags, Net::kRudpFlagReliable);
    EXPECT_EQ(packetHeader.channelId, static_cast<uint8_t>(Net::RudpChannelId::kEvent));
    EXPECT_EQ(
        packetHeader.packetType,
        static_cast<uint16_t>(Net::RudpPacketType::kGameEvent));
    EXPECT_EQ(packetHeader.sequence, sequence);
    EXPECT_EQ(packetHeader.ack, 0U);
    EXPECT_EQ(packetHeader.ackBits, 0U);

    Net::RudpMonsterDeathGameEventPayload payload;
    ASSERT_TRUE(Net::parseRudpMonsterDeathGameEventPayload(
        payloadBytes.data(),
        payloadBytes.size(),
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

    Net::RudpPacketHeader packetHeader;
    std::vector<uint8_t> payloadBytes;
    ASSERT_TRUE(Net::parseRudpPacket(
        packetBytes->data(),
        packetBytes->size(),
        packetHeader,
        payloadBytes));
    EXPECT_EQ(packetHeader.flags, Net::kRudpFlagReliable);
    EXPECT_EQ(packetHeader.channelId, static_cast<uint8_t>(Net::RudpChannelId::kEvent));
    EXPECT_EQ(
        packetHeader.packetType,
        static_cast<uint16_t>(Net::RudpPacketType::kGameEvent));
    EXPECT_EQ(packetHeader.sequence, sequence);
    EXPECT_EQ(packetHeader.ack, 0U);
    EXPECT_EQ(packetHeader.ackBits, 0U);

    Net::RudpLootResolvedGameEventPayload payload;
    ASSERT_TRUE(Net::parseRudpLootResolvedGameEventPayload(
        payloadBytes.data(),
        payloadBytes.size(),
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

    Net::RudpPacketHeader packetHeader;
    std::vector<uint8_t> payloadBytes;
    ASSERT_TRUE(Net::parseRudpPacket(
        packetBytes->data(),
        packetBytes->size(),
        packetHeader,
        payloadBytes));
    EXPECT_EQ(packetHeader.flags, Net::kRudpFlagReliable);
    EXPECT_EQ(packetHeader.channelId, static_cast<uint8_t>(Net::RudpChannelId::kControl));
    EXPECT_EQ(
        packetHeader.packetType,
        static_cast<uint16_t>(Net::RudpPacketType::kMetaResponse));
    EXPECT_EQ(packetHeader.sequence, sequence);
    EXPECT_EQ(packetHeader.ack, 0U);
    EXPECT_EQ(packetHeader.ackBits, 0U);

    Net::RudpMetaResponsePayload payload;
    ASSERT_TRUE(Net::parseRudpMetaResponsePayload(
        payloadBytes.data(),
        payloadBytes.size(),
        payload));
    EXPECT_EQ(payload.op, expectedOp);
    EXPECT_EQ(payload.settlementId, expectedSettlementId);
    EXPECT_EQ(payload.status, expectedStatus);
    EXPECT_EQ(payload.retryAfterMs, expectedRetryAfterMs);
}

void expectNoReliableEventPending(
    Core::Server& server,
    uint64_t sessionId,
    uint32_t sequence) {
    EXPECT_EQ(
        Core::ServerTestAccess::rudpReliableEventPendingEntry(
            server,
            sessionId,
            sequence),
        nullptr);
    EXPECT_EQ(
        Core::ServerTestAccess::rudpReliableEventPacketBytes(
            server,
            sessionId,
            sequence),
        nullptr);
}

void expectMovementPosition(
    const std::optional<Game::MovementPosition>& position,
    int32_t expectedX,
    int32_t expectedY) {
    ASSERT_TRUE(position.has_value());
    EXPECT_EQ(position->x, expectedX);
    EXPECT_EQ(position->y, expectedY);
}

void expectStateSnapshotPlayer(
    const Net::RudpStateSnapshotPayload& payload,
    uint64_t expectedSessionId,
    int32_t expectedX,
    int32_t expectedY) {
    const auto playerIt = std::find_if(
        payload.players.begin(),
        payload.players.end(),
        [expectedSessionId](const Net::RudpStateSnapshotPlayer& player) {
            return player.sessionId == expectedSessionId;
        });
    ASSERT_NE(playerIt, payload.players.end());
    EXPECT_EQ(playerIt->posX, expectedX);
    EXPECT_EQ(playerIt->posY, expectedY);
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

Net::RudpPacketHeader ackOnlyRudpHeader(uint32_t ack, uint32_t ackBits = 0) {
    Net::RudpPacketHeader header;
    header.flags = Net::kRudpFlagAckOnly;
    header.channelId = static_cast<uint8_t>(Net::RudpChannelId::kControl);
    header.packetType = static_cast<uint16_t>(Net::RudpPacketType::kHello);
    header.sequence = 30;
    header.ack = ack;
    header.ackBits = ackBits;
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

bool receiveRudpPacketWithWait(
    Net::UdpSocket& receiver,
    Net::RudpPacketHeader& outHeader,
    std::vector<uint8_t>& outPayload,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::array<uint8_t, Net::kMaxRudpPacketSize> buffer{};
    while (std::chrono::steady_clock::now() < deadline) {
        Net::UdpEndpoint endpoint;
        const ssize_t received =
            receiver.receiveFrom(buffer.data(), buffer.size(), endpoint);
        if (received > 0) {
            return Net::parseRudpPacket(
                buffer.data(),
                static_cast<size_t>(received),
                outHeader,
                outPayload);
        }
        if (received < 0) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool receiveBattleStartPacketWithWait(
    Net::UdpSocket& receiver,
    uint32_t expectedSequence,
    Net::RudpBattleStartPayload& outPayload,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(500),
    uint32_t* outSequence = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const auto slice = std::min(remaining, std::chrono::milliseconds(20));
        Net::RudpPacketHeader header;
        std::vector<uint8_t> payloadBytes;
        if (!receiveRudpPacketWithWait(receiver, header, payloadBytes, slice)) {
            continue;
        }
        if (header.flags != Net::kRudpFlagReliable ||
            header.channelId != static_cast<uint8_t>(Net::RudpChannelId::kEvent) ||
            header.packetType !=
                static_cast<uint16_t>(Net::RudpPacketType::kBattleStart)) {
            continue;
        }
        if (expectedSequence != 0 && header.sequence != expectedSequence) {
            continue;
        }
        if (!Net::parseRudpBattleStartPayload(
                payloadBytes.data(),
                payloadBytes.size(),
                outPayload)) {
            return false;
        }
        if (outSequence != nullptr) {
            *outSequence = header.sequence;
        }
        return true;
    }
    return false;
}

bool receiveBattleStartRosterPacketWithWait(
    Net::UdpSocket& receiver,
    uint32_t expectedSequence,
    Net::RudpBattleStartRosterPayload& outPayload,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const auto slice = std::min(remaining, std::chrono::milliseconds(20));
        Net::RudpPacketHeader header;
        std::vector<uint8_t> payloadBytes;
        if (!receiveRudpPacketWithWait(receiver, header, payloadBytes, slice)) {
            continue;
        }
        if (header.flags != Net::kRudpFlagReliable ||
            header.channelId != static_cast<uint8_t>(Net::RudpChannelId::kEvent) ||
            header.packetType !=
                static_cast<uint16_t>(Net::RudpPacketType::kBattleStartRoster) ||
            header.sequence != expectedSequence) {
            continue;
        }
        return Net::parseRudpBattleStartRosterPayload(
            payloadBytes.data(),
            payloadBytes.size(),
            outPayload);
    }
    return false;
}

bool receiveStateSnapshotPacketWithWait(
    Net::UdpSocket& receiver,
    uint32_t expectedSequence,
    Net::RudpStateSnapshotPayload& outPayload,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) {
    Net::RudpPacketHeader header;
    std::vector<uint8_t> payloadBytes;
    if (!receiveRudpPacketWithWait(receiver, header, payloadBytes, timeout)) {
        return false;
    }
    if (header.flags != 0 ||
        header.channelId != static_cast<uint8_t>(Net::RudpChannelId::kSnapshot) ||
        header.packetType !=
            static_cast<uint16_t>(Net::RudpPacketType::kStateSnapshot) ||
        header.sequence != expectedSequence) {
        return false;
    }
    return Net::parseRudpStateSnapshotPayload(
        payloadBytes.data(),
        payloadBytes.size(),
        outPayload);
}

bool receiveMonsterDeathPacketWithWait(
    Net::UdpSocket& receiver,
    uint32_t expectedSequence,
    Net::RudpMonsterDeathGameEventPayload& outPayload,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const auto slice = std::min(remaining, std::chrono::milliseconds(20));
        Net::RudpPacketHeader header;
        std::vector<uint8_t> payloadBytes;
        if (!receiveRudpPacketWithWait(receiver, header, payloadBytes, slice)) {
            continue;
        }
        if (header.flags != Net::kRudpFlagReliable ||
            header.channelId != static_cast<uint8_t>(Net::RudpChannelId::kEvent) ||
            header.packetType != static_cast<uint16_t>(Net::RudpPacketType::kGameEvent) ||
            header.sequence != expectedSequence) {
            continue;
        }
        return Net::parseRudpMonsterDeathGameEventPayload(
            payloadBytes.data(),
            payloadBytes.size(),
            outPayload);
    }
    return false;
}

bool receiveLootResolvedPacketWithWait(
    Net::UdpSocket& receiver,
    uint32_t expectedSequence,
    Net::RudpLootResolvedGameEventPayload& outPayload,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(500)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const auto slice = std::min(remaining, std::chrono::milliseconds(20));
        Net::RudpPacketHeader header;
        std::vector<uint8_t> payloadBytes;
        if (!receiveRudpPacketWithWait(receiver, header, payloadBytes, slice)) {
            continue;
        }
        if (header.flags != Net::kRudpFlagReliable ||
            header.channelId != static_cast<uint8_t>(Net::RudpChannelId::kEvent) ||
            header.packetType != static_cast<uint16_t>(Net::RudpPacketType::kGameEvent) ||
            header.sequence != expectedSequence) {
            continue;
        }
        return Net::parseRudpLootResolvedGameEventPayload(
            payloadBytes.data(),
            payloadBytes.size(),
            outPayload);
    }
    return false;
}

void expectSnapshotEquals(
    const std::vector<uint64_t>& actual,
    const std::vector<uint64_t>& expected) {
    EXPECT_EQ(actual, expected);
}

struct RunningServer {
    explicit RunningServer(uint16_t port) : meta(), server(port, &meta) {}
    RunningServer(uint16_t port, Core::IMetaSessionClaimClient* metaClient)
        : meta(), server(port, metaClient) {}
    RunningServer(uint16_t port, std::chrono::milliseconds rudpPeerTimeout)
        : meta(), server(port, rudpPeerTimeout, &meta) {}

    ~RunningServer() {
        server.requestStop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    TestMetaSessionClaimClient meta;
    Core::Server server;
    std::thread thread;
};

struct TwoPlayerRoomClients {
    int clientA{-1};
    int clientB{-1};
    uint64_t sessionA{0};
    uint64_t sessionB{0};
    uint32_t roomId{0};
    uint32_t battleStartSequenceA{0};
    uint32_t battleStartSequenceB{0};
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

::testing::AssertionResult setupTwoPlayerRoomForRudpTest(
    RunningServer& runningServer,
    TwoPlayerRoomClients& out) {
    const uint16_t tcpPort = runningServer.server.boundPort();
    if (tcpPort == 0) {
        return ::testing::AssertionFailure() << "server TCP port is not bound";
    }

    out.clientA = connectToServer(tcpPort);
    if (out.clientA < 0) {
        return ::testing::AssertionFailure() << "failed to connect TCP client A";
    }
    if (!setReceiveTimeout(out.clientA, std::chrono::milliseconds(500))) {
        return ::testing::AssertionFailure() << "failed to set TCP receive timeout";
    }

    std::vector<uint64_t> clientSnapshot;
    if (!recvWelcomePacket(out.clientA, out.sessionA) ||
        !recvClientListSnapshotPacket(out.clientA, clientSnapshot)) {
        return ::testing::AssertionFailure() << "failed to receive client A welcome/snapshot";
    }

    out.clientB = connectToServer(tcpPort);
    if (out.clientB < 0) {
        return ::testing::AssertionFailure() << "failed to connect TCP client B";
    }
    if (!setReceiveTimeout(out.clientB, std::chrono::milliseconds(500))) {
        return ::testing::AssertionFailure() << "failed to set TCP receive timeout";
    }

    if (!recvWelcomePacket(out.clientB, out.sessionB) ||
        !recvClientListSnapshotPacket(out.clientB, clientSnapshot) ||
        !recvClientListSnapshotPacket(out.clientA, clientSnapshot)) {
        return ::testing::AssertionFailure() << "failed to receive client B welcome/snapshots";
    }

    if (!sendCreateRoomRequestPacket(out.clientA)) {
        return ::testing::AssertionFailure() << "failed to send create room";
    }
    uint16_t playerCount = 0;
    if (!recvCreateRoomResponsePacket(out.clientA, out.roomId, playerCount) ||
        out.roomId == 0 ||
        playerCount != 1u) {
        return ::testing::AssertionFailure() << "unexpected create room response";
    }

    Net::TcpRoomDetailState detail;
    std::vector<Net::TcpRoomEntry> roomSnapshot;
    if (!recvRoomDetailStatePacket(out.clientA, detail) ||
        detail.roomId != out.roomId ||
        !recvRoomListSnapshotPacket(out.clientB, roomSnapshot)) {
        return ::testing::AssertionFailure() << "failed to receive create room snapshots";
    }

    if (!sendJoinRoomRequestPacket(out.clientB, out.roomId)) {
        return ::testing::AssertionFailure() << "failed to send join room";
    }
    uint32_t joinedRoomId = 0;
    if (!recvJoinRoomResponsePacket(out.clientB, joinedRoomId, playerCount) ||
        joinedRoomId != out.roomId ||
        playerCount != 2u) {
        return ::testing::AssertionFailure() << "unexpected join room response";
    }
    if (!recvRoomDetailStatePacket(out.clientA, detail) ||
        detail.roomId != out.roomId ||
        detail.members.size() != 2U ||
        !recvRoomDetailStatePacket(out.clientB, detail) ||
        detail.roomId != out.roomId ||
        detail.members.size() != 2U) {
        return ::testing::AssertionFailure() << "failed to receive join room snapshots";
    }

    return ::testing::AssertionSuccess();
}

::testing::AssertionResult bindRudpEndpointForSessionTest(
    RunningServer& runningServer,
    Net::UdpSocket& sender,
    uint64_t sessionId,
    uint32_t clientId,
    uint32_t sequence,
    size_t expectedBindingCount) {
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    if (udpPort == 0) {
        return ::testing::AssertionFailure() << "server UDP port is not bound";
    }

    const std::vector<uint8_t> helloPacket =
        serializeRudpHelloPacketForTest(sequence, 1, clientId, sessionId);
    if (!sender.sendTo(
            helloPacket.data(),
            helloPacket.size(),
            udpLoopbackEndpoint(udpPort))) {
        return ::testing::AssertionFailure() << "failed to send RUDP Hello";
    }

    if (!waitUntil(
            [&runningServer, expectedBindingCount]() {
                return runningServer.server.rudpBindingCount() >= expectedBindingCount;
            })) {
        return ::testing::AssertionFailure() << "RUDP binding count did not reach "
                                             << expectedBindingCount;
    }

    return ::testing::AssertionSuccess();
}

void expectNoTcpPacketForTest(int fd) {
    ASSERT_TRUE(setReceiveTimeout(fd, std::chrono::milliseconds(50)));
    std::vector<uint8_t> unexpectedPacket;
    EXPECT_FALSE(recvPacket(fd, unexpectedPacket));
}

void drainAvailableTcpPacketsForTest(int fd) {
    ASSERT_TRUE(setReceiveTimeout(fd, std::chrono::milliseconds(50)));
    std::vector<uint8_t> packet;
    while (recvPacket(fd, packet)) {
    }
    ASSERT_TRUE(setReceiveTimeout(fd, std::chrono::milliseconds(500)));
}

::testing::AssertionResult startRudpBattleForTest(
    RunningServer& runningServer,
    TwoPlayerRoomClients& clients,
    Net::UdpSocket& senderA,
    Net::UdpSocket& senderB,
    uint16_t udpPort) {
    sendUdpPacket(
        senderA,
        serializeRudpPacketForTest(
            inputCommandRudpHeader(201),
            readyInputCommandPayloadForTest(77, 1)),
        udpPort);
    sendUdpPacket(
        senderB,
        serializeRudpPacketForTest(
            inputCommandRudpHeader(202),
            readyInputCommandPayloadForTest(78, 1)),
        udpPort);

    if (!waitUntil(
            [&runningServer, &clients]() {
                const Core::RudpServerBindingStats stats =
                    runningServer.server.rudpBindingStats();
                const Game::Room* room = Core::ServerTestAccess::findRoom(
                    runningServer.server,
                    clients.roomId);
                return stats.inputDecoded >= 2U &&
                    stats.inputSequenceAccepted >= 2U &&
                    runningServer.server.rudpReliableEventPendingCount() == 0U &&
                    room != nullptr &&
                    room->isReady(clients.sessionA) &&
                    room->isReady(clients.sessionB) &&
                    !room->battleStarted();
            })) {
        return ::testing::AssertionFailure() << "RUDP Ready state was not applied";
    }

    if (!sendHostStartBattleRequestPacket(clients.clientA)) {
        return ::testing::AssertionFailure() << "failed to send HostStartBattleRequest";
    }

    if (!waitUntil(
            [&runningServer, &clients]() {
                const Game::Room* room = Core::ServerTestAccess::findRoom(
                    runningServer.server,
                    clients.roomId);
                return room != nullptr &&
                    room->battleStarted() &&
                    room->hasActiveArenaLoadBarrier() &&
                    !room->arenaGameplayStarted() &&
                    !room->hasAliveMonster();
            })) {
        return ::testing::AssertionFailure() << "HostStart did not create RUDP BattleStart";
    }

    const Game::Room* room = Core::ServerTestAccess::findRoom(
        runningServer.server,
        clients.roomId);
    if (room == nullptr ||
        !room->battleStarted() ||
        !room->hasActiveArenaLoadBarrier() ||
        room->arenaGameplayStarted() ||
        room->hasAliveMonster()) {
        return ::testing::AssertionFailure() << "battle did not start";
    }
    const uint64_t battleInstanceId = room->battleInstanceId();

    Net::RudpBattleStartPayload battleStartA;
    Net::RudpBattleStartPayload battleStartB;
    uint32_t sequenceA = 0;
    uint32_t sequenceB = 0;
    if (!receiveBattleStartPacketWithWait(
            senderA,
            0,
            battleStartA,
            std::chrono::milliseconds(500),
            &sequenceA) ||
        !receiveBattleStartPacketWithWait(
            senderB,
            0,
            battleStartB,
            std::chrono::milliseconds(500),
            &sequenceB)) {
        return ::testing::AssertionFailure() << "RUDP BattleStart packet was not sent";
    }
    if (battleStartA.roomId != clients.roomId || battleStartB.roomId != clients.roomId) {
        return ::testing::AssertionFailure() << "RUDP BattleStart packet used wrong room";
    }
    clients.battleStartSequenceA = sequenceA;
    clients.battleStartSequenceB = sequenceB;

    if (!sendArenaLoadCompletePacket(clients.clientA, clients.roomId, battleInstanceId) ||
        !sendArenaLoadCompletePacket(clients.clientB, clients.roomId, battleInstanceId)) {
        return ::testing::AssertionFailure() << "failed to send ArenaLoadComplete packets";
    }

    if (!waitUntil(
            [&runningServer, &clients]() {
                const Game::Room* startedRoom = Core::ServerTestAccess::findRoom(
                    runningServer.server,
                    clients.roomId);
                return startedRoom != nullptr &&
                    !startedRoom->hasActiveArenaLoadBarrier() &&
                    startedRoom->arenaGameplayStarted() &&
                    startedRoom->hasAliveMonster();
            })) {
        return ::testing::AssertionFailure() << "ArenaGameplayStart did not spawn monster";
    }

    drainAvailableTcpPacketsForTest(clients.clientA);
    drainAvailableTcpPacketsForTest(clients.clientB);

    return ::testing::AssertionSuccess();
}

::testing::AssertionResult prepareRudpBattleForSmokeEndpointTest(
    RunningServer& runningServer,
    TwoPlayerRoomClients& clients,
    Net::UdpSocket& senderA,
    Net::UdpSocket& senderB) {
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    if (udpPort == 0) {
        return ::testing::AssertionFailure() << "server UDP port is not bound";
    }

    const ::testing::AssertionResult setupResult =
        setupTwoPlayerRoomForRudpTest(runningServer, clients);
    if (!setupResult) {
        return setupResult;
    }

    if (!senderA.open(0) || !senderB.open(0)) {
        return ::testing::AssertionFailure() << "failed to open RUDP smoke senders";
    }

    const ::testing::AssertionResult bindAResult = bindRudpEndpointForSessionTest(
        runningServer,
        senderA,
        clients.sessionA,
        77,
        100,
        1);
    if (!bindAResult) {
        return bindAResult;
    }

    const ::testing::AssertionResult bindBResult = bindRudpEndpointForSessionTest(
        runningServer,
        senderB,
        clients.sessionB,
        78,
        101,
        2);
    if (!bindBResult) {
        return bindBResult;
    }

    return startRudpBattleForTest(runningServer, clients, senderA, senderB, udpPort);
}

::testing::AssertionResult expectCenterDropSnapshot(
    int clientFd,
    uint32_t expectedRoomId,
    uint32_t& outDropId,
    uint32_t& outItemId,
    uint16_t& outQuantity) {
    uint32_t roomId = 0;
    std::vector<Net::TcpDropEntry> drops;
    if (!recvDropListSnapshotPacket(clientFd, roomId, drops)) {
        return ::testing::AssertionFailure() << "failed to receive DropListSnapshot";
    }
    if (roomId != expectedRoomId || drops.size() != 1u) {
        return ::testing::AssertionFailure()
               << "unexpected DropListSnapshot roomId=" << roomId
               << " dropCount=" << drops.size();
    }

    outDropId = drops[0].dropId;
    outItemId = drops[0].itemId;
    outQuantity = drops[0].quantity;
    return ::testing::AssertionSuccess();
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
    const std::string textfile = server.renderRelease1RuntimeMetricsTextfile();
    EXPECT_NE(
        textfile.find(
            "lol_rudp_reliable_event_tracked_by_kind_total{kind=\"battle_start\"} 1\n"),
        std::string::npos);
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
        2,
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

TEST(ServerIntegrationTests, RudpReliableGameplayEventsRequireBoundEndpoint) {
    Core::Server server(0);
    std::vector<int> disconnectedClients;

    EXPECT_TRUE(Core::ServerTestAccess::broadcastBattleStart(
        server,
        battleStartResultForTest(42, {1002, 1001}),
        disconnectedClients));
    EXPECT_TRUE(Core::ServerTestAccess::broadcastMonsterDeath(
        server,
        monsterDeathResultForTest(42, 7, {1001, 1002}),
        disconnectedClients));
    EXPECT_TRUE(Core::ServerTestAccess::broadcastLootResolved(
        server,
        lootResolvedResultForTest(42, 77, 1001, 3001, 2, {1001, 1002}),
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
    EXPECT_TRUE(disconnectedClients.empty());
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

TEST(ServerIntegrationTests, RudpBattleStartRosterReliableEventTracksThreePlayerRoom) {
    Core::Server server(0);
    ASSERT_TRUE(server.start());

    Net::UdpSocket receiverA;
    Net::UdpSocket receiverB;
    Net::UdpSocket receiverC;
    ASSERT_TRUE(receiverA.open(0));
    ASSERT_TRUE(receiverB.open(0));
    ASSERT_TRUE(receiverC.open(0));

    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(Core::ServerTestAccess::joinRoom(
        server,
        1002,
        created.room.roomId).ok);
    ASSERT_TRUE(Core::ServerTestAccess::joinRoom(
        server,
        1003,
        created.room.roomId).ok);
    ASSERT_TRUE(Core::ServerTestAccess::bindRudpEndpointWithPeer(
        server,
        udpLoopbackEndpoint(receiverA.boundPort()),
        1001,
        timeAt(1000)));
    ASSERT_TRUE(Core::ServerTestAccess::bindRudpEndpointWithPeer(
        server,
        udpLoopbackEndpoint(receiverB.boundPort()),
        1002,
        timeAt(1000)));
    ASSERT_TRUE(Core::ServerTestAccess::bindRudpEndpointWithPeer(
        server,
        udpLoopbackEndpoint(receiverC.boundPort()),
        1003,
        timeAt(1000)));

    std::vector<int> disconnectedClients;
    const std::vector<uint64_t> players{1001, 1002, 1003};
    EXPECT_TRUE(Core::ServerTestAccess::broadcastBattleStart(
        server,
        battleStartResultForTest(created.room.roomId, players),
        disconnectedClients));

    EXPECT_TRUE(disconnectedClients.empty());
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 3U);
    const std::string key =
        "BattleStartRoster:" + std::to_string(created.room.roomId) + ":1001:1002:1003";
    expectBattleStartRosterPending(server, 1001, 1, key, created.room.roomId, players);
    expectBattleStartRosterPending(server, 1002, 1, key, created.room.roomId, players);
    expectBattleStartRosterPending(server, 1003, 1, key, created.room.roomId, players);
}

TEST(ServerIntegrationTests, RudpBattleStartRosterPendingEventSendsAfterLateBind) {
    Core::Server server(0);
    ASSERT_TRUE(server.start());

    Net::UdpSocket receiverA;
    Net::UdpSocket receiverB;
    ASSERT_TRUE(receiverA.open(0));
    ASSERT_TRUE(receiverB.open(0));

    const uint64_t sessionA =
        Core::ServerTestAccess::createSession(server, "late-bind-A", timeAt(900));
    const uint64_t sessionB =
        Core::ServerTestAccess::createSession(server, "late-bind-B", timeAt(900));
    const uint64_t sessionC =
        Core::ServerTestAccess::createSession(server, "late-bind-C", timeAt(900));
    ASSERT_NE(sessionA, 0U);
    ASSERT_NE(sessionB, 0U);
    ASSERT_NE(sessionC, 0U);

    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, sessionA);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(Core::ServerTestAccess::joinRoom(
        server,
        sessionB,
        created.room.roomId).ok);
    ASSERT_TRUE(Core::ServerTestAccess::joinRoom(
        server,
        sessionC,
        created.room.roomId).ok);
    ASSERT_TRUE(Core::ServerTestAccess::bindRudpEndpointWithPeer(
        server,
        udpLoopbackEndpoint(receiverA.boundPort()),
        sessionA,
        timeAt(1000)));

    const std::vector<uint64_t> players{sessionA, sessionB, sessionC};
    Core::ServerTestAccess::enqueueRudpBattleStartEvent(
        server,
        battleStartResultForTest(created.room.roomId, players),
        timeAt(1000));

    const std::string key =
        "BattleStartRoster:" + std::to_string(created.room.roomId) + ":" +
        std::to_string(sessionA) + ":" + std::to_string(sessionB) + ":" +
        std::to_string(sessionC);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 3U);
    expectBattleStartRosterPending(server, sessionB, 1, key, created.room.roomId, players);

    Core::ServerTestAccess::processRudpReliableEventRetransmissions(
        server,
        timeAt(1300));
    EXPECT_EQ(server.rudpRetransmissionStats().due, 3U);
    EXPECT_EQ(server.rudpRetransmissionStats().resent, 1U);
    EXPECT_EQ(server.rudpRetransmissionStats().sendErrors, 0U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 3U);

    std::vector<uint8_t> helloPayload;
    ASSERT_TRUE(Net::serializeRudpHelloPayload(
        Net::RudpHelloPayload{1, 78, sessionB},
        helloPayload));
    Core::ServerTestAccess::processRudpHelloDelivery(
        server,
        udpLoopbackEndpoint(receiverB.boundPort()),
        101,
        helloPayload,
        timeAt(1010));

    Net::RudpBattleStartRosterPayload replayed;
    ASSERT_TRUE(receiveBattleStartRosterPacketWithWait(receiverB, 1, replayed));
    EXPECT_EQ(replayed.roomId, created.room.roomId);
    EXPECT_EQ(replayed.playerSessionIds, players);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 3U);
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

#if defined(__linux__)
TEST(ServerIntegrationTests, RequestStopWakesIdleLinuxEpollLoop) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    std::atomic<bool> runReturned{false};
    runningServer.thread = std::thread([&runningServer, &runReturned]() {
        runningServer.server.run();
        runReturned.store(true, std::memory_order_release);
    });
    ASSERT_TRUE(waitUntil([&runningServer]() {
        return Core::ServerTestAccess::linuxWakeupFd(runningServer.server) >= 0;
    }));

    const auto stopRequestedAt = std::chrono::steady_clock::now();
    runningServer.server.requestStop();
    const bool stopped = waitUntil(
        [&runReturned]() {
            return runReturned.load(std::memory_order_acquire);
        },
        std::chrono::milliseconds(200));
    if (!stopped) {
        Net::UdpSocket sender;
        ASSERT_TRUE(sender.open(0));
        sendUdpPacket(sender, {0x00}, udpPort);
    }
    ASSERT_TRUE(waitUntil([&runReturned]() {
        return runReturned.load(std::memory_order_acquire);
    }));
    ASSERT_TRUE(runningServer.thread.joinable());
    runningServer.thread.join();

    const auto elapsed = std::chrono::steady_clock::now() - stopRequestedAt;
    EXPECT_LT(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed),
        std::chrono::milliseconds(200));
    EXPECT_EQ(runningServer.server.udpBoundPort(), 0);
    EXPECT_EQ(Core::ServerTestAccess::linuxWakeupFd(runningServer.server), -1);
}
#endif

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

TEST(ServerIntegrationTests, RudpSocketDrainBudgetLeavesFortyNinthDatagramForNextPass) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    Net::UdpSocket sender;
    ASSERT_TRUE(sender.open(0));
    for (uint32_t sequence = 1; sequence <= 49; ++sequence) {
        sendUdpPacket(
            sender,
            serializeRudpPacketForTest(reliableRudpHeader(1000 + sequence), {0x01}),
            udpPort);
    }

    Core::ServerTestAccess::processRudpSocket(runningServer.server, timeAt(1000));

    Core::RudpServerDrainStats stats = runningServer.server.rudpDrainStats();
    EXPECT_EQ(stats.delivered, 48U);
    EXPECT_EQ(stats.stoppedByMaxPackets, 1U);
    EXPECT_EQ(stats.stoppedByWouldBlock, 0U);

    Core::ServerTestAccess::processRudpSocket(runningServer.server, timeAt(1010));

    stats = runningServer.server.rudpDrainStats();
    EXPECT_GE(stats.delivered, 49U);
    EXPECT_GE(stats.stoppedByWouldBlock, 1U);
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
                runningServer.server.rudpBindingCount() == 0U &&
                runningServer.server.rudpReliableEventPendingCount() == 0U;
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
                runningServer.server.rudpBindingCount() == 1U &&
                runningServer.server.rudpReliableEventPendingCount() == 0U;
        }));

    expectNoTcpPacketForTest(clientFd);

    ::close(clientFd);
}

TEST(ServerIntegrationTests, TracksRudpInputSequenceByBoundSessionNotPayloadPlayerId) {
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
            readyInputCommandPayloadForTest(999, 10)),
        udpPort);

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerBindingStats stats =
                runningServer.server.rudpBindingStats();
            return stats.inputDecoded >= 2U &&
                stats.inputSequenceAccepted == 1U &&
                stats.inputSequenceDuplicateRejected >= 1U &&
                stats.inputSequenceStaleRejected == 0U &&
                stats.inputSequenceAmbiguousRejected == 0U &&
                runningServer.server.rudpReliableEventPendingCount() == 0U;
        }));

    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(50)));
    std::vector<uint8_t> unexpectedPacket;
    EXPECT_FALSE(recvPacket(clientFd, unexpectedPacket));

    ::close(clientFd);
}

TEST(ServerIntegrationTests, RejectsUnboundMoveBeforeDecodeAndSequenceState) {
    Core::Server server(0);
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(31000);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        101,
        moveInputCommandPayloadForTest(77, 1, 100, -100),
        timeAt(1000));

    const Core::RudpServerBindingStats stats = server.rudpBindingStats();
    EXPECT_EQ(stats.unboundInputRejected, 1U);
    EXPECT_EQ(stats.inputCandidates, 0U);
    EXPECT_EQ(stats.inputDecoded, 0U);
    EXPECT_EQ(stats.inputSequenceAccepted, 0U);
    EXPECT_EQ(stats.inputNoRoomRejected, 0U);
    EXPECT_EQ(stats.moveAccepted, 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpInputCommandSequenceTrackerSize(server), 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMoveInputGuardSize(server), 0U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
}

TEST(ServerIntegrationTests, BoundRudpGameplayInputRefreshesSessionLiveness) {
    Core::Server server(0);
    const Util::TimePoint createdAt = timeAt(1000);
    const uint64_t sessionId =
        Core::ServerTestAccess::createSession(server, "127.0.0.1:49001", createdAt);
    ASSERT_NE(sessionId, 0U);
    ASSERT_TRUE(Core::ServerTestAccess::createRoom(server, sessionId).ok);

    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(31905);
    ASSERT_EQ(
        Core::ServerTestAccess::bindRudpEndpoint(server, endpoint, sessionId),
        Net::RudpSessionBindResult::kBoundNew);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        1,
        moveInputCommandPayloadForTest(1001, 1, 100, 0),
        createdAt + std::chrono::milliseconds(59000));

    Core::ServerTestAccess::processRuntimeTimerMaintenanceForRelease1Metrics(
        server,
        createdAt + std::chrono::milliseconds(118000));

    EXPECT_EQ(server.release1RuntimeMetricsSnapshot().activeSessionCount, 1U);
}

TEST(ServerIntegrationTests, BoundNoRoomMoveRecordsSequenceWithoutOutbound) {
    Core::Server server(0);
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(31001);
    ASSERT_EQ(
        Core::ServerTestAccess::bindRudpEndpoint(server, endpoint, 1001),
        Net::RudpSessionBindResult::kBoundNew);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        101,
        moveInputCommandPayloadForTest(77, 10, 100, -100),
        timeAt(1000));

    const Core::RudpServerBindingStats stats = server.rudpBindingStats();
    EXPECT_EQ(stats.inputSequenceAccepted, 1U);
    EXPECT_EQ(stats.inputNoRoomRejected, 1U);
    EXPECT_EQ(stats.moveAccepted, 0U);
    EXPECT_EQ(stats.moveDispatched, 0U);
    EXPECT_EQ(stats.moveApplyRejected, 0U);
    EXPECT_EQ(stats.moveInvalidReservedFlagsRejected, 0U);
    EXPECT_EQ(stats.moveRateLimitedRejected, 0U);
    ASSERT_TRUE(Core::ServerTestAccess::lastAcceptedRudpInputCmdSeq(
        server,
        1001).has_value());
    EXPECT_EQ(
        *Core::ServerTestAccess::lastAcceptedRudpInputCmdSeq(server, 1001),
        10U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMoveInputGuardSize(server), 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMoveDispatchStateSize(server), 0U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
    const Game::RoomEventMetricsSnapshot metrics =
        Core::ServerTestAccess::roomEventMetrics(server);
    EXPECT_EQ(metrics.roomEventEnqueuedCount, 0U);
    EXPECT_EQ(metrics.roomEventProcessedCount, 0U);
}

TEST(ServerIntegrationTests, MoveDuplicateStaleAmbiguousDropBeforeMoveGuard) {
    Core::Server server(0);
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(31002);
    ASSERT_EQ(
        Core::ServerTestAccess::bindRudpEndpoint(server, endpoint, 1001),
        Net::RudpSessionBindResult::kBoundNew);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        101,
        moveInputCommandPayloadForTest(77, 10, 100, -100),
        timeAt(1000));
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        102,
        moveInputCommandPayloadForTest(77, 10, 100, -100),
        timeAt(1000));
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        103,
        moveInputCommandPayloadForTest(77, 12, 100, -100),
        timeAt(1000));
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        104,
        moveInputCommandPayloadForTest(77, 11, 100, -100),
        timeAt(1000));
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        105,
        moveInputCommandPayloadForTest(77, 12U + 0x80000000U, 100, -100),
        timeAt(1000));

    const Core::RudpServerBindingStats stats = server.rudpBindingStats();
    EXPECT_EQ(stats.inputSequenceAccepted, 2U);
    EXPECT_EQ(stats.inputNoRoomRejected, 2U);
    EXPECT_EQ(stats.inputSequenceDuplicateRejected, 1U);
    EXPECT_EQ(stats.inputSequenceStaleRejected, 1U);
    EXPECT_EQ(stats.inputSequenceAmbiguousRejected, 1U);
    EXPECT_EQ(stats.moveAccepted, 0U);
    EXPECT_EQ(stats.moveDispatched, 0U);
    EXPECT_EQ(stats.moveApplyRejected, 0U);
    EXPECT_EQ(stats.moveInvalidReservedFlagsRejected, 0U);
    EXPECT_EQ(stats.moveRateLimitedRejected, 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMoveInputGuardSize(server), 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMoveDispatchStateSize(server), 0U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
}

TEST(ServerIntegrationTests, MoveSequenceRejectsDoNotUpdateMovementClockOrPosition) {
    Core::Server server(0);
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(31008);
    ASSERT_EQ(
        Core::ServerTestAccess::bindRudpEndpoint(server, endpoint, 1001),
        Net::RudpSessionBindResult::kBoundNew);
    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        101,
        moveInputCommandPayloadForTest(77, 10, 1000, 0),
        timeAt(1000));
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        102,
        moveInputCommandPayloadForTest(77, 10, 0, 1000),
        timeAt(1500));
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        103,
        moveInputCommandPayloadForTest(77, 12, 0, 0),
        timeAt(2000));

    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(server, created.room.roomId, 1001),
        0,
        0);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        104,
        moveInputCommandPayloadForTest(77, 11, 0, 1000),
        timeAt(2500));
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        105,
        moveInputCommandPayloadForTest(77, 12U + 0x80000000U, 0, 1000),
        timeAt(3000));
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        106,
        moveInputCommandPayloadForTest(77, 13, 1000, 0),
        timeAt(3500));

    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(server, created.room.roomId, 1001),
        0,
        0);

    const Core::RudpServerBindingStats stats = server.rudpBindingStats();
    EXPECT_EQ(stats.inputSequenceAccepted, 3U);
    EXPECT_EQ(stats.inputSequenceDuplicateRejected, 1U);
    EXPECT_EQ(stats.inputSequenceStaleRejected, 1U);
    EXPECT_EQ(stats.inputSequenceAmbiguousRejected, 1U);
    EXPECT_EQ(stats.moveAccepted, 3U);
    EXPECT_EQ(stats.moveDispatched, 3U);
    EXPECT_EQ(stats.moveApplyRejected, 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMoveDispatchStateSize(server), 1U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
}

TEST(ServerIntegrationTests, InvalidMoveFlagsDropWithoutSequenceRollbackOrOutbound) {
    Core::Server server(0);
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(31003);
    ASSERT_EQ(
        Core::ServerTestAccess::bindRudpEndpoint(server, endpoint, 1001),
        Net::RudpSessionBindResult::kBoundNew);
    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);
    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(server, created.room.roomId, 1001),
        -1000,
        0);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        101,
        moveInputCommandPayloadForTest(77, 20, 100, -100, 1),
        timeAt(1000));

    const Core::RudpServerBindingStats stats = server.rudpBindingStats();
    EXPECT_EQ(stats.inputSequenceAccepted, 1U);
    EXPECT_EQ(stats.inputNoRoomRejected, 0U);
    EXPECT_EQ(stats.moveAccepted, 0U);
    EXPECT_EQ(stats.moveDispatched, 0U);
    EXPECT_EQ(stats.moveApplyRejected, 0U);
    EXPECT_EQ(stats.moveInvalidReservedFlagsRejected, 1U);
    EXPECT_EQ(stats.moveRateLimitedRejected, 0U);
    ASSERT_TRUE(Core::ServerTestAccess::lastAcceptedRudpInputCmdSeq(
        server,
        1001).has_value());
    EXPECT_EQ(
        *Core::ServerTestAccess::lastAcceptedRudpInputCmdSeq(server, 1001),
        20U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMoveInputGuardSize(server), 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMoveDispatchStateSize(server), 0U);
    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(server, created.room.roomId, 1001),
        -1000,
        0);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
    const Game::RoomEventMetricsSnapshot metrics =
        Core::ServerTestAccess::roomEventMetrics(server);
    EXPECT_EQ(metrics.roomEventEnqueuedCount, 0U);
    EXPECT_EQ(metrics.roomEventProcessedCount, 0U);
}

TEST(ServerIntegrationTests, RateLimitedMoveDropsWithoutSequenceRollbackOrOutbound) {
    Core::Server server(0);
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(31004);
    ASSERT_EQ(
        Core::ServerTestAccess::bindRudpEndpoint(server, endpoint, 1001),
        Net::RudpSessionBindResult::kBoundNew);
    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);

    for (uint32_t i = 0; i < Net::RudpMoveInputGuard::kDefaultBurstCapacity; ++i) {
        Core::ServerTestAccess::processRudpInputCommandDelivery(
            server,
            endpoint,
            101 + i,
            moveInputCommandPayloadForTest(77, 100 + i, 1000, 0),
            timeAt(1000));
    }
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        111,
        moveInputCommandPayloadForTest(77, 110, 0, 1000),
        timeAt(1000));

    Core::RudpServerBindingStats stats = server.rudpBindingStats();
    EXPECT_EQ(stats.inputSequenceAccepted, 11U);
    EXPECT_EQ(stats.inputNoRoomRejected, 0U);
    EXPECT_EQ(stats.moveAccepted, 10U);
    EXPECT_EQ(stats.moveDispatched, 10U);
    EXPECT_EQ(stats.moveApplyRejected, 0U);
    EXPECT_EQ(stats.moveInvalidReservedFlagsRejected, 0U);
    EXPECT_EQ(stats.moveRateLimitedRejected, 1U);
    ASSERT_TRUE(Core::ServerTestAccess::lastAcceptedRudpInputCmdSeq(
        server,
        1001).has_value());
    EXPECT_EQ(
        *Core::ServerTestAccess::lastAcceptedRudpInputCmdSeq(server, 1001),
        110U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMoveInputGuardSize(server), 1U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMoveDispatchStateSize(server), 1U);
    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(server, created.room.roomId, 1001),
        -1000,
        0);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        112,
        moveInputCommandPayloadForTest(77, 111, 0, 0),
        timeAt(1034));

    stats = server.rudpBindingStats();
    EXPECT_EQ(stats.inputSequenceAccepted, 12U);
    EXPECT_EQ(stats.moveAccepted, 11U);
    EXPECT_EQ(stats.moveDispatched, 11U);
    EXPECT_EQ(stats.moveRateLimitedRejected, 1U);
    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(server, created.room.roomId, 1001),
        -966,
        0);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
    const Game::RoomEventMetricsSnapshot metrics =
        Core::ServerTestAccess::roomEventMetrics(server);
    EXPECT_EQ(metrics.roomEventEnqueuedCount, 0U);
    EXPECT_EQ(metrics.roomEventProcessedCount, 0U);
}

TEST(ServerIntegrationTests, FirstValidMoveInitializesMovementClockWithoutRoomEventOrOutbound) {
    Core::Server server(0);
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(31005);
    ASSERT_EQ(
        Core::ServerTestAccess::bindRudpEndpoint(server, endpoint, 1001),
        Net::RudpSessionBindResult::kBoundNew);
    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        101,
        moveInputCommandPayloadForTest(77, 30, 100, -100),
        timeAt(1000));

    const Core::RudpServerBindingStats stats = server.rudpBindingStats();
    EXPECT_EQ(stats.inputSequenceAccepted, 1U);
    EXPECT_EQ(stats.moveAccepted, 1U);
    EXPECT_EQ(stats.moveDispatched, 1U);
    EXPECT_EQ(stats.moveApplyRejected, 0U);
    EXPECT_EQ(stats.moveInvalidReservedFlagsRejected, 0U);
    EXPECT_EQ(stats.moveRateLimitedRejected, 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMoveDispatchStateSize(server), 1U);
    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(server, created.room.roomId, 1001),
        -1000,
        0);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
    const Game::RoomEventMetricsSnapshot metrics =
        Core::ServerTestAccess::roomEventMetrics(server);
    EXPECT_EQ(metrics.roomEventEnqueuedCount, 0U);
    EXPECT_EQ(metrics.roomEventProcessedCount, 0U);
}

TEST(ServerIntegrationTests, MoveDispatchAppliesElapsedIntervalUsingPreviousDirection) {
    Core::Server server(0);
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(31006);
    ASSERT_EQ(
        Core::ServerTestAccess::bindRudpEndpoint(server, endpoint, 1001),
        Net::RudpSessionBindResult::kBoundNew);
    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        101,
        moveInputCommandPayloadForTest(77, 30, 1000, 0),
        timeAt(1000));
    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(server, created.room.roomId, 1001),
        -1000,
        0);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        102,
        moveInputCommandPayloadForTest(77, 31, 0, 1000),
        timeAt(1500));
    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(server, created.room.roomId, 1001),
        -500,
        0);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        103,
        moveInputCommandPayloadForTest(77, 32, 0, 0),
        timeAt(2000));
    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(server, created.room.roomId, 1001),
        -500,
        500);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        104,
        moveInputCommandPayloadForTest(77, 33, 1000, 0),
        timeAt(2500));
    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(server, created.room.roomId, 1001),
        -500,
        500);

    const Core::RudpServerBindingStats stats = server.rudpBindingStats();
    EXPECT_EQ(stats.inputSequenceAccepted, 4U);
    EXPECT_EQ(stats.moveAccepted, 4U);
    EXPECT_EQ(stats.moveDispatched, 4U);
    EXPECT_EQ(stats.moveApplyRejected, 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMoveDispatchStateSize(server), 1U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
    const Game::RoomEventMetricsSnapshot metrics =
        Core::ServerTestAccess::roomEventMetrics(server);
    EXPECT_EQ(metrics.roomEventEnqueuedCount, 0U);
    EXPECT_EQ(metrics.roomEventProcessedCount, 0U);
}

TEST(ServerIntegrationTests, RejectedMoveDoesNotUpdateMovementClockOrStoredDirection) {
    Core::Server server(0);
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(31007);
    ASSERT_EQ(
        Core::ServerTestAccess::bindRudpEndpoint(server, endpoint, 1001),
        Net::RudpSessionBindResult::kBoundNew);
    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        101,
        moveInputCommandPayloadForTest(77, 40, 1000, 0),
        timeAt(1000));
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        102,
        moveInputCommandPayloadForTest(77, 41, 0, 1000, 1),
        timeAt(1500));
    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(server, created.room.roomId, 1001),
        -1000,
        0);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        103,
        moveInputCommandPayloadForTest(77, 42, 0, 0),
        timeAt(2000));
    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(server, created.room.roomId, 1001),
        0,
        0);

    const Core::RudpServerBindingStats stats = server.rudpBindingStats();
    EXPECT_EQ(stats.inputSequenceAccepted, 3U);
    EXPECT_EQ(stats.moveAccepted, 2U);
    EXPECT_EQ(stats.moveDispatched, 2U);
    EXPECT_EQ(stats.moveInvalidReservedFlagsRejected, 1U);
    EXPECT_EQ(stats.moveApplyRejected, 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMoveDispatchStateSize(server), 1U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
}

TEST(ServerIntegrationTests, FirstMovementSnapshotSendsUnreliablePayloadToBoundEndpoint) {
    Core::Server server(0);
    ASSERT_TRUE(server.start());
    Net::UdpSocket receiver;
    ASSERT_TRUE(receiver.open(0));
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(receiver.boundPort());

    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(Core::ServerTestAccess::joinRoom(
        server,
        1002,
        created.room.roomId).ok);
    ASSERT_TRUE(Core::ServerTestAccess::bindRudpEndpointWithPeer(
        server,
        endpoint,
        1001,
        timeAt(1000)));

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1000));

    Net::RudpStateSnapshotPayload snapshot;
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiver, 1, snapshot));
    EXPECT_EQ(snapshot.roomId, created.room.roomId);
    EXPECT_EQ(snapshot.serverTick, 1U);
    ASSERT_EQ(snapshot.players.size(), 2U);
    expectStateSnapshotPlayer(snapshot, 1001, -1000, 0);
    expectStateSnapshotPlayer(snapshot, 1002, 1000, 0);

    const Core::RudpServerSnapshotStats stats = server.rudpSnapshotStats();
    EXPECT_EQ(stats.built, 1U);
    EXPECT_EQ(stats.sent, 1U);
    EXPECT_EQ(stats.sendErrors, 0U);
    EXPECT_EQ(stats.skippedNoBoundEndpoint, 0U);
    EXPECT_EQ(stats.serializeFailed, 0U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpSnapshotRoomStateSize(server), 1U);
}

TEST(ServerIntegrationTests, MovementSnapshotCadenceUsesRoomLocalTick) {
    Core::Server server(0);
    ASSERT_TRUE(server.start());
    Net::UdpSocket receiver;
    ASSERT_TRUE(receiver.open(0));
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(receiver.boundPort());

    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(Core::ServerTestAccess::bindRudpEndpointWithPeer(
        server,
        endpoint,
        1001,
        timeAt(1000)));

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1000));
    Net::RudpStateSnapshotPayload snapshot;
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiver, 1, snapshot));
    EXPECT_EQ(snapshot.roomId, created.room.roomId);
    EXPECT_EQ(snapshot.serverTick, 1U);

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1099));
    Net::RudpStateSnapshotPayload unexpected;
    EXPECT_FALSE(receiveStateSnapshotPacketWithWait(
        receiver,
        2,
        unexpected,
        std::chrono::milliseconds(30)));
    Core::RudpServerSnapshotStats stats = server.rudpSnapshotStats();
    EXPECT_EQ(stats.built, 1U);
    EXPECT_EQ(stats.sent, 1U);

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1100));
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiver, 2, snapshot));
    EXPECT_EQ(snapshot.serverTick, 2U);
    stats = server.rudpSnapshotStats();
    EXPECT_EQ(stats.built, 2U);
    EXPECT_EQ(stats.sent, 2U);

    ASSERT_TRUE(Core::ServerTestAccess::leaveRoom(server, 1001).ok);
    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1200));
    EXPECT_EQ(Core::ServerTestAccess::rudpSnapshotRoomStateSize(server), 0U);
}

TEST(ServerIntegrationTests, MovementSnapshotIntegratesHeldIntentBeforePayload) {
    Core::Server server(0);
    ASSERT_TRUE(server.start());
    Net::UdpSocket receiver;
    ASSERT_TRUE(receiver.open(0));
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(receiver.boundPort());

    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(Core::ServerTestAccess::bindRudpEndpointWithPeer(
        server,
        endpoint,
        1001,
        timeAt(1000)));

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        101,
        moveInputCommandPayloadForTest(77, 1, 1000, 0),
        timeAt(1000));
    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(server, created.room.roomId, 1001),
        -1000,
        0);

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1500));

    Net::RudpStateSnapshotPayload snapshot;
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiver, 1, snapshot));
    EXPECT_EQ(snapshot.serverTick, 1U);
    ASSERT_EQ(snapshot.players.size(), 1U);
    expectStateSnapshotPlayer(snapshot, 1001, -500, 0);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1600));
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiver, 2, snapshot));
    EXPECT_EQ(snapshot.serverTick, 2U);
    expectStateSnapshotPlayer(snapshot, 1001, -400, 0);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
}

TEST(ServerIntegrationTests, StopMovePreventsFurtherTimerIntegratedMovement) {
    Core::Server server(0);
    ASSERT_TRUE(server.start());
    Net::UdpSocket receiver;
    ASSERT_TRUE(receiver.open(0));
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(receiver.boundPort());

    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(Core::ServerTestAccess::bindRudpEndpointWithPeer(
        server,
        endpoint,
        1001,
        timeAt(1000)));

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        101,
        moveInputCommandPayloadForTest(77, 1, 1000, 0),
        timeAt(1000));
    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1500));
    Net::RudpStateSnapshotPayload snapshot;
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiver, 1, snapshot));
    expectStateSnapshotPlayer(snapshot, 1001, -500, 0);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        102,
        moveInputCommandPayloadForTest(77, 2, 0, 0),
        timeAt(1550));
    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(server, created.room.roomId, 1001),
        -450,
        0);

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1600));
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiver, 2, snapshot));
    EXPECT_EQ(snapshot.serverTick, 2U);
    expectStateSnapshotPlayer(snapshot, 1001, -450, 0);
    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1700));
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiver, 3, snapshot));
    EXPECT_EQ(snapshot.serverTick, 3U);
    expectStateSnapshotPlayer(snapshot, 1001, -450, 0);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
}

TEST(ServerIntegrationTests, RejectedMoveDoesNotBypassSnapshotCadenceOrChangePayload) {
    Core::Server server(0);
    ASSERT_TRUE(server.start());
    Net::UdpSocket receiver;
    ASSERT_TRUE(receiver.open(0));
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(receiver.boundPort());

    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(Core::ServerTestAccess::bindRudpEndpointWithPeer(
        server,
        endpoint,
        1001,
        timeAt(1000)));

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        101,
        moveInputCommandPayloadForTest(77, 1, 1000, 0),
        timeAt(1000));
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        102,
        moveInputCommandPayloadForTest(77, 2, 0, 0),
        timeAt(1500));
    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1500));
    Net::RudpStateSnapshotPayload snapshot;
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiver, 1, snapshot));
    expectStateSnapshotPlayer(snapshot, 1001, -500, 0);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        103,
        moveInputCommandPayloadForTest(77, 2, 0, 1000),
        timeAt(1510));
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        104,
        moveInputCommandPayloadForTest(77, 1, 0, 1000),
        timeAt(1520));
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        105,
        moveInputCommandPayloadForTest(77, 2U + 0x80000000U, 0, 1000),
        timeAt(1530));
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        106,
        moveInputCommandPayloadForTest(77, 3, 0, 1000, 1),
        timeAt(1540));

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1550));
    Net::RudpStateSnapshotPayload unexpected;
    EXPECT_FALSE(receiveStateSnapshotPacketWithWait(
        receiver,
        2,
        unexpected,
        std::chrono::milliseconds(30)));
    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(server, created.room.roomId, 1001),
        -500,
        0);

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1600));
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiver, 2, snapshot));
    EXPECT_EQ(snapshot.serverTick, 2U);
    expectStateSnapshotPlayer(snapshot, 1001, -500, 0);

    const Core::RudpServerBindingStats bindingStats = server.rudpBindingStats();
    EXPECT_EQ(bindingStats.inputSequenceDuplicateRejected, 1U);
    EXPECT_EQ(bindingStats.inputSequenceStaleRejected, 1U);
    EXPECT_EQ(bindingStats.inputSequenceAmbiguousRejected, 1U);
    EXPECT_EQ(bindingStats.moveInvalidReservedFlagsRejected, 1U);
    const Core::RudpServerSnapshotStats snapshotStats = server.rudpSnapshotStats();
    EXPECT_EQ(snapshotStats.built, 2U);
    EXPECT_EQ(snapshotStats.sent, 2U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
}

TEST(ServerIntegrationTests, RateLimitedMoveDoesNotBypassSnapshotCadence) {
    Core::Server server(0);
    ASSERT_TRUE(server.start());
    Net::UdpSocket receiver;
    ASSERT_TRUE(receiver.open(0));
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(receiver.boundPort());

    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(Core::ServerTestAccess::bindRudpEndpointWithPeer(
        server,
        endpoint,
        1001,
        timeAt(1000)));

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1000));
    Net::RudpStateSnapshotPayload snapshot;
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiver, 1, snapshot));

    for (uint32_t i = 0; i <= Net::RudpMoveInputGuard::kDefaultBurstCapacity; ++i) {
        Core::ServerTestAccess::processRudpInputCommandDelivery(
            server,
            endpoint,
            101 + i,
            moveInputCommandPayloadForTest(77, 10 + i, 1000, 0),
            timeAt(1050));
    }

    const Core::RudpServerBindingStats bindingStats = server.rudpBindingStats();
    EXPECT_EQ(bindingStats.moveRateLimitedRejected, 1U);
    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(server, created.room.roomId, 1001),
        -1000,
        0);

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1050));
    Net::RudpStateSnapshotPayload unexpected;
    EXPECT_FALSE(receiveStateSnapshotPacketWithWait(
        receiver,
        2,
        unexpected,
        std::chrono::milliseconds(30)));

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1100));
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiver, 2, snapshot));
    EXPECT_EQ(snapshot.serverTick, 2U);
    expectStateSnapshotPlayer(snapshot, 1001, -950, 0);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
}

TEST(ServerIntegrationTests, UnboundMoveDoesNotTouchMovementOrSnapshotState) {
    Core::Server server(0);
    ASSERT_TRUE(server.start());
    Net::UdpSocket receiver;
    ASSERT_TRUE(receiver.open(0));
    const Net::UdpEndpoint boundEndpoint = udpLoopbackEndpoint(receiver.boundPort());
    const Net::UdpEndpoint unboundEndpoint = udpLoopbackEndpoint(31990);

    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(Core::ServerTestAccess::bindRudpEndpointWithPeer(
        server,
        boundEndpoint,
        1001,
        timeAt(1000)));

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1000));
    Net::RudpStateSnapshotPayload snapshot;
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiver, 1, snapshot));
    expectStateSnapshotPlayer(snapshot, 1001, -1000, 0);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        unboundEndpoint,
        501,
        moveInputCommandPayloadForTest(77, 50, 1000, 0),
        timeAt(1010));

    const Core::RudpServerBindingStats bindingStats = server.rudpBindingStats();
    EXPECT_EQ(bindingStats.unboundInputRejected, 1U);
    EXPECT_EQ(bindingStats.inputCandidates, 0U);
    EXPECT_EQ(bindingStats.inputDecoded, 0U);
    EXPECT_EQ(bindingStats.inputSequenceAccepted, 0U);
    EXPECT_EQ(bindingStats.moveAccepted, 0U);
    EXPECT_EQ(bindingStats.moveDispatched, 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpInputCommandSequenceTrackerSize(server), 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMoveInputGuardSize(server), 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMoveDispatchStateSize(server), 0U);
    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(server, created.room.roomId, 1001),
        -1000,
        0);

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1050));
    Net::RudpStateSnapshotPayload unexpected;
    EXPECT_FALSE(receiveStateSnapshotPacketWithWait(
        receiver,
        2,
        unexpected,
        std::chrono::milliseconds(30)));

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1100));
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiver, 2, snapshot));
    expectStateSnapshotPlayer(snapshot, 1001, -1000, 0);

    const Core::RudpServerSnapshotStats snapshotStats = server.rudpSnapshotStats();
    EXPECT_EQ(snapshotStats.built, 2U);
    EXPECT_EQ(snapshotStats.sent, 2U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
}

TEST(ServerIntegrationTests, BoundNoRoomMoveDoesNotRecreateSnapshotState) {
    Core::Server server(0);
    ASSERT_TRUE(server.start());
    Net::UdpSocket receiver;
    ASSERT_TRUE(receiver.open(0));
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(receiver.boundPort());

    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(Core::ServerTestAccess::bindRudpEndpointWithPeer(
        server,
        endpoint,
        1001,
        timeAt(1000)));

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1000));
    Net::RudpStateSnapshotPayload snapshot;
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiver, 1, snapshot));
    EXPECT_EQ(Core::ServerTestAccess::rudpSnapshotRoomStateSize(server), 1U);

    ASSERT_TRUE(Core::ServerTestAccess::leaveRoom(server, 1001).ok);
    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1100));
    EXPECT_EQ(Core::ServerTestAccess::rudpSnapshotRoomStateSize(server), 0U);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        502,
        moveInputCommandPayloadForTest(77, 60, 1000, 0),
        timeAt(1110));

    const Core::RudpServerBindingStats bindingStats = server.rudpBindingStats();
    EXPECT_EQ(bindingStats.inputSequenceAccepted, 1U);
    EXPECT_EQ(bindingStats.inputNoRoomRejected, 1U);
    EXPECT_EQ(bindingStats.moveAccepted, 0U);
    EXPECT_EQ(bindingStats.moveDispatched, 0U);
    ASSERT_TRUE(Core::ServerTestAccess::lastAcceptedRudpInputCmdSeq(
        server,
        1001).has_value());
    EXPECT_EQ(
        *Core::ServerTestAccess::lastAcceptedRudpInputCmdSeq(server, 1001),
        60U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMoveInputGuardSize(server), 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpMoveDispatchStateSize(server), 0U);

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1200));
    Net::RudpStateSnapshotPayload unexpected;
    EXPECT_FALSE(receiveStateSnapshotPacketWithWait(
        receiver,
        2,
        unexpected,
        std::chrono::milliseconds(30)));
    EXPECT_EQ(Core::ServerTestAccess::rudpSnapshotRoomStateSize(server), 0U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
}

TEST(ServerIntegrationTests, RoomLeavePrunesTimerMovementIntentWithoutSnapshotMutation) {
    Core::Server server(0);
    ASSERT_TRUE(server.start());
    Net::UdpSocket receiver;
    ASSERT_TRUE(receiver.open(0));
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(receiver.boundPort());

    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(Core::ServerTestAccess::bindRudpEndpointWithPeer(
        server,
        endpoint,
        1001,
        timeAt(1000)));

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1000));
    Net::RudpStateSnapshotPayload snapshot;
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiver, 1, snapshot));

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        503,
        moveInputCommandPayloadForTest(77, 61, 1000, 0),
        timeAt(1010));
    EXPECT_EQ(Core::ServerTestAccess::rudpMoveDispatchStateSize(server), 1U);

    ASSERT_TRUE(Core::ServerTestAccess::leaveRoom(server, 1001).ok);
    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1110));
    EXPECT_EQ(Core::ServerTestAccess::rudpMoveDispatchStateSize(server), 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpSnapshotRoomStateSize(server), 0U);

    Net::RudpStateSnapshotPayload unexpected;
    EXPECT_FALSE(receiveStateSnapshotPacketWithWait(
        receiver,
        2,
        unexpected,
        std::chrono::milliseconds(30)));
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
}

TEST(ServerIntegrationTests, MovementBurstAbuseDoesNotSpamSnapshots) {
    Core::Server server(0);
    ASSERT_TRUE(server.start());
    Net::UdpSocket receiver;
    ASSERT_TRUE(receiver.open(0));
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(receiver.boundPort());

    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(Core::ServerTestAccess::bindRudpEndpointWithPeer(
        server,
        endpoint,
        1001,
        timeAt(1000)));

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1000));
    Net::RudpStateSnapshotPayload snapshot;
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiver, 1, snapshot));

    for (uint32_t i = 0; i <= Net::RudpMoveInputGuard::kDefaultBurstCapacity; ++i) {
        Core::ServerTestAccess::processRudpInputCommandDelivery(
            server,
            endpoint,
            601 + i,
            moveInputCommandPayloadForTest(77, 200 + i, 1000, 0),
            timeAt(1050));
    }
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        612,
        moveInputCommandPayloadForTest(77, 211, 0, 0),
        timeAt(1084));

    const Core::RudpServerBindingStats bindingStats = server.rudpBindingStats();
    EXPECT_EQ(bindingStats.inputSequenceAccepted, 12U);
    EXPECT_EQ(bindingStats.moveAccepted, 11U);
    EXPECT_EQ(bindingStats.moveDispatched, 11U);
    EXPECT_EQ(bindingStats.moveRateLimitedRejected, 1U);
    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(server, created.room.roomId, 1001),
        -966,
        0);

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1084));
    Net::RudpStateSnapshotPayload unexpected;
    EXPECT_FALSE(receiveStateSnapshotPacketWithWait(
        receiver,
        2,
        unexpected,
        std::chrono::milliseconds(30)));

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1100));
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiver, 2, snapshot));
    EXPECT_EQ(snapshot.serverTick, 2U);
    expectStateSnapshotPlayer(snapshot, 1001, -966, 0);

    const Core::RudpServerSnapshotStats snapshotStats = server.rudpSnapshotStats();
    EXPECT_EQ(snapshotStats.built, 2U);
    EXPECT_EQ(snapshotStats.sent, 2U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
}

TEST(ServerIntegrationTests, TwoPlayerSnapshotFanoutIgnoresRejectedMoveWorkload) {
    Core::Server server(0);
    ASSERT_TRUE(server.start());
    Net::UdpSocket receiverA;
    Net::UdpSocket receiverB;
    ASSERT_TRUE(receiverA.open(0));
    ASSERT_TRUE(receiverB.open(0));
    const Net::UdpEndpoint endpointA = udpLoopbackEndpoint(receiverA.boundPort());
    const Net::UdpEndpoint endpointB = udpLoopbackEndpoint(receiverB.boundPort());

    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(Core::ServerTestAccess::joinRoom(
        server,
        1002,
        created.room.roomId).ok);
    ASSERT_TRUE(Core::ServerTestAccess::bindRudpEndpointWithPeer(
        server,
        endpointA,
        1001,
        timeAt(1000)));
    ASSERT_TRUE(Core::ServerTestAccess::bindRudpEndpointWithPeer(
        server,
        endpointB,
        1002,
        timeAt(1000)));

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1000));
    Net::RudpStateSnapshotPayload snapshotA;
    Net::RudpStateSnapshotPayload snapshotB;
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiverA, 1, snapshotA));
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiverB, 1, snapshotB));

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpointA,
        701,
        moveInputCommandPayloadForTest(77, 1, 1000, 0),
        timeAt(1010));
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpointB,
        801,
        moveInputCommandPayloadForTest(88, 1, 0, 1000),
        timeAt(1010));
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpointA,
        702,
        moveInputCommandPayloadForTest(77, 2, 0, 0),
        timeAt(1110));
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpointB,
        802,
        moveInputCommandPayloadForTest(88, 2, 0, 0),
        timeAt(1110));

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1110));
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiverA, 2, snapshotA));
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiverB, 2, snapshotB));
    EXPECT_EQ(snapshotA.serverTick, 2U);
    EXPECT_EQ(snapshotB.serverTick, 2U);
    expectStateSnapshotPlayer(snapshotA, 1001, -900, 0);
    expectStateSnapshotPlayer(snapshotA, 1002, 1000, 100);
    expectStateSnapshotPlayer(snapshotB, 1001, -900, 0);
    expectStateSnapshotPlayer(snapshotB, 1002, 1000, 100);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpointA,
        703,
        moveInputCommandPayloadForTest(77, 2, 0, -1000),
        timeAt(1120));
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpointA,
        704,
        moveInputCommandPayloadForTest(77, 1, 0, -1000),
        timeAt(1130));
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpointB,
        803,
        moveInputCommandPayloadForTest(88, 3, -1000, 0, 1),
        timeAt(1140));

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1150));
    Net::RudpStateSnapshotPayload unexpected;
    EXPECT_FALSE(receiveStateSnapshotPacketWithWait(
        receiverA,
        3,
        unexpected,
        std::chrono::milliseconds(30)));
    EXPECT_FALSE(receiveStateSnapshotPacketWithWait(
        receiverB,
        3,
        unexpected,
        std::chrono::milliseconds(30)));

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1210));
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiverA, 3, snapshotA));
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiverB, 3, snapshotB));
    EXPECT_EQ(snapshotA.serverTick, 3U);
    EXPECT_EQ(snapshotB.serverTick, 3U);
    expectStateSnapshotPlayer(snapshotA, 1001, -900, 0);
    expectStateSnapshotPlayer(snapshotA, 1002, 1000, 100);
    expectStateSnapshotPlayer(snapshotB, 1001, -900, 0);
    expectStateSnapshotPlayer(snapshotB, 1002, 1000, 100);

    const Core::RudpServerBindingStats bindingStats = server.rudpBindingStats();
    EXPECT_EQ(bindingStats.inputSequenceDuplicateRejected, 1U);
    EXPECT_EQ(bindingStats.inputSequenceStaleRejected, 1U);
    EXPECT_EQ(bindingStats.moveInvalidReservedFlagsRejected, 1U);
    const Core::RudpServerSnapshotStats snapshotStats = server.rudpSnapshotStats();
    EXPECT_EQ(snapshotStats.built, 3U);
    EXPECT_EQ(snapshotStats.sent, 6U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
}

TEST(ServerIntegrationTests, MovementSnapshotSkipsRoomWithoutBoundEndpoint) {
    Core::Server server(0);
    ASSERT_TRUE(server.start());
    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1000));

    const Core::RudpServerSnapshotStats stats = server.rudpSnapshotStats();
    EXPECT_EQ(stats.built, 0U);
    EXPECT_EQ(stats.sent, 0U);
    EXPECT_EQ(stats.sendErrors, 0U);
    EXPECT_EQ(stats.skippedNoBoundEndpoint, 1U);
    EXPECT_EQ(stats.serializeFailed, 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpSnapshotRoomStateSize(server), 0U);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
}

TEST(ServerIntegrationTests, MovementSnapshotSharesOutboundSequenceWithReliableEvents) {
    Core::Server server(0);
    ASSERT_TRUE(server.start());
    Net::UdpSocket receiverA;
    Net::UdpSocket receiverB;
    ASSERT_TRUE(receiverA.open(0));
    ASSERT_TRUE(receiverB.open(0));
    const Net::UdpEndpoint endpointA = udpLoopbackEndpoint(receiverA.boundPort());
    const Net::UdpEndpoint endpointB = udpLoopbackEndpoint(receiverB.boundPort());

    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(Core::ServerTestAccess::joinRoom(
        server,
        1002,
        created.room.roomId).ok);
    ASSERT_TRUE(Core::ServerTestAccess::bindRudpEndpointWithPeer(
        server,
        endpointA,
        1001,
        timeAt(1000)));
    ASSERT_TRUE(Core::ServerTestAccess::bindRudpEndpointWithPeer(
        server,
        endpointB,
        1002,
        timeAt(1000)));

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1000));
    Net::RudpStateSnapshotPayload snapshotA;
    Net::RudpStateSnapshotPayload snapshotB;
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiverA, 1, snapshotA));
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiverB, 1, snapshotB));
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);

    std::vector<int> disconnectedClients;
    ASSERT_TRUE(Core::ServerTestAccess::broadcastBattleStart(
        server,
        battleStartResultForTest(created.room.roomId, {1001, 1002}),
        disconnectedClients));

    Net::RudpBattleStartPayload battleStartA;
    Net::RudpBattleStartPayload battleStartB;
    ASSERT_TRUE(receiveBattleStartPacketWithWait(receiverA, 2, battleStartA));
    ASSERT_TRUE(receiveBattleStartPacketWithWait(receiverB, 2, battleStartB));
    EXPECT_EQ(battleStartA.roomId, created.room.roomId);
    EXPECT_EQ(battleStartB.roomId, created.room.roomId);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 2U);
    expectBattleStartPending(
        server,
        1001,
        2,
        "BattleStart:" + std::to_string(created.room.roomId) + ":1001:1002",
        created.room.roomId,
        1001,
        1002);
    expectBattleStartPending(
        server,
        1002,
        2,
        "BattleStart:" + std::to_string(created.room.roomId) + ":1001:1002",
        created.room.roomId,
        1001,
        1002);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpointA,
        901,
        moveInputCommandPayloadForTest(77, 1, 1000, 0, 1),
        timeAt(1010));
    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpointB,
        902,
        moveInputCommandPayloadForTest(88, 1, 0, 1000, 1),
        timeAt(1010));

    Core::ServerTestAccess::processRudpMovementSnapshots(server, timeAt(1100));
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiverA, 3, snapshotA));
    ASSERT_TRUE(receiveStateSnapshotPacketWithWait(receiverB, 3, snapshotB));
    EXPECT_EQ(snapshotA.serverTick, 2U);
    EXPECT_EQ(snapshotB.serverTick, 2U);
    expectStateSnapshotPlayer(snapshotA, 1001, -1000, 0);
    expectStateSnapshotPlayer(snapshotA, 1002, 1000, 0);
    expectStateSnapshotPlayer(snapshotB, 1001, -1000, 0);
    expectStateSnapshotPlayer(snapshotB, 1002, 1000, 0);
    EXPECT_EQ(server.rudpReliableEventPendingCount(), 2U);
    const Core::RudpServerBindingStats bindingStats = server.rudpBindingStats();
    EXPECT_EQ(bindingStats.moveInvalidReservedFlagsRejected, 2U);
}

TEST(ServerIntegrationTests, RudpReadyInputsDispatchThroughInlineActorWithoutTcpFallback) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TwoPlayerRoomClients clients;
    ASSERT_TRUE(setupTwoPlayerRoomForRudpTest(runningServer, clients));

    Net::UdpSocket senderA;
    Net::UdpSocket senderB;
    ASSERT_TRUE(senderA.open(0));
    ASSERT_TRUE(senderB.open(0));
    ASSERT_TRUE(bindRudpEndpointForSessionTest(
        runningServer,
        senderA,
        clients.sessionA,
        77,
        100,
        1));
    ASSERT_TRUE(bindRudpEndpointForSessionTest(
        runningServer,
        senderB,
        clients.sessionB,
        78,
        101,
        2));

    sendUdpPacket(
        senderA,
        serializeRudpPacketForTest(
            inputCommandRudpHeader(101),
            readyInputCommandPayloadForTest(77, 1)),
        udpPort);

    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerBindingStats stats =
                runningServer.server.rudpBindingStats();
            return stats.inputCandidates >= 1U &&
                stats.inputDecoded >= 1U &&
                stats.inputSequenceAccepted >= 1U &&
                runningServer.server.rudpReliableEventPendingCount() == 0U;
        }));

    const Game::Room* room = Core::ServerTestAccess::findRoom(
        runningServer.server,
        clients.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_TRUE(room->isReady(clients.sessionA));
    EXPECT_FALSE(room->battleStarted());

    sendUdpPacket(
        senderB,
        serializeRudpPacketForTest(
            inputCommandRudpHeader(102),
            readyInputCommandPayloadForTest(78, 1)),
        udpPort);

    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            const Core::RudpServerBindingStats stats =
                runningServer.server.rudpBindingStats();
            return stats.inputDecoded >= 2U &&
                stats.inputSequenceAccepted >= 2U &&
                runningServer.server.rudpReliableEventPendingCount() == 0U;
        }));

    room = Core::ServerTestAccess::findRoom(runningServer.server, clients.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_TRUE(room->isReady(clients.sessionA));
    EXPECT_TRUE(room->isReady(clients.sessionB));
    EXPECT_FALSE(room->battleStarted());
    EXPECT_FALSE(room->hasAliveMonster());

    expectNoTcpPacketForTest(clients.clientA);
    expectNoTcpPacketForTest(clients.clientB);

    ::close(clients.clientB);
    ::close(clients.clientA);
}

TEST(ServerIntegrationTests, TcpArenaLoadBarrierStartsGameplayAfterAllClientsComplete) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TwoPlayerRoomClients clients;
    ASSERT_TRUE(setupTwoPlayerRoomForRudpTest(runningServer, clients));

    ASSERT_TRUE(sendReadyRoomRequestPacket(clients.clientA));
    uint32_t readyRoomId = 0;
    uint16_t readyPlayerCount = 0;
    uint16_t totalPlayerCount = 0;
    ASSERT_TRUE(recvReadyRoomResponsePacket(
        clients.clientA,
        readyRoomId,
        readyPlayerCount,
        totalPlayerCount));
    EXPECT_EQ(readyRoomId, clients.roomId);
    EXPECT_EQ(readyPlayerCount, 1u);
    EXPECT_EQ(totalPlayerCount, 2u);

    Net::TcpRoomDetailState detail;
    ASSERT_TRUE(recvRoomDetailStatePacket(clients.clientA, detail));
    ASSERT_TRUE(recvRoomDetailStatePacket(clients.clientB, detail));

    ASSERT_TRUE(sendReadyRoomRequestPacket(clients.clientB));
    ASSERT_TRUE(recvReadyRoomResponsePacket(
        clients.clientB,
        readyRoomId,
        readyPlayerCount,
        totalPlayerCount));
    EXPECT_EQ(readyRoomId, clients.roomId);
    EXPECT_EQ(readyPlayerCount, 2u);
    EXPECT_EQ(totalPlayerCount, 2u);
    ASSERT_TRUE(recvRoomDetailStatePacket(clients.clientA, detail));
    ASSERT_TRUE(recvRoomDetailStatePacket(clients.clientB, detail));

    ASSERT_TRUE(sendHostStartBattleRequestPacket(clients.clientA));
    uint32_t hostStartRoomId = 0;
    ASSERT_TRUE(recvHostStartBattleResponsePacket(clients.clientA, hostStartRoomId));
    EXPECT_EQ(hostStartRoomId, clients.roomId);

    uint32_t battleStartRoomId = 0;
    uint64_t playerA = 0;
    uint64_t playerB = 0;
    ASSERT_TRUE(recvBattleStartPacket(clients.clientA, battleStartRoomId, playerA, playerB));
    EXPECT_EQ(battleStartRoomId, clients.roomId);
    EXPECT_EQ(playerA, clients.sessionA);
    EXPECT_EQ(playerB, clients.sessionB);

    ASSERT_TRUE(recvBattleStartPacket(clients.clientB, battleStartRoomId, playerA, playerB));
    EXPECT_EQ(battleStartRoomId, clients.roomId);
    EXPECT_EQ(playerA, clients.sessionA);
    EXPECT_EQ(playerB, clients.sessionB);

    const std::vector<uint64_t> expectedPlayers{clients.sessionA, clients.sessionB};
    uint32_t loadRoomId = 0;
    uint64_t battleInstanceId = 0;
    std::vector<uint64_t> loadPlayers;
    ASSERT_TRUE(recvBattleLoadEntryPacket(
        clients.clientA,
        loadRoomId,
        battleInstanceId,
        loadPlayers));
    EXPECT_EQ(loadRoomId, clients.roomId);
    EXPECT_GT(battleInstanceId, 0u);
    EXPECT_EQ(loadPlayers, expectedPlayers);

    uint64_t battleInstanceIdB = 0;
    ASSERT_TRUE(recvBattleLoadEntryPacket(
        clients.clientB,
        loadRoomId,
        battleInstanceIdB,
        loadPlayers));
    EXPECT_EQ(loadRoomId, clients.roomId);
    EXPECT_EQ(battleInstanceIdB, battleInstanceId);
    EXPECT_EQ(loadPlayers, expectedPlayers);

    const Game::Room* room = Core::ServerTestAccess::findRoom(
        runningServer.server,
        clients.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_TRUE(room->battleStarted());
    EXPECT_TRUE(room->hasActiveArenaLoadBarrier());
    EXPECT_FALSE(room->arenaGameplayStarted());
    EXPECT_FALSE(room->hasAliveMonster());

    ASSERT_TRUE(sendArenaLoadCompletePacket(clients.clientA, clients.roomId, battleInstanceId + 1));
    expectNoTcpPacketForTest(clients.clientA);
    expectNoTcpPacketForTest(clients.clientB);

    room = Core::ServerTestAccess::findRoom(runningServer.server, clients.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_TRUE(room->hasActiveArenaLoadBarrier());
    EXPECT_FALSE(room->arenaGameplayStarted());
    EXPECT_FALSE(room->hasAliveMonster());

    ASSERT_TRUE(sendArenaLoadCompletePacket(clients.clientA, clients.roomId, battleInstanceId));
    expectNoTcpPacketForTest(clients.clientA);
    expectNoTcpPacketForTest(clients.clientB);
    ASSERT_TRUE(setReceiveTimeout(clients.clientA, std::chrono::milliseconds(500)));
    ASSERT_TRUE(setReceiveTimeout(clients.clientB, std::chrono::milliseconds(500)));

    room = Core::ServerTestAccess::findRoom(runningServer.server, clients.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_TRUE(room->hasActiveArenaLoadBarrier());
    EXPECT_FALSE(room->arenaGameplayStarted());
    EXPECT_FALSE(room->hasAliveMonster());

    ASSERT_TRUE(sendArenaLoadCompletePacket(clients.clientB, clients.roomId, battleInstanceId));

    uint32_t arenaStartRoomId = 0;
    uint64_t arenaStartBattleInstanceId = 0;
    ASSERT_TRUE(recvArenaGameplayStartPacket(
        clients.clientA,
        arenaStartRoomId,
        arenaStartBattleInstanceId));
    EXPECT_EQ(arenaStartRoomId, clients.roomId);
    EXPECT_EQ(arenaStartBattleInstanceId, battleInstanceId);

    ASSERT_TRUE(recvArenaGameplayStartPacket(
        clients.clientB,
        arenaStartRoomId,
        arenaStartBattleInstanceId));
    EXPECT_EQ(arenaStartRoomId, clients.roomId);
    EXPECT_EQ(arenaStartBattleInstanceId, battleInstanceId);

    uint32_t spawnRoomId = 0;
    uint32_t monsterId = 0;
    uint32_t monsterTypeId = 0;
    uint16_t maxHp = 0;
    ASSERT_TRUE(recvMonsterSpawnPacket(
        clients.clientA,
        spawnRoomId,
        monsterId,
        monsterTypeId,
        maxHp));
    EXPECT_EQ(spawnRoomId, clients.roomId);
    EXPECT_GT(monsterId, 0u);
    EXPECT_GT(monsterTypeId, 0u);
    EXPECT_GT(maxHp, 0u);

    uint32_t monsterIdB = 0;
    uint32_t monsterTypeIdB = 0;
    uint16_t maxHpB = 0;
    ASSERT_TRUE(recvMonsterSpawnPacket(
        clients.clientB,
        spawnRoomId,
        monsterIdB,
        monsterTypeIdB,
        maxHpB));
    EXPECT_EQ(spawnRoomId, clients.roomId);
    EXPECT_EQ(monsterIdB, monsterId);
    EXPECT_EQ(monsterTypeIdB, monsterTypeId);
    EXPECT_EQ(maxHpB, maxHp);

    room = Core::ServerTestAccess::findRoom(runningServer.server, clients.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_FALSE(room->hasActiveArenaLoadBarrier());
    EXPECT_TRUE(room->arenaGameplayStarted());
    EXPECT_TRUE(room->hasAliveMonster());

    ::close(clients.clientB);
    ::close(clients.clientA);
}

TEST(ServerIntegrationTests, SmokeCreateCenterDropRequestBroadcastsDropListSnapshot) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TwoPlayerRoomClients clients;
    Net::UdpSocket senderA;
    Net::UdpSocket senderB;
    ASSERT_TRUE(prepareRudpBattleForSmokeEndpointTest(
        runningServer,
        clients,
        senderA,
        senderB));

    ASSERT_TRUE(sendSmokeCreateCenterDropRequestPacket(clients.clientA));

    uint32_t dropIdA = 0;
    uint32_t itemIdA = 0;
    uint16_t quantityA = 0;
    ASSERT_TRUE(expectCenterDropSnapshot(
        clients.clientA,
        clients.roomId,
        dropIdA,
        itemIdA,
        quantityA));

    uint32_t dropIdB = 0;
    uint32_t itemIdB = 0;
    uint16_t quantityB = 0;
    ASSERT_TRUE(expectCenterDropSnapshot(
        clients.clientB,
        clients.roomId,
        dropIdB,
        itemIdB,
        quantityB));

    EXPECT_EQ(dropIdA, 1u);
    EXPECT_EQ(dropIdB, dropIdA);
    EXPECT_EQ(itemIdA, 1001u);
    EXPECT_EQ(itemIdB, itemIdA);
    EXPECT_EQ(quantityA, 1u);
    EXPECT_EQ(quantityB, quantityA);

    ::close(clients.clientB);
    ::close(clients.clientA);
}

TEST(ServerIntegrationTests, RejectsSmokeCreateCenterDropRequestOutsideRoom) {
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
    std::vector<uint64_t> clientSnapshot;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientFd, clientSnapshot));

    ASSERT_TRUE(sendSmokeCreateCenterDropRequestPacket(clientFd));
    Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
    Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
    ASSERT_TRUE(recvErrorPacket(clientFd, failedType, errorCode));
    EXPECT_EQ(failedType, Net::TcpPacketType::kSmokeCreateCenterDropRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kNotInRoom);

    ::close(clientFd);
}

TEST(ServerIntegrationTests, RejectsSmokeCreateCenterDropRequestBeforeBattleStart) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TwoPlayerRoomClients clients;
    ASSERT_TRUE(setupTwoPlayerRoomForRudpTest(runningServer, clients));

    ASSERT_TRUE(sendSmokeCreateCenterDropRequestPacket(clients.clientA));
    Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
    Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
    ASSERT_TRUE(recvErrorPacket(clients.clientA, failedType, errorCode));
    EXPECT_EQ(failedType, Net::TcpPacketType::kSmokeCreateCenterDropRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kNotFound);
    expectNoTcpPacketForTest(clients.clientB);

    ::close(clients.clientB);
    ::close(clients.clientA);
}

TEST(ServerIntegrationTests, SmokePlacePlayersAroundCenterDropRequestUpdatesAuthoritativeMovementOnly) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TwoPlayerRoomClients clients;
    Net::UdpSocket senderA;
    Net::UdpSocket senderB;
    ASSERT_TRUE(prepareRudpBattleForSmokeEndpointTest(
        runningServer,
        clients,
        senderA,
        senderB));

    ASSERT_TRUE(sendSmokeCreateCenterDropRequestPacket(clients.clientA));
    uint32_t dropId = 0;
    uint32_t itemId = 0;
    uint16_t quantity = 0;
    ASSERT_TRUE(expectCenterDropSnapshot(clients.clientA, clients.roomId, dropId, itemId, quantity));
    uint32_t ignoredDropId = 0;
    uint32_t ignoredItemId = 0;
    uint16_t ignoredQuantity = 0;
    ASSERT_TRUE(expectCenterDropSnapshot(
        clients.clientB,
        clients.roomId,
        ignoredDropId,
        ignoredItemId,
        ignoredQuantity));

    ASSERT_TRUE(sendSmokePlacePlayersAroundCenterDropRequestPacket(clients.clientA));
    ASSERT_TRUE(waitUntil(
        [&runningServer, &clients]() {
            const auto positionA = Core::ServerTestAccess::movementPosition(
                runningServer.server,
                clients.roomId,
                clients.sessionA);
            const auto positionB = Core::ServerTestAccess::movementPosition(
                runningServer.server,
                clients.roomId,
                clients.sessionB);
            return positionA.has_value() &&
                   positionB.has_value() &&
                   positionA->x == -1000 &&
                   positionA->y == 0 &&
                   positionB->x == 1000 &&
                   positionB->y == 0;
        }));

    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(runningServer.server, clients.roomId, clients.sessionA),
        -1000,
        0);
    expectMovementPosition(
        Core::ServerTestAccess::movementPosition(runningServer.server, clients.roomId, clients.sessionB),
        1000,
        0);
    expectNoTcpPacketForTest(clients.clientA);
    expectNoTcpPacketForTest(clients.clientB);

    ::close(clients.clientB);
    ::close(clients.clientA);
}

TEST(ServerIntegrationTests, RejectsSmokePlacePlayersAroundCenterDropRequestWithoutDrop) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TwoPlayerRoomClients clients;
    Net::UdpSocket senderA;
    Net::UdpSocket senderB;
    ASSERT_TRUE(prepareRudpBattleForSmokeEndpointTest(
        runningServer,
        clients,
        senderA,
        senderB));

    ASSERT_TRUE(sendSmokePlacePlayersAroundCenterDropRequestPacket(clients.clientA));
    Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
    Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
    ASSERT_TRUE(recvErrorPacket(clients.clientA, failedType, errorCode));
    EXPECT_EQ(failedType, Net::TcpPacketType::kSmokePlacePlayersAroundCenterDropRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kNotFound);
    expectNoTcpPacketForTest(clients.clientB);

    ::close(clients.clientB);
    ::close(clients.clientA);
}

TEST(ServerIntegrationTests, RejectsSmokePlacePlayersAroundCenterDropRequestAfterBattleResultCloseout) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TwoPlayerRoomClients clients;
    Net::UdpSocket senderA;
    Net::UdpSocket senderB;
    ASSERT_TRUE(prepareRudpBattleForSmokeEndpointTest(
        runningServer,
        clients,
        senderA,
        senderB));

    ASSERT_TRUE(sendSmokeCreateCenterDropRequestPacket(clients.clientA));
    uint32_t dropId = 0;
    uint32_t itemId = 0;
    uint16_t quantity = 0;
    ASSERT_TRUE(expectCenterDropSnapshot(clients.clientA, clients.roomId, dropId, itemId, quantity));
    uint32_t ignoredDropId = 0;
    uint32_t ignoredItemId = 0;
    uint16_t ignoredQuantity = 0;
    ASSERT_TRUE(expectCenterDropSnapshot(
        clients.clientB,
        clients.roomId,
        ignoredDropId,
        ignoredItemId,
        ignoredQuantity));

    ASSERT_TRUE(sendClickLootRequestPacket(clients.clientA, dropId));

    uint32_t lootRoomId = 0;
    uint32_t resolvedDropId = 0;
    uint64_t winnerSessionId = 0;
    uint32_t resolvedItemId = 0;
    uint16_t resolvedQuantity = 0;
    ASSERT_TRUE(recvLootResolvedPacket(
        clients.clientA,
        lootRoomId,
        resolvedDropId,
        winnerSessionId,
        resolvedItemId,
        resolvedQuantity));
    EXPECT_EQ(lootRoomId, clients.roomId);
    EXPECT_EQ(resolvedDropId, dropId);
    EXPECT_EQ(winnerSessionId, clients.sessionA);
    EXPECT_EQ(resolvedItemId, itemId);
    EXPECT_EQ(resolvedQuantity, quantity);
    ASSERT_TRUE(recvLootResolvedPacket(
        clients.clientB,
        lootRoomId,
        resolvedDropId,
        winnerSessionId,
        resolvedItemId,
        resolvedQuantity));

    uint64_t inventorySessionId = 0;
    uint16_t currentWeight = 0;
    uint16_t maxWeight = 0;
    std::vector<Net::TcpInventoryEntry> entries;
    ASSERT_TRUE(recvInventorySnapshotPacket(
        clients.clientA,
        inventorySessionId,
        currentWeight,
        maxWeight,
        entries));
    EXPECT_EQ(inventorySessionId, clients.sessionA);

    uint32_t rankingRoomId = 0;
    uint64_t battleInstanceId = 0;
    std::vector<Net::BattleFinalRankingEntry> rankingRows;
    ASSERT_TRUE(recvBattleFinalRankingPacket(
        clients.clientA,
        rankingRoomId,
        battleInstanceId,
        rankingRows));
    EXPECT_EQ(rankingRoomId, clients.roomId);
    EXPECT_GT(battleInstanceId, 0u);
    ASSERT_EQ(rankingRows.size(), 2U);
    EXPECT_EQ(rankingRows[0].sessionId, clients.sessionA);
    EXPECT_EQ(rankingRows[0].totalAssetValue, 100);

    uint32_t rankingRoomIdB = 0;
    uint64_t battleInstanceIdB = 0;
    std::vector<Net::BattleFinalRankingEntry> rankingRowsB;
    ASSERT_TRUE(recvBattleFinalRankingPacket(
        clients.clientB,
        rankingRoomIdB,
        battleInstanceIdB,
        rankingRowsB));
    EXPECT_EQ(rankingRoomIdB, clients.roomId);
    EXPECT_EQ(battleInstanceIdB, battleInstanceId);
    ASSERT_EQ(rankingRowsB.size(), rankingRows.size());

    uint32_t previousRoomId = 0;
    Net::TcpLobbyReturnReason returnReason = Net::TcpLobbyReturnReason::kHostKick;
    ASSERT_TRUE(recvLobbyReturnVisibilityPacket(clients.clientA, previousRoomId, returnReason));
    EXPECT_EQ(previousRoomId, clients.roomId);
    EXPECT_EQ(returnReason, Net::TcpLobbyReturnReason::kNone);
    ASSERT_TRUE(recvLobbyReturnVisibilityPacket(clients.clientB, previousRoomId, returnReason));
    EXPECT_EQ(previousRoomId, clients.roomId);
    EXPECT_EQ(returnReason, Net::TcpLobbyReturnReason::kNone);

    std::vector<Net::TcpRoomEntry> roomSnapshot;
    ASSERT_TRUE(recvRoomListSnapshotPacket(clients.clientA, roomSnapshot));
    EXPECT_TRUE(roomSnapshot.empty());
    ASSERT_TRUE(recvRoomListSnapshotPacket(clients.clientB, roomSnapshot));
    EXPECT_TRUE(roomSnapshot.empty());

    ASSERT_TRUE(sendSmokePlacePlayersAroundCenterDropRequestPacket(clients.clientA));
    Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
    Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
    ASSERT_TRUE(recvErrorPacket(clients.clientA, failedType, errorCode));
    EXPECT_EQ(failedType, Net::TcpPacketType::kSmokePlacePlayersAroundCenterDropRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kNotInRoom);

    ::close(clients.clientB);
    ::close(clients.clientA);
}

TEST(ServerIntegrationTests, AllowsSameSessionsToCreateFreshRoomAfterBattleResultCloseout) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TwoPlayerRoomClients clients;
    Net::UdpSocket senderA;
    Net::UdpSocket senderB;
    ASSERT_TRUE(prepareRudpBattleForSmokeEndpointTest(
        runningServer,
        clients,
        senderA,
        senderB));

    ASSERT_TRUE(sendSmokeCreateCenterDropRequestPacket(clients.clientA));
    uint32_t dropId = 0;
    uint32_t itemId = 0;
    uint16_t quantity = 0;
    ASSERT_TRUE(expectCenterDropSnapshot(clients.clientA, clients.roomId, dropId, itemId, quantity));
    uint32_t ignoredDropId = 0;
    uint32_t ignoredItemId = 0;
    uint16_t ignoredQuantity = 0;
    ASSERT_TRUE(expectCenterDropSnapshot(
        clients.clientB,
        clients.roomId,
        ignoredDropId,
        ignoredItemId,
        ignoredQuantity));

    ASSERT_TRUE(sendClickLootRequestPacket(clients.clientA, dropId));

    uint32_t lootRoomId = 0;
    uint32_t resolvedDropId = 0;
    uint64_t winnerSessionId = 0;
    uint32_t resolvedItemId = 0;
    uint16_t resolvedQuantity = 0;
    ASSERT_TRUE(recvLootResolvedPacket(
        clients.clientA,
        lootRoomId,
        resolvedDropId,
        winnerSessionId,
        resolvedItemId,
        resolvedQuantity));
    ASSERT_TRUE(recvLootResolvedPacket(
        clients.clientB,
        lootRoomId,
        resolvedDropId,
        winnerSessionId,
        resolvedItemId,
        resolvedQuantity));

    uint64_t inventorySessionId = 0;
    uint16_t currentWeight = 0;
    uint16_t maxWeight = 0;
    std::vector<Net::TcpInventoryEntry> entries;
    ASSERT_TRUE(recvInventorySnapshotPacket(
        clients.clientA,
        inventorySessionId,
        currentWeight,
        maxWeight,
        entries));

    uint32_t rankingRoomId = 0;
    uint64_t battleInstanceId = 0;
    std::vector<Net::BattleFinalRankingEntry> rankingRows;
    ASSERT_TRUE(recvBattleFinalRankingPacket(
        clients.clientA,
        rankingRoomId,
        battleInstanceId,
        rankingRows));
    ASSERT_TRUE(recvBattleFinalRankingPacket(
        clients.clientB,
        rankingRoomId,
        battleInstanceId,
        rankingRows));

    uint32_t previousRoomId = 0;
    Net::TcpLobbyReturnReason returnReason = Net::TcpLobbyReturnReason::kHostKick;
    ASSERT_TRUE(recvLobbyReturnVisibilityPacket(clients.clientA, previousRoomId, returnReason));
    EXPECT_EQ(previousRoomId, clients.roomId);
    EXPECT_EQ(returnReason, Net::TcpLobbyReturnReason::kNone);
    ASSERT_TRUE(recvLobbyReturnVisibilityPacket(clients.clientB, previousRoomId, returnReason));
    EXPECT_EQ(previousRoomId, clients.roomId);
    EXPECT_EQ(returnReason, Net::TcpLobbyReturnReason::kNone);

    std::vector<Net::TcpRoomEntry> roomSnapshot;
    ASSERT_TRUE(recvRoomListSnapshotPacket(clients.clientA, roomSnapshot));
    EXPECT_TRUE(roomSnapshot.empty());
    ASSERT_TRUE(recvRoomListSnapshotPacket(clients.clientB, roomSnapshot));
    EXPECT_TRUE(roomSnapshot.empty());

    ASSERT_TRUE(sendCreateRoomRequestPacket(clients.clientA));
    uint32_t freshRoomId = 0;
    uint16_t playerCount = 0;
    ASSERT_TRUE(recvCreateRoomResponsePacket(clients.clientA, freshRoomId, playerCount));
    EXPECT_NE(freshRoomId, clients.roomId);
    EXPECT_EQ(playerCount, 1U);

    Net::TcpRoomDetailState detail;
    ASSERT_TRUE(recvRoomDetailStatePacket(clients.clientA, detail));
    EXPECT_EQ(detail.roomId, freshRoomId);
    ASSERT_EQ(detail.members.size(), 1U);
    EXPECT_EQ(detail.members[0].sessionId, clients.sessionA);

    ASSERT_TRUE(recvRoomListSnapshotPacket(clients.clientB, roomSnapshot));
    ASSERT_EQ(roomSnapshot.size(), 1U);
    EXPECT_EQ(roomSnapshot[0].roomId, freshRoomId);

    ASSERT_TRUE(sendJoinRoomRequestPacket(clients.clientB, freshRoomId));
    uint32_t joinedRoomId = 0;
    ASSERT_TRUE(recvJoinRoomResponsePacket(clients.clientB, joinedRoomId, playerCount));
    EXPECT_EQ(joinedRoomId, freshRoomId);
    EXPECT_EQ(playerCount, 2U);

    ASSERT_TRUE(recvRoomDetailStatePacket(clients.clientA, detail));
    EXPECT_EQ(detail.roomId, freshRoomId);
    EXPECT_EQ(detail.members.size(), 2U);
    ASSERT_TRUE(recvRoomDetailStatePacket(clients.clientB, detail));
    EXPECT_EQ(detail.roomId, freshRoomId);
    EXPECT_EQ(detail.members.size(), 2U);

    ::close(clients.clientB);
    ::close(clients.clientA);
}

TEST(ServerIntegrationTests, RudpMonsterDeathDispatchesThroughInlineActorOnlyRudp) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TwoPlayerRoomClients clients;
    ASSERT_TRUE(setupTwoPlayerRoomForRudpTest(runningServer, clients));

    Net::UdpSocket senderA;
    Net::UdpSocket senderB;
    ASSERT_TRUE(senderA.open(0));
    ASSERT_TRUE(senderB.open(0));
    ASSERT_TRUE(bindRudpEndpointForSessionTest(
        runningServer,
        senderA,
        clients.sessionA,
        77,
        100,
        1));
    ASSERT_TRUE(bindRudpEndpointForSessionTest(
        runningServer,
        senderB,
        clients.sessionB,
        78,
        101,
        2));
    ASSERT_TRUE(startRudpBattleForTest(
        runningServer,
        clients,
        senderA,
        senderB,
        udpPort));

    const Game::Room* room = Core::ServerTestAccess::findRoom(
        runningServer.server,
        clients.roomId);
    ASSERT_NE(room, nullptr);
    const uint32_t monsterId = room->monster().monsterId;
    ASSERT_GT(monsterId, 0u);

    sendUdpPacket(
        senderA,
        serializeRudpPacketForTest(
            ackOnlyRudpHeader(clients.battleStartSequenceA),
            {}),
        udpPort);
    sendUdpPacket(
        senderB,
        serializeRudpPacketForTest(
            ackOnlyRudpHeader(clients.battleStartSequenceB),
            {}),
        udpPort);
    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.rudpReliableEventPendingCount() == 0U;
        }));
    ASSERT_EQ(runningServer.server.rudpReliableEventPendingCount(), 0U);

    sendUdpPacket(
        senderA,
        serializeRudpPacketForTest(
            inputCommandRudpHeader(203),
            monsterDeathInputCommandPayloadForTest(77, 2, monsterId)),
        udpPort);

    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.rudpReliableEventPendingCount() == 2U;
        }));

    room = Core::ServerTestAccess::findRoom(runningServer.server, clients.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_FALSE(room->hasAliveMonster());
    ASSERT_EQ(room->drops().size(), 1u);
    EXPECT_GT(room->drops()[0].dropId, 0u);

    const std::string monsterDeathKey =
        "MonsterDeath:" + std::to_string(clients.roomId) + ":" +
        std::to_string(monsterId);
    const std::optional<uint32_t> monsterDeathSequenceA =
        Core::ServerTestAccess::rudpReliableEventSequence(
            runningServer.server,
            clients.sessionA,
            Net::RudpReliableEventKind::kMonsterDeath,
            monsterDeathKey);
    const std::optional<uint32_t> monsterDeathSequenceB =
        Core::ServerTestAccess::rudpReliableEventSequence(
            runningServer.server,
            clients.sessionB,
            Net::RudpReliableEventKind::kMonsterDeath,
            monsterDeathKey);
    ASSERT_TRUE(monsterDeathSequenceA.has_value());
    ASSERT_TRUE(monsterDeathSequenceB.has_value());
    expectMonsterDeathPending(
        runningServer.server,
        clients.sessionA,
        *monsterDeathSequenceA,
        monsterDeathKey,
        clients.roomId,
        monsterId);
    expectMonsterDeathPending(
        runningServer.server,
        clients.sessionB,
        *monsterDeathSequenceB,
        monsterDeathKey,
        clients.roomId,
        monsterId);

    Net::RudpMonsterDeathGameEventPayload monsterDeathA;
    Net::RudpMonsterDeathGameEventPayload monsterDeathB;
    ASSERT_TRUE(receiveMonsterDeathPacketWithWait(
        senderA,
        *monsterDeathSequenceA,
        monsterDeathA));
    ASSERT_TRUE(receiveMonsterDeathPacketWithWait(
        senderB,
        *monsterDeathSequenceB,
        monsterDeathB));
    EXPECT_EQ(monsterDeathA.roomId, clients.roomId);
    EXPECT_EQ(monsterDeathA.monsterId, monsterId);
    EXPECT_EQ(monsterDeathB.roomId, clients.roomId);
    EXPECT_EQ(monsterDeathB.monsterId, monsterId);

    expectNoTcpPacketForTest(clients.clientA);
    expectNoTcpPacketForTest(clients.clientB);

    ::close(clients.clientB);
    ::close(clients.clientA);
}

TEST(ServerIntegrationTests, RudpAttackDispatchesTcpCombatResults) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TwoPlayerRoomClients clients;
    ASSERT_TRUE(setupTwoPlayerRoomForRudpTest(runningServer, clients));

    Net::UdpSocket senderA;
    Net::UdpSocket senderB;
    ASSERT_TRUE(senderA.open(0));
    ASSERT_TRUE(senderB.open(0));
    ASSERT_TRUE(bindRudpEndpointForSessionTest(
        runningServer,
        senderA,
        clients.sessionA,
        77,
        100,
        1));
    ASSERT_TRUE(bindRudpEndpointForSessionTest(
        runningServer,
        senderB,
        clients.sessionB,
        78,
        101,
        2));
    ASSERT_TRUE(startRudpBattleForTest(
        runningServer,
        clients,
        senderA,
        senderB,
        udpPort));

    const Game::Room* room = Core::ServerTestAccess::findRoom(
        runningServer.server,
        clients.roomId);
    ASSERT_NE(room, nullptr);
    const uint32_t monsterId = room->monster().monsterId;
    ASSERT_GT(monsterId, 0u);

    sendUdpPacket(
        senderA,
        serializeRudpPacketForTest(
            ackOnlyRudpHeader(clients.battleStartSequenceA),
            {}),
        udpPort);
    sendUdpPacket(
        senderB,
        serializeRudpPacketForTest(
            ackOnlyRudpHeader(clients.battleStartSequenceB),
            {}),
        udpPort);
    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.rudpReliableEventPendingCount() == 0U;
        }));

    auto sendAttack = [&](uint32_t packetSequence, uint32_t cmdSeq) {
        sendUdpPacket(
            senderA,
            serializeRudpPacketForTest(
                inputCommandRudpHeader(packetSequence),
                attackInputCommandPayloadForTest(77, cmdSeq, monsterId)),
            udpPort);
    };

    auto expectHealth = [&](int fd, uint16_t expectedHp) {
        uint32_t roomId = 0;
        uint32_t observedMonsterId = 0;
        uint16_t currentHp = 0;
        uint16_t maxHp = 0;
        ASSERT_TRUE(recvMonsterHealthSnapshotPacket(
            fd,
            roomId,
            observedMonsterId,
            currentHp,
            maxHp));
        EXPECT_EQ(roomId, clients.roomId);
        EXPECT_EQ(observedMonsterId, monsterId);
        EXPECT_EQ(currentHp, expectedHp);
        EXPECT_EQ(maxHp, Game::Room::kAttackDamage * 4u);
    };

    sendAttack(203, 2);
    expectHealth(clients.clientA, 75);
    expectHealth(clients.clientB, 75);

    sendAttack(204, 3);
    expectHealth(clients.clientA, 50);
    expectHealth(clients.clientB, 50);

    sendAttack(205, 4);
    expectHealth(clients.clientA, 25);
    expectHealth(clients.clientB, 25);

    sendAttack(206, 5);

    uint32_t deathRoomId = 0;
    uint32_t deadMonsterId = 0;
    ASSERT_TRUE(recvMonsterDeathPacket(clients.clientA, deathRoomId, deadMonsterId));
    EXPECT_EQ(deathRoomId, clients.roomId);
    EXPECT_EQ(deadMonsterId, monsterId);
    ASSERT_TRUE(recvMonsterDeathPacket(clients.clientB, deathRoomId, deadMonsterId));
    EXPECT_EQ(deathRoomId, clients.roomId);
    EXPECT_EQ(deadMonsterId, monsterId);

    uint32_t dropRoomIdA = 0;
    uint32_t scatterSeedA = 0;
    std::vector<Net::TcpDropEntryV2> dropsA;
    ASSERT_TRUE(recvDropListSnapshotV2Packet(
        clients.clientA,
        dropRoomIdA,
        scatterSeedA,
        dropsA));
    EXPECT_EQ(dropRoomIdA, clients.roomId);
    EXPECT_GT(scatterSeedA, 0u);
    ASSERT_EQ(dropsA.size(), 1u);
    EXPECT_GT(dropsA[0].dropId, 0u);
    EXPECT_GE(dropsA[0].posX, Game::Room::kArenaMinPosition);
    EXPECT_LE(dropsA[0].posX, Game::Room::kArenaMaxPosition);
    EXPECT_GE(dropsA[0].posY, Game::Room::kArenaMinPosition);
    EXPECT_LE(dropsA[0].posY, Game::Room::kArenaMaxPosition);

    uint32_t dropRoomIdB = 0;
    uint32_t scatterSeedB = 0;
    std::vector<Net::TcpDropEntryV2> dropsB;
    ASSERT_TRUE(recvDropListSnapshotV2Packet(
        clients.clientB,
        dropRoomIdB,
        scatterSeedB,
        dropsB));
    EXPECT_EQ(dropRoomIdB, clients.roomId);
    EXPECT_EQ(scatterSeedB, scatterSeedA);
    ASSERT_EQ(dropsB.size(), dropsA.size());
    EXPECT_EQ(dropsB[0].dropId, dropsA[0].dropId);
    EXPECT_EQ(dropsB[0].posX, dropsA[0].posX);
    EXPECT_EQ(dropsB[0].posY, dropsA[0].posY);

    room = Core::ServerTestAccess::findRoom(runningServer.server, clients.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_FALSE(room->hasAliveMonster());
    ASSERT_EQ(room->drops().size(), 1u);
    EXPECT_EQ(room->drops()[0].dropId, dropsA[0].dropId);

    ::close(clients.clientB);
    ::close(clients.clientA);
}

TEST(ServerIntegrationTests, RudpSpaceLootClaimsNearestDropThroughTcpResults) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TwoPlayerRoomClients clients;
    Net::UdpSocket senderA;
    Net::UdpSocket senderB;
    ASSERT_TRUE(prepareRudpBattleForSmokeEndpointTest(
        runningServer,
        clients,
        senderA,
        senderB));

    sendUdpPacket(
        senderA,
        serializeRudpPacketForTest(
            ackOnlyRudpHeader(clients.battleStartSequenceA),
            {}),
        udpPort);
    sendUdpPacket(
        senderB,
        serializeRudpPacketForTest(
            ackOnlyRudpHeader(clients.battleStartSequenceB),
            {}),
        udpPort);
    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.rudpReliableEventPendingCount() == 0U;
        }));

    ASSERT_TRUE(sendSmokeCreateCenterDropRequestPacket(clients.clientA));
    uint32_t dropId = 0;
    uint32_t itemId = 0;
    uint16_t quantity = 0;
    ASSERT_TRUE(expectCenterDropSnapshot(clients.clientA, clients.roomId, dropId, itemId, quantity));
    uint32_t ignoredDropId = 0;
    uint32_t ignoredItemId = 0;
    uint16_t ignoredQuantity = 0;
    ASSERT_TRUE(expectCenterDropSnapshot(
        clients.clientB,
        clients.roomId,
        ignoredDropId,
        ignoredItemId,
        ignoredQuantity));
    ASSERT_TRUE(sendSmokePlacePlayersAroundCenterDropRequestPacket(clients.clientA));

    sendUdpPacket(
        senderA,
        serializeRudpPacketForTest(
            inputCommandRudpHeader(203),
            spaceLootInputCommandPayloadForTest(77, 2)),
        udpPort);

    uint32_t lootRoomId = 0;
    uint32_t resolvedDropId = 0;
    uint64_t winnerSessionId = 0;
    uint32_t resolvedItemId = 0;
    uint16_t resolvedQuantity = 0;
    ASSERT_TRUE(recvLootResolvedPacket(
        clients.clientA,
        lootRoomId,
        resolvedDropId,
        winnerSessionId,
        resolvedItemId,
        resolvedQuantity));
    EXPECT_EQ(lootRoomId, clients.roomId);
    EXPECT_EQ(resolvedDropId, dropId);
    EXPECT_EQ(winnerSessionId, clients.sessionA);
    EXPECT_EQ(resolvedItemId, itemId);
    EXPECT_EQ(resolvedQuantity, quantity);
    ASSERT_TRUE(recvLootResolvedPacket(
        clients.clientB,
        lootRoomId,
        resolvedDropId,
        winnerSessionId,
        resolvedItemId,
        resolvedQuantity));
    EXPECT_EQ(resolvedDropId, dropId);
    EXPECT_EQ(winnerSessionId, clients.sessionA);

    uint64_t inventorySessionId = 0;
    uint16_t currentWeight = 0;
    uint16_t maxWeight = 0;
    std::vector<Net::TcpInventoryEntry> entries;
    ASSERT_TRUE(recvInventorySnapshotPacket(
        clients.clientA,
        inventorySessionId,
        currentWeight,
        maxWeight,
        entries));
    EXPECT_EQ(inventorySessionId, clients.sessionA);
    EXPECT_EQ(currentWeight, quantity);
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].itemId, itemId);
    EXPECT_EQ(entries[0].quantity, quantity);

    uint32_t rankingRoomId = 0;
    uint64_t battleInstanceId = 0;
    std::vector<Net::BattleFinalRankingEntry> rankingRows;
    ASSERT_TRUE(recvBattleFinalRankingPacket(
        clients.clientA,
        rankingRoomId,
        battleInstanceId,
        rankingRows));
    EXPECT_EQ(rankingRoomId, clients.roomId);
    EXPECT_GT(battleInstanceId, 0u);
    ASSERT_EQ(rankingRows.size(), 2U);
    EXPECT_EQ(rankingRows[0].sessionId, clients.sessionA);
    EXPECT_EQ(rankingRows[0].totalAssetValue, 100);

    uint32_t rankingRoomIdB = 0;
    uint64_t battleInstanceIdB = 0;
    std::vector<Net::BattleFinalRankingEntry> rankingRowsB;
    ASSERT_TRUE(recvBattleFinalRankingPacket(
        clients.clientB,
        rankingRoomIdB,
        battleInstanceIdB,
        rankingRowsB));
    EXPECT_EQ(rankingRoomIdB, clients.roomId);
    EXPECT_EQ(battleInstanceIdB, battleInstanceId);
    ASSERT_EQ(rankingRowsB.size(), rankingRows.size());

    uint32_t previousRoomId = 0;
    Net::TcpLobbyReturnReason returnReason = Net::TcpLobbyReturnReason::kHostKick;
    ASSERT_TRUE(recvLobbyReturnVisibilityPacket(clients.clientA, previousRoomId, returnReason));
    EXPECT_EQ(previousRoomId, clients.roomId);
    EXPECT_EQ(returnReason, Net::TcpLobbyReturnReason::kNone);
    ASSERT_TRUE(recvLobbyReturnVisibilityPacket(clients.clientB, previousRoomId, returnReason));
    EXPECT_EQ(previousRoomId, clients.roomId);
    EXPECT_EQ(returnReason, Net::TcpLobbyReturnReason::kNone);

    std::vector<Net::TcpRoomEntry> roomSnapshot;
    ASSERT_TRUE(recvRoomListSnapshotPacket(clients.clientA, roomSnapshot));
    EXPECT_TRUE(roomSnapshot.empty());
    ASSERT_TRUE(recvRoomListSnapshotPacket(clients.clientB, roomSnapshot));
    EXPECT_TRUE(roomSnapshot.empty());

    const Game::Room* room = Core::ServerTestAccess::findRoom(
        runningServer.server,
        clients.roomId);
    EXPECT_EQ(room, nullptr);

    ::close(clients.clientB);
    ::close(clients.clientA);
}

TEST(ServerIntegrationTests, RudpClickLootDispatchCompletesBattleResult) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TwoPlayerRoomClients clients;
    ASSERT_TRUE(setupTwoPlayerRoomForRudpTest(runningServer, clients));

    Net::UdpSocket senderA;
    Net::UdpSocket senderB;
    ASSERT_TRUE(senderA.open(0));
    ASSERT_TRUE(senderB.open(0));
    ASSERT_TRUE(bindRudpEndpointForSessionTest(
        runningServer,
        senderA,
        clients.sessionA,
        77,
        100,
        1));
    ASSERT_TRUE(bindRudpEndpointForSessionTest(
        runningServer,
        senderB,
        clients.sessionB,
        78,
        101,
        2));
    ASSERT_TRUE(startRudpBattleForTest(
        runningServer,
        clients,
        senderA,
        senderB,
        udpPort));

    const Game::Room* room = Core::ServerTestAccess::findRoom(
        runningServer.server,
        clients.roomId);
    ASSERT_NE(room, nullptr);
    const uint32_t monsterId = room->monster().monsterId;
    ASSERT_GT(monsterId, 0u);

    sendUdpPacket(
        senderA,
        serializeRudpPacketForTest(
            ackOnlyRudpHeader(clients.battleStartSequenceA),
            {}),
        udpPort);
    sendUdpPacket(
        senderB,
        serializeRudpPacketForTest(
            ackOnlyRudpHeader(clients.battleStartSequenceB),
            {}),
        udpPort);
    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.rudpReliableEventPendingCount() == 0U;
        }));

    sendUdpPacket(
        senderA,
        serializeRudpPacketForTest(
            inputCommandRudpHeader(203),
            monsterDeathInputCommandPayloadForTest(77, 2, monsterId)),
        udpPort);
    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.rudpReliableEventPendingCount() == 2U;
        }));

    room = Core::ServerTestAccess::findRoom(runningServer.server, clients.roomId);
    ASSERT_NE(room, nullptr);
    ASSERT_EQ(room->drops().size(), 1u);
    const uint32_t dropId = room->drops()[0].dropId;
    const uint32_t itemId = room->drops()[0].itemId;
    const uint16_t quantity = room->drops()[0].quantity;

    const std::string monsterDeathKey =
        "MonsterDeath:" + std::to_string(clients.roomId) + ":" +
        std::to_string(monsterId);
    const std::optional<uint32_t> monsterDeathSequenceA =
        Core::ServerTestAccess::rudpReliableEventSequence(
            runningServer.server,
            clients.sessionA,
            Net::RudpReliableEventKind::kMonsterDeath,
            monsterDeathKey);
    const std::optional<uint32_t> monsterDeathSequenceB =
        Core::ServerTestAccess::rudpReliableEventSequence(
            runningServer.server,
            clients.sessionB,
            Net::RudpReliableEventKind::kMonsterDeath,
            monsterDeathKey);
    ASSERT_TRUE(monsterDeathSequenceA.has_value());
    ASSERT_TRUE(monsterDeathSequenceB.has_value());

    Net::RudpMonsterDeathGameEventPayload monsterDeathA;
    Net::RudpMonsterDeathGameEventPayload monsterDeathB;
    ASSERT_TRUE(receiveMonsterDeathPacketWithWait(
        senderA,
        *monsterDeathSequenceA,
        monsterDeathA));
    ASSERT_TRUE(receiveMonsterDeathPacketWithWait(
        senderB,
        *monsterDeathSequenceB,
        monsterDeathB));

    sendUdpPacket(
        senderA,
        serializeRudpPacketForTest(
            ackOnlyRudpHeader(*monsterDeathSequenceA),
            {}),
        udpPort);
    sendUdpPacket(
        senderB,
        serializeRudpPacketForTest(
            ackOnlyRudpHeader(*monsterDeathSequenceB),
            {}),
        udpPort);
    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.rudpReliableEventPendingCount() == 0U;
        }));
    ASSERT_EQ(runningServer.server.rudpReliableEventPendingCount(), 0U);
    const uint32_t expectedLootResolvedSequenceA = *monsterDeathSequenceA + 1U;
    const uint32_t expectedLootResolvedSequenceB = *monsterDeathSequenceB + 1U;

    sendUdpPacket(
        senderA,
        serializeRudpPacketForTest(
            inputCommandRudpHeader(204),
            clickLootInputCommandPayloadForTest(77, 3, dropId)),
        udpPort);

    Net::RudpLootResolvedGameEventPayload lootResolvedA;
    Net::RudpLootResolvedGameEventPayload lootResolvedB;
    ASSERT_TRUE(receiveLootResolvedPacketWithWait(
        senderA,
        expectedLootResolvedSequenceA,
        lootResolvedA));
    ASSERT_TRUE(receiveLootResolvedPacketWithWait(
        senderB,
        expectedLootResolvedSequenceB,
        lootResolvedB));
    EXPECT_EQ(lootResolvedA.roomId, clients.roomId);
    EXPECT_EQ(lootResolvedA.dropId, dropId);
    EXPECT_EQ(lootResolvedA.winnerSessionId, clients.sessionA);
    EXPECT_EQ(lootResolvedB.roomId, clients.roomId);
    EXPECT_EQ(lootResolvedB.dropId, dropId);
    EXPECT_EQ(lootResolvedB.winnerSessionId, clients.sessionA);

    uint32_t rankingRoomId = 0;
    uint64_t battleInstanceId = 0;
    std::vector<Net::BattleFinalRankingEntry> rankingRows;
    ASSERT_TRUE(recvBattleFinalRankingPacket(
        clients.clientA,
        rankingRoomId,
        battleInstanceId,
        rankingRows));
    EXPECT_EQ(rankingRoomId, clients.roomId);
    EXPECT_GT(battleInstanceId, 0u);
    ASSERT_EQ(rankingRows.size(), 2U);
    EXPECT_EQ(rankingRows[0].sessionId, clients.sessionA);
    EXPECT_EQ(rankingRows[0].totalAssetValue, 100);

    uint32_t rankingRoomIdB = 0;
    uint64_t battleInstanceIdB = 0;
    std::vector<Net::BattleFinalRankingEntry> rankingRowsB;
    ASSERT_TRUE(recvBattleFinalRankingPacket(
        clients.clientB,
        rankingRoomIdB,
        battleInstanceIdB,
        rankingRowsB));
    EXPECT_EQ(rankingRoomIdB, clients.roomId);
    EXPECT_EQ(battleInstanceIdB, battleInstanceId);
    ASSERT_EQ(rankingRowsB.size(), rankingRows.size());

    uint32_t previousRoomId = 0;
    Net::TcpLobbyReturnReason returnReason = Net::TcpLobbyReturnReason::kHostKick;
    ASSERT_TRUE(recvLobbyReturnVisibilityPacket(clients.clientA, previousRoomId, returnReason));
    EXPECT_EQ(previousRoomId, clients.roomId);
    EXPECT_EQ(returnReason, Net::TcpLobbyReturnReason::kNone);
    ASSERT_TRUE(recvLobbyReturnVisibilityPacket(clients.clientB, previousRoomId, returnReason));
    EXPECT_EQ(previousRoomId, clients.roomId);
    EXPECT_EQ(returnReason, Net::TcpLobbyReturnReason::kNone);

    std::vector<Net::TcpRoomEntry> roomSnapshot;
    ASSERT_TRUE(recvRoomListSnapshotPacket(clients.clientA, roomSnapshot));
    EXPECT_TRUE(roomSnapshot.empty());
    ASSERT_TRUE(recvRoomListSnapshotPacket(clients.clientB, roomSnapshot));
    EXPECT_TRUE(roomSnapshot.empty());

    room = Core::ServerTestAccess::findRoom(runningServer.server, clients.roomId);
    EXPECT_EQ(room, nullptr);

    ::close(clients.clientB);
    ::close(clients.clientA);
}

TEST(ServerIntegrationTests, RudpReliableEventAcksConsumePendingEventQueue) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TwoPlayerRoomClients clients;
    ASSERT_TRUE(setupTwoPlayerRoomForRudpTest(runningServer, clients));

    Net::UdpSocket senderA;
    Net::UdpSocket senderB;
    ASSERT_TRUE(senderA.open(0));
    ASSERT_TRUE(senderB.open(0));
    ASSERT_TRUE(bindRudpEndpointForSessionTest(
        runningServer,
        senderA,
        clients.sessionA,
        77,
        100,
        1));
    ASSERT_TRUE(bindRudpEndpointForSessionTest(
        runningServer,
        senderB,
        clients.sessionB,
        78,
        101,
        2));
    ASSERT_TRUE(startRudpBattleForTest(
        runningServer,
        clients,
        senderA,
        senderB,
        udpPort));
    ASSERT_EQ(runningServer.server.rudpReliableEventPendingCount(), 2U);

    Net::RudpPacketHeader deliveredAckHeader = reliableRudpHeader(301);
    deliveredAckHeader.ack = clients.battleStartSequenceA;
    sendUdpPacket(
        senderA,
        serializeRudpPacketForTest(deliveredAckHeader, {0x01}),
        udpPort);
    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.rudpReliableEventPendingCount() == 1U;
        }));

    sendUdpPacket(
        senderB,
        serializeRudpPacketForTest(
            ackOnlyRudpHeader(clients.battleStartSequenceB),
            {}),
        udpPort);
    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.rudpReliableEventPendingCount() == 0U;
        }));
    EXPECT_EQ(
        Core::ServerTestAccess::rudpReliableEventSessionQueueCount(
            runningServer.server),
        0U);
    EXPECT_EQ(
        Core::ServerTestAccess::rudpReliableEventSequenceAllocatorCount(
            runningServer.server),
        2U);

    ::close(clients.clientB);
    ::close(clients.clientA);
}

TEST(ServerIntegrationTests, RudpReliableEventMissingAckRetransmitsStoredPacket) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t udpPort = runningServer.server.udpBoundPort();
    ASSERT_GT(udpPort, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TwoPlayerRoomClients clients;
    ASSERT_TRUE(setupTwoPlayerRoomForRudpTest(runningServer, clients));

    Net::UdpSocket senderA;
    Net::UdpSocket senderB;
    ASSERT_TRUE(senderA.open(0));
    ASSERT_TRUE(senderB.open(0));
    ASSERT_TRUE(bindRudpEndpointForSessionTest(
        runningServer,
        senderA,
        clients.sessionA,
        77,
        100,
        1));
    ASSERT_TRUE(bindRudpEndpointForSessionTest(
        runningServer,
        senderB,
        clients.sessionB,
        78,
        101,
        2));
    ASSERT_TRUE(startRudpBattleForTest(
        runningServer,
        clients,
        senderA,
        senderB,
        udpPort));

    Net::RudpBattleStartPayload retransmitted;
    ASSERT_TRUE(receiveBattleStartPacketWithWait(
        senderA,
        clients.battleStartSequenceA,
        retransmitted,
        std::chrono::milliseconds(800)));
    EXPECT_EQ(retransmitted.roomId, clients.roomId);
    EXPECT_EQ(runningServer.server.rudpReliableEventPendingCount(), 2U);
    EXPECT_GE(runningServer.server.rudpRetransmissionStats().resent, 1U);

    ::close(clients.clientB);
    ::close(clients.clientA);
}

TEST(ServerIntegrationTests, RudpReliableEventMaxRetryExpiryDropsPendingOnly) {
    Core::Server server(0);
    const Net::RudpReliableEventDescriptor descriptor =
        reliableEventDescriptorForTest(
            Net::RudpReliableEventKind::kBattleStart,
            "battle-expire");
    ASSERT_EQ(
        Core::ServerTestAccess::trackRudpReliableEvent(
            server,
            1001,
            descriptor,
            50,
            reliableEventPacketBytesForTest(0x50),
            timeAt(1000)),
        Core::RudpServerReliableEventTrackResult::kTracked);

    auto sentAt = timeAt(1000);
    for (uint32_t retry = 0;
         retry < Net::RudpReliableSendQueue::kDefaultMaxRetransmissions;
         ++retry) {
        sentAt += Net::RudpReliableSendQueue::kDefaultRetransmissionTimeout;
        ASSERT_TRUE(Core::ServerTestAccess::markRudpReliableEventRetransmitted(
            server,
            1001,
            50,
            sentAt));
    }

    Core::ServerTestAccess::processRudpReliableEventRetransmissions(
        server,
        sentAt + Net::RudpReliableSendQueue::kDefaultRetransmissionTimeout);

    EXPECT_EQ(server.rudpReliableEventPendingCount(), 0U);
    EXPECT_EQ(Core::ServerTestAccess::rudpReliableEventSessionQueueCount(server), 0U);
    EXPECT_EQ(server.rudpRetransmissionStats().expired, 1U);
    const std::string textfile = server.renderRelease1RuntimeMetricsTextfile();
    EXPECT_NE(
        textfile.find(
            "lol_rudp_retransmission_expired_by_kind_total{kind=\"battle_start\"} 1\n"),
        std::string::npos);
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
                stats.inputSequenceAmbiguousRejected == 0U &&
                runningServer.server.rudpReliableEventPendingCount() == 0U;
        }));

    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(50)));
    std::vector<uint8_t> unexpectedPacket;
    EXPECT_FALSE(recvPacket(clientFd, unexpectedPacket));

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
                stats.inputSequenceAmbiguousRejected == 0U &&
                runningServer.server.rudpReliableEventPendingCount() == 0U;
        }));

    expectNoTcpPacketForTest(clientFd);

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
                stats.inputSequenceAmbiguousRejected == 0U &&
                runningServer.server.rudpReliableEventPendingCount() == 0U;
        }));

    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(50)));
    std::vector<uint8_t> unexpectedPacket;
    EXPECT_FALSE(recvPacket(clientFd, unexpectedPacket));

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
                stats.inputSequenceStaleRejected == 0U &&
                runningServer.server.rudpReliableEventPendingCount() == 0U;
        }));

    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(50)));
    std::vector<uint8_t> unexpectedPacket;
    EXPECT_FALSE(recvPacket(clientFd, unexpectedPacket));

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

TEST(ServerIntegrationTests, DoesNotSendWelcomeBeforeAuthenticateGameSessionClaimSuccess) {
    TestMetaSessionClaimClient meta(false);
    RunningServer runningServer(0, &meta);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const int clientFd = connectRawToServer(port);
    ASSERT_GE(clientFd, 0);
    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(500)));

    expectNoTcpPacketForTest(clientFd);
    ASSERT_TRUE(sendAuthenticateGameSessionPacket(clientFd, "token-1"));
    ASSERT_TRUE(waitUntil([&meta]() { return meta.hasPendingClaim("token-1"); }));
    expectNoTcpPacketForTest(clientFd);

    ASSERT_TRUE(meta.completeClaim("token-1", acceptedClaim(123, "player123")));
    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(500)));

    uint64_t sessionId = 0;
    ASSERT_TRUE(recvWelcomePacket(clientFd, sessionId));
    EXPECT_GT(sessionId, 0u);

    std::vector<Net::TcpRoomEntry> rooms;
    ASSERT_TRUE(recvRoomListSnapshotPacket(clientFd, rooms));
    EXPECT_TRUE(rooms.empty());
    EXPECT_EQ(runningServer.server.sessionCount(), 1u);

    ::close(clientFd);
}

TEST(ServerIntegrationTests, PreAuthCreateRoomIsIgnoredAndNotReplayedAfterAuth) {
    TestMetaSessionClaimClient meta(false);
    RunningServer runningServer(0, &meta);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const int clientFd = connectRawToServer(port);
    ASSERT_GE(clientFd, 0);
    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(500)));

    ASSERT_TRUE(sendCreateRoomRequestPacket(clientFd));
    expectNoTcpPacketForTest(clientFd);
    EXPECT_EQ(Core::ServerTestAccess::roomCount(runningServer.server), 0u);

    ASSERT_TRUE(sendAuthenticateGameSessionPacket(clientFd, "token-2"));
    ASSERT_TRUE(waitUntil([&meta]() { return meta.hasPendingClaim("token-2"); }));
    ASSERT_TRUE(meta.completeClaim("token-2", acceptedClaim(124, "player124")));
    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(500)));

    uint64_t sessionId = 0;
    ASSERT_TRUE(recvWelcomePacket(clientFd, sessionId));
    std::vector<Net::TcpRoomEntry> rooms;
    ASSERT_TRUE(recvRoomListSnapshotPacket(clientFd, rooms));
    EXPECT_TRUE(rooms.empty());
    EXPECT_EQ(Core::ServerTestAccess::roomCount(runningServer.server), 0u);

    ::close(clientFd);
}

TEST(ServerIntegrationTests, AuthSuccessSendsWelcomeThenInitialRoomListSnapshot) {
    TestMetaSessionClaimClient meta(false);
    RunningServer runningServer(0, &meta);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const int clientFd = connectRawToServer(port);
    ASSERT_GE(clientFd, 0);
    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(500)));

    ASSERT_TRUE(sendAuthenticateGameSessionPacket(clientFd, "token-3"));
    ASSERT_TRUE(waitUntil([&meta]() { return meta.hasPendingClaim("token-3"); }));
    ASSERT_TRUE(meta.completeClaim("token-3", acceptedClaim(125, "player125")));

    uint64_t sessionId = 0;
    ASSERT_TRUE(recvWelcomePacket(clientFd, sessionId));
    EXPECT_GT(sessionId, 0u);

    std::vector<Net::TcpRoomEntry> rooms;
    ASSERT_TRUE(recvRoomListSnapshotPacket(clientFd, rooms));
    EXPECT_TRUE(rooms.empty());

    ::close(clientFd);
}

TEST(ServerIntegrationTests, ClaimRejectedClosesWithoutLobbyMembership) {
    TestMetaSessionClaimClient meta(false);
    RunningServer runningServer(0, &meta);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const int clientFd = connectRawToServer(port);
    ASSERT_GE(clientFd, 0);
    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(500)));

    ASSERT_TRUE(sendAuthenticateGameSessionPacket(clientFd, "token-rejected"));
    ASSERT_TRUE(waitUntil([&meta]() { return meta.hasPendingClaim("token-rejected"); }));
    ASSERT_TRUE(meta.completeClaim("token-rejected", rejectedClaim()));

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                runningServer.server.sessionCount() == 0u;
        }));
    EXPECT_EQ(Core::ServerTestAccess::roomCount(runningServer.server), 0u);
    EXPECT_FALSE(expectConnectionAlive(clientFd));
}

TEST(ServerIntegrationTests, SameAccountSecondClaimReplacesOldSession) {
    TestMetaSessionClaimClient meta(false);
    RunningServer runningServer(0, &meta);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const int firstFd = connectRawToServer(port);
    ASSERT_GE(firstFd, 0);
    ASSERT_TRUE(setReceiveTimeout(firstFd, std::chrono::milliseconds(500)));
    ASSERT_TRUE(sendAuthenticateGameSessionPacket(firstFd, "token-old"));
    ASSERT_TRUE(waitUntil([&meta]() { return meta.hasPendingClaim("token-old"); }));
    ASSERT_TRUE(meta.completeClaim("token-old", acceptedClaim(777, "player777")));

    uint64_t firstSessionId = 0;
    ASSERT_TRUE(recvWelcomePacket(firstFd, firstSessionId));
    std::vector<Net::TcpRoomEntry> rooms;
    ASSERT_TRUE(recvRoomListSnapshotPacket(firstFd, rooms));
    drainAvailableTcpPacketsForTest(firstFd);

    const int secondFd = connectRawToServer(port);
    ASSERT_GE(secondFd, 0);
    ASSERT_TRUE(setReceiveTimeout(secondFd, std::chrono::milliseconds(500)));
    ASSERT_TRUE(sendAuthenticateGameSessionPacket(secondFd, "token-new"));
    ASSERT_TRUE(waitUntil([&meta]() { return meta.hasPendingClaim("token-new"); }));
    ASSERT_TRUE(meta.completeClaim("token-new", acceptedClaim(777, "player777")));

    ASSERT_TRUE(recvSessionReplacedPacket(firstFd));
    EXPECT_TRUE(waitUntil([firstFd]() { return !expectConnectionAlive(firstFd); }));

    uint64_t secondSessionId = 0;
    ASSERT_TRUE(recvWelcomePacket(secondFd, secondSessionId));
    ASSERT_TRUE(recvRoomListSnapshotPacket(secondFd, rooms));
    EXPECT_GT(secondSessionId, 0u);
    EXPECT_NE(firstSessionId, secondSessionId);
    EXPECT_EQ(runningServer.server.sessionCount(), 1u);

    ::close(secondFd);
}

TEST(ServerIntegrationTests, SameAccountReplacementReleasesOldThenNewSession) {
    TestMetaSessionClaimClient meta(false);
    RunningServer runningServer(0, &meta);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const int firstFd = connectRawToServer(port);
    ASSERT_GE(firstFd, 0);
    ASSERT_TRUE(setReceiveTimeout(firstFd, std::chrono::milliseconds(500)));
    ASSERT_TRUE(sendAuthenticateGameSessionPacket(firstFd, "token-replace-old-release"));
    ASSERT_TRUE(waitUntil(
        [&meta]() { return meta.hasPendingClaim("token-replace-old-release"); }));
    ASSERT_TRUE(meta.completeClaim(
        "token-replace-old-release",
        acceptedClaim(778, "player778")));

    uint64_t firstSessionId = 0;
    ASSERT_TRUE(recvWelcomePacket(firstFd, firstSessionId));
    std::vector<Net::TcpRoomEntry> rooms;
    ASSERT_TRUE(recvRoomListSnapshotPacket(firstFd, rooms));
    drainAvailableTcpPacketsForTest(firstFd);

    const int secondFd = connectRawToServer(port);
    ASSERT_GE(secondFd, 0);
    ASSERT_TRUE(setReceiveTimeout(secondFd, std::chrono::milliseconds(500)));
    ASSERT_TRUE(sendAuthenticateGameSessionPacket(secondFd, "token-replace-new-release"));
    ASSERT_TRUE(waitUntil(
        [&meta]() { return meta.hasPendingClaim("token-replace-new-release"); }));
    ASSERT_TRUE(meta.completeClaim(
        "token-replace-new-release",
        acceptedClaim(778, "player778")));

    ASSERT_TRUE(recvSessionReplacedPacket(firstFd));
    EXPECT_TRUE(waitUntil([firstFd]() { return !expectConnectionAlive(firstFd); }));
    ASSERT_TRUE(waitUntil([&meta]() { return meta.releaseCount() == 1u; }));

    uint64_t secondSessionId = 0;
    ASSERT_TRUE(recvWelcomePacket(secondFd, secondSessionId));
    ASSERT_TRUE(recvRoomListSnapshotPacket(secondFd, rooms));
    EXPECT_GT(secondSessionId, 0u);
    EXPECT_NE(firstSessionId, secondSessionId);

    std::vector<Core::MetaSessionReleaseRequest> releases = meta.releaseRequests();
    ASSERT_EQ(releases.size(), 1u);
    EXPECT_EQ(releases[0].accountId, 778u);
    EXPECT_EQ(releases[0].serverSessionId, firstSessionId);
    EXPECT_GT(releases[0].connectionId, 0u);

    ::close(secondFd);
    ASSERT_TRUE(waitUntil([&meta]() { return meta.releaseCount() == 2u; }));

    releases = meta.releaseRequests();
    ASSERT_EQ(releases.size(), 2u);
    EXPECT_EQ(releases[1].accountId, 778u);
    EXPECT_EQ(releases[1].serverSessionId, secondSessionId);
    EXPECT_GT(releases[1].connectionId, 0u);
}

TEST(ServerIntegrationTests, AuthenticatedDisconnectReleasesMetaSessionWithIdentity) {
    TestMetaSessionClaimClient meta(false);
    RunningServer runningServer(0, &meta);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const int clientFd = connectRawToServer(port);
    ASSERT_GE(clientFd, 0);
    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(500)));
    ASSERT_TRUE(sendAuthenticateGameSessionPacket(clientFd, "token-release"));
    ASSERT_TRUE(waitUntil([&meta]() { return meta.hasPendingClaim("token-release"); }));
    ASSERT_TRUE(meta.completeClaim("token-release", acceptedClaim(888, "player888")));

    uint64_t sessionId = 0;
    ASSERT_TRUE(recvWelcomePacket(clientFd, sessionId));
    std::vector<Net::TcpRoomEntry> rooms;
    ASSERT_TRUE(recvRoomListSnapshotPacket(clientFd, rooms));

    ::close(clientFd);
    ASSERT_TRUE(waitUntil([&meta]() { return meta.releaseCount() == 1u; }));

    const std::vector<Core::MetaSessionReleaseRequest> releases = meta.releaseRequests();
    ASSERT_EQ(releases.size(), 1u);
    EXPECT_EQ(releases[0].accountId, 888u);
    EXPECT_EQ(releases[0].serverSessionId, sessionId);
    EXPECT_GT(releases[0].connectionId, 0u);
}

TEST(ServerIntegrationTests, AuthenticatedSessionReportsLivenessAfterInterval) {
    TestMetaSessionClaimClient meta(false);
    RunningServer runningServer(0, &meta);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const int clientFd = connectRawToServer(port);
    ASSERT_GE(clientFd, 0);
    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(500)));
    ASSERT_TRUE(sendAuthenticateGameSessionPacket(clientFd, "token-liveness"));
    ASSERT_TRUE(waitUntil([&meta]() { return meta.hasPendingClaim("token-liveness"); }));
    ASSERT_TRUE(meta.completeClaim("token-liveness", acceptedClaim(889, "player889")));

    uint64_t sessionId = 0;
    ASSERT_TRUE(recvWelcomePacket(clientFd, sessionId));
    std::vector<Net::TcpRoomEntry> rooms;
    ASSERT_TRUE(recvRoomListSnapshotPacket(clientFd, rooms));

    Core::ServerTestAccess::processMetaSessionLivenessReports(
        runningServer.server,
        Util::now() + std::chrono::minutes(4));
    EXPECT_EQ(meta.renewCount(), 0u);

    Core::ServerTestAccess::processMetaSessionLivenessReports(
        runningServer.server,
        Util::now() + std::chrono::minutes(6));
    ASSERT_EQ(meta.renewCount(), 1u);

    const std::vector<Core::MetaSessionRenewRequest> renews = meta.renewRequests();
    ASSERT_EQ(renews.size(), 1u);
    EXPECT_EQ(renews[0].accountId, 889u);
    EXPECT_GT(renews[0].connectionId, 0u);

    ::close(clientFd);
}

TEST(ServerIntegrationTests, ExpiredAcceptedClaimClosesWithoutWelcomeOrLobbyMembership) {
    TestMetaSessionClaimClient meta(false);
    RunningServer runningServer(0, &meta);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const int clientFd = connectRawToServer(port);
    ASSERT_GE(clientFd, 0);
    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(500)));

    ASSERT_TRUE(sendAuthenticateGameSessionPacket(clientFd, "token-expired-accepted"));
    ASSERT_TRUE(waitUntil(
        [&meta]() { return meta.hasPendingClaim("token-expired-accepted"); }));

    Core::MetaSessionClaimResult expired = acceptedClaim(901, "expired901");
    expired.reservationExpiresAtUnixMs = unixTimeMsForTest(-1);
    ASSERT_TRUE(meta.completeClaim("token-expired-accepted", expired));

    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                runningServer.server.sessionCount() == 0u;
        }));
    EXPECT_EQ(Core::ServerTestAccess::roomCount(runningServer.server), 0u);
    EXPECT_FALSE(expectConnectionAlive(clientFd));

    ASSERT_TRUE(waitUntil([&meta]() { return meta.releaseCount() == 1u; }));
    const std::vector<Core::MetaSessionReleaseRequest> releases = meta.releaseRequests();
    ASSERT_EQ(releases.size(), 1u);
    EXPECT_EQ(releases[0].accountId, 901u);
    EXPECT_EQ(releases[0].serverSessionId, 0u);
    EXPECT_GT(releases[0].connectionId, 0u);
}

TEST(ServerIntegrationTests, ClaimPendingClientDoesNotReceiveClientListSnapshot) {
    TestMetaSessionClaimClient meta(false);
    RunningServer runningServer(0, &meta);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const int pendingFd = connectRawToServer(port);
    ASSERT_GE(pendingFd, 0);
    ASSERT_TRUE(setReceiveTimeout(pendingFd, std::chrono::milliseconds(500)));
    ASSERT_TRUE(sendAuthenticateGameSessionPacket(pendingFd, "token-still-pending"));
    ASSERT_TRUE(waitUntil(
        [&meta]() { return meta.hasPendingClaim("token-still-pending"); }));
    expectNoTcpPacketForTest(pendingFd);

    const int activeFd = connectRawToServer(port);
    ASSERT_GE(activeFd, 0);
    ASSERT_TRUE(setReceiveTimeout(activeFd, std::chrono::milliseconds(500)));
    ASSERT_TRUE(sendAuthenticateGameSessionPacket(activeFd, "token-active-visible"));
    ASSERT_TRUE(waitUntil(
        [&meta]() { return meta.hasPendingClaim("token-active-visible"); }));
    ASSERT_TRUE(meta.completeClaim(
        "token-active-visible",
        acceptedClaim(902, "active902")));

    uint64_t activeSessionId = 0;
    ASSERT_TRUE(recvWelcomePacket(activeFd, activeSessionId));
    std::vector<Net::TcpRoomEntry> rooms;
    ASSERT_TRUE(recvRoomListSnapshotPacket(activeFd, rooms));
    EXPECT_TRUE(waitUntil(
        [&runningServer]() { return runningServer.server.sessionCount() == 1u; }));
    expectNoTcpPacketForTest(pendingFd);

    ::close(activeFd);
    ASSERT_TRUE(waitUntil(
        [&runningServer]() { return runningServer.server.sessionCount() == 0u; }));
    expectNoTcpPacketForTest(pendingFd);

    ::close(pendingFd);
}

TEST(ServerIntegrationTests, PreWelcomeDisconnectLateAcceptedClaimReleasesMetaSlot) {
    TestMetaSessionClaimClient meta(false);
    RunningServer runningServer(0, &meta);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const int clientFd = connectRawToServer(port);
    ASSERT_GE(clientFd, 0);
    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(500)));
    ASSERT_TRUE(sendAuthenticateGameSessionPacket(clientFd, "token-late-accepted"));
    ASSERT_TRUE(waitUntil(
        [&meta]() { return meta.hasPendingClaim("token-late-accepted"); }));

    ::close(clientFd);
    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                runningServer.server.sessionCount() == 0u;
        }));

    ASSERT_TRUE(meta.completeClaim(
        "token-late-accepted",
        acceptedClaim(903, "late903")));

    ASSERT_TRUE(waitUntil([&meta]() { return meta.releaseCount() == 1u; }));
    EXPECT_EQ(runningServer.server.sessionCount(), 0u);
    EXPECT_EQ(Core::ServerTestAccess::roomCount(runningServer.server), 0u);

    const std::vector<Core::MetaSessionReleaseRequest> releases = meta.releaseRequests();
    ASSERT_EQ(releases.size(), 1u);
    EXPECT_EQ(releases[0].accountId, 903u);
    EXPECT_EQ(releases[0].serverSessionId, 0u);
    EXPECT_GT(releases[0].connectionId, 0u);
}

TEST(ServerIntegrationTests, ShutdownLateAcceptedClaimReleasesMetaSlotWithoutServer) {
    TestMetaSessionClaimClient meta(false);
    std::optional<Core::IMetaSessionClaimClient::ClaimCallback> lateCallback;

    {
        RunningServer runningServer(0, &meta);
        ASSERT_TRUE(runningServer.server.start());
        const uint16_t port = runningServer.server.boundPort();
        ASSERT_GT(port, 0);

        runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        const int clientFd = connectRawToServer(port);
        ASSERT_GE(clientFd, 0);
        ASSERT_TRUE(sendAuthenticateGameSessionPacket(clientFd, "token-shutdown-late"));
        ASSERT_TRUE(waitUntil(
            [&meta]() { return meta.hasPendingClaim("token-shutdown-late"); }));

        lateCallback = meta.takePendingClaimCallback("token-shutdown-late");
        ASSERT_TRUE(lateCallback.has_value());
        ::close(clientFd);
    }

    std::move(*lateCallback)(acceptedClaim(904, "shutdown904"));

    ASSERT_TRUE(waitUntil([&meta]() { return meta.releaseCount() == 1u; }));
    const std::vector<Core::MetaSessionReleaseRequest> releases = meta.releaseRequests();
    ASSERT_EQ(releases.size(), 1u);
    EXPECT_EQ(releases[0].accountId, 904u);
    EXPECT_EQ(releases[0].serverSessionId, 0u);
    EXPECT_GT(releases[0].connectionId, 0u);
}

TEST(ServerIntegrationTests, BlockingMetaClaimDoesNotBlockAcceptingAnotherTcpClient) {
    BlockingMetaSessionClaimClient meta;
    RunningServer runningServer(0, &meta);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const int blockedFd = connectRawToServer(port);
    ASSERT_GE(blockedFd, 0);
    ASSERT_TRUE(sendAuthenticateGameSessionPacket(blockedFd, "token-blocking-meta"));
    ASSERT_TRUE(meta.waitForFirstClaimStarted());

    const int secondFd = connectRawToServer(port);
    ASSERT_GE(secondFd, 0);
    ASSERT_TRUE(sendAuthenticateGameSessionPacket(secondFd, "token-second-meta"));

    const bool acceptedSecondConnection = waitUntil(
        std::function<bool()>(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() >= 2u;
        }),
        std::chrono::milliseconds(300));

    meta.unblockFirstClaim();
    EXPECT_TRUE(acceptedSecondConnection);

    ::close(blockedFd);
    ::close(secondFd);
}

TEST(ServerIntegrationTests, SendsWelcomePacketAfterAuthClaimSuccess) {
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

TEST(ServerIntegrationTests, AuthenticatedMalformedCreateRoomRequestDoesNotCreateLobbyRoom) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    int creatorFd = connectToServer(port);
    ASSERT_GE(creatorFd, 0);
    ASSERT_TRUE(setReceiveTimeout(creatorFd, std::chrono::milliseconds(500)));

    uint64_t creatorSessionId = 0;
    ASSERT_TRUE(recvWelcomePacket(creatorFd, creatorSessionId));
    ASSERT_GT(creatorSessionId, 0u);

    std::vector<Net::TcpRoomEntry> rooms;
    ASSERT_TRUE(recvRoomListSnapshotPacket(creatorFd, rooms));
    ASSERT_TRUE(rooms.empty());

    std::vector<uint8_t> malformedCreateRoom;
    ASSERT_TRUE(Net::serializeCreateRoomRequestPacket("BadCap", 10, malformedCreateRoom));
    malformedCreateRoom.back() = 1;
    ASSERT_TRUE(sendAll(creatorFd, malformedCreateRoom.data(), malformedCreateRoom.size()));
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
    EXPECT_EQ(Core::ServerTestAccess::roomCount(runningServer.server), 0u);

    int observerFd = connectToServer(port);
    ASSERT_GE(observerFd, 0);
    ASSERT_TRUE(setReceiveTimeout(observerFd, std::chrono::milliseconds(500)));

    uint64_t observerSessionId = 0;
    ASSERT_TRUE(recvWelcomePacket(observerFd, observerSessionId));
    ASSERT_GT(observerSessionId, 0u);

    rooms.clear();
    ASSERT_TRUE(recvRoomListSnapshotPacket(observerFd, rooms));
    EXPECT_TRUE(rooms.empty());
    EXPECT_EQ(Core::ServerTestAccess::roomCount(runningServer.server), 0u);

    ::close(creatorFd);
    ::close(observerFd);
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

TEST(ServerIntegrationTests, CoalescesBattleResultRoomListSnapshotBroadcastsForOneFlush) {
    Core::Server server(0);

    const Game::RoomCommandResult roomA =
        Core::ServerTestAccess::createRoom(server, 1001);
    const Game::RoomCommandResult roomB =
        Core::ServerTestAccess::createRoom(server, 2001);
    ASSERT_TRUE(roomA.ok);
    ASSERT_TRUE(roomB.ok);

    Game::BattleFinalRankingResult rankingA;
    rankingA.status = Game::BattleFinalRankingStatus::kOk;
    rankingA.roomId = roomA.room.roomId;
    rankingA.battleInstanceId = 101;
    rankingA.participantSessionIds = {1001};
    rankingA.rows = {Game::BattleFinalRankingRow{1, 1001, "Player1001", 0}};

    Game::BattleFinalRankingResult rankingB;
    rankingB.status = Game::BattleFinalRankingStatus::kOk;
    rankingB.roomId = roomB.room.roomId;
    rankingB.battleInstanceId = 102;
    rankingB.participantSessionIds = {2001};
    rankingB.rows = {Game::BattleFinalRankingRow{1, 2001, "Player2001", 0}};

    ASSERT_TRUE(Core::ServerTestAccess::completeBattleResultsThenBroadcastRoomList(
        server,
        {rankingA, rankingB}));

    const Core::Release1RuntimeMetricsSnapshot metrics =
        server.release1RuntimeMetricsSnapshot();
    EXPECT_EQ(metrics.tcpRoomListSnapshotBroadcastCount, 1U);
    EXPECT_EQ(metrics.tcpRoomListSnapshotBroadcastRecipientCount, 0U);
    EXPECT_EQ(Core::ServerTestAccess::findRoom(server, roomA.room.roomId), nullptr);
    EXPECT_EQ(Core::ServerTestAccess::findRoom(server, roomB.room.roomId), nullptr);
}

TEST(ServerIntegrationTests, DefersRoomListBroadcastUntilDebounceWindow) {
    Core::Server server(0);

    Core::ServerTestAccess::broadcastStateSnapshots(
        server,
        false,
        true,
        timeAt(1000));
    Core::ServerTestAccess::broadcastStateSnapshots(
        server,
        false,
        true,
        timeAt(1020));

    EXPECT_EQ(server.release1RuntimeMetricsSnapshot().tcpRoomListSnapshotBroadcastCount, 0U);

    Core::ServerTestAccess::processPendingRoomListSnapshotBroadcast(server, timeAt(1049));
    EXPECT_EQ(server.release1RuntimeMetricsSnapshot().tcpRoomListSnapshotBroadcastCount, 0U);

    Core::ServerTestAccess::processPendingRoomListSnapshotBroadcast(server, timeAt(1050));
    EXPECT_EQ(server.release1RuntimeMetricsSnapshot().tcpRoomListSnapshotBroadcastCount, 1U);
}

TEST(ServerIntegrationTests, Release1RuntimeMetricsSnapshotCountsCompletedTimerTicks) {
    Core::Server server(0);

    EXPECT_EQ(server.release1RuntimeMetricsSnapshot().runtimeTickCount, 0U);

    Core::ServerTestAccess::processRuntimeTimerMaintenanceForRelease1Metrics(
        server,
        timeAt(1000));

    EXPECT_EQ(server.release1RuntimeMetricsSnapshot().runtimeTickCount, 1U);
}

TEST(ServerIntegrationTests, Release1RuntimeMetricsRenderTextfile) {
    Core::Server server(0);
    Core::ServerTestAccess::processRuntimeTimerMaintenanceForRelease1Metrics(
        server,
        timeAt(1000));

    const std::string textfile = server.renderRelease1RuntimeMetricsTextfile();

    EXPECT_NE(
        textfile.find("lol_server_runtime_tick_total 1\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_input_sequence_accepted_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_move_accepted_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_rudp_move_receive_to_apply_latency_ms_bucket{le=\"1\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_move_receive_to_apply_latency_ms_count 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_move_receive_to_apply_latency_ms_sum 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_attack_accepted_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_rudp_attack_receive_to_apply_latency_ms_bucket{le=\"1\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_attack_receive_to_apply_latency_ms_count 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_attack_receive_to_apply_latency_ms_sum 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_loot_claim_accepted_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_rudp_loot_claim_receive_to_apply_latency_ms_bucket{le=\"1\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_loot_claim_receive_to_apply_latency_ms_count 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_loot_claim_receive_to_apply_latency_ms_sum 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_state_snapshot_sent_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_drain_delivered_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_drain_attempted_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_drain_malformed_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_drain_duplicate_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_drain_socket_errors_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_drain_stopped_by_would_block_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_drain_stopped_by_max_packets_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_drain_stopped_by_socket_error_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_binding_hello_received_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_binding_bound_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_binding_refreshed_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_binding_unknown_session_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_binding_unbound_input_rejected_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_binding_input_candidates_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_binding_input_decode_failed_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_input_sequence_duplicate_rejected_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_binding_input_no_room_rejected_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_rudp_binding_input_no_room_rejected_by_op_total{op=\"ready\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_rudp_binding_input_no_room_rejected_by_op_total{op=\"move\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_rudp_binding_input_no_room_rejected_by_op_total{op=\"attack\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_rudp_binding_input_no_room_rejected_by_op_total{op=\"click_loot\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_move_apply_rejected_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_reliable_event_tracked_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_rudp_reliable_event_tracked_by_kind_total{kind=\"battle_start_roster\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_reliable_event_pending 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_retransmission_expired_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_rudp_retransmission_expired_by_kind_total{kind=\"battle_start_roster\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_retransmission_due_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_retransmission_resent_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_retransmission_send_errors_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_rudp_retransmission_dropped_peers_total 0\n"),
        std::string::npos);
    EXPECT_NE(textfile.find("lol_tcp_disconnect_total 0\n"), std::string::npos);
    EXPECT_NE(textfile.find("lol_tcp_disconnect_marked_read_closed_total 0\n"), std::string::npos);
    EXPECT_NE(textfile.find("lol_tcp_disconnect_marked_read_error_total 0\n"), std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_tcp_disconnect_marked_read_error_by_errno_total{errno=\"econnreset\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_tcp_disconnect_marked_read_error_by_errno_total{errno=\"econnaborted\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_tcp_disconnect_marked_read_error_by_errno_total{errno=\"etimedout\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_tcp_disconnect_marked_read_error_by_errno_total{errno=\"other\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_tcp_disconnect_marked_packet_reader_rejected_total 0\n"),
        std::string::npos);
    EXPECT_NE(textfile.find("lol_tcp_disconnect_marked_invalid_packet_total 0\n"), std::string::npos);
    EXPECT_NE(textfile.find("lol_tcp_disconnect_marked_network_event_total 0\n"), std::string::npos);
    EXPECT_NE(textfile.find("lol_tcp_disconnect_marked_send_failure_total 0\n"), std::string::npos);
    EXPECT_NE(
        textfile.find("lol_tcp_disconnect_marked_outbound_queue_full_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_tcp_disconnect_marked_event_loop_update_failure_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_tcp_disconnect_marked_missing_connection_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_tcp_send_failure_by_packet_type_total{packet=\"room_list_snapshot\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_tcp_send_failure_by_packet_type_total{packet=\"battle_start_roster\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_tcp_send_failure_by_packet_type_total{packet=\"arena_gameplay_start\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_tcp_send_failure_by_packet_type_total{packet=\"battle_final_ranking\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_tcp_send_failure_by_packet_type_total{packet=\"lobby_return_visibility\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_tcp_room_command_request_received_total{command=\"create_room\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_tcp_room_command_request_received_total{command=\"join_room\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_tcp_room_command_response_sent_total{command=\"create_room\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_tcp_room_command_response_sent_total{command=\"join_room\"} 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_tcp_send_failure_unknown_packet_type_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_tcp_room_list_snapshot_direct_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_tcp_room_list_snapshot_broadcast_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_tcp_room_list_snapshot_broadcast_recipients_total 0\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find("lol_tcp_room_list_snapshot_bytes_total 0\n"),
        std::string::npos);
    EXPECT_NE(textfile.find("lol_active_connections 0\n"), std::string::npos);
    EXPECT_NE(textfile.find("lol_active_sessions 0\n"), std::string::npos);
}

TEST(ServerIntegrationTests, Release1RuntimeMetricsCountsTcpReadClosedDisconnect) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    const int clientFd = connectToServer(runningServer.server.boundPort());
    ASSERT_GE(clientFd, 0);
    ASSERT_TRUE(setReceiveTimeout(clientFd, std::chrono::milliseconds(500)));

    uint64_t sessionId = 0;
    std::vector<uint64_t> snapshot;
    ASSERT_TRUE(recvWelcomePacket(clientFd, sessionId));
    ASSERT_TRUE(recvClientListSnapshotPacket(clientFd, snapshot));

    ::close(clientFd);

    ASSERT_TRUE(waitUntil(
        [&runningServer]() {
            const std::string textfile =
                runningServer.server.renderRelease1RuntimeMetricsTextfile();
            return textfile.find("lol_tcp_disconnect_total 1\n") != std::string::npos &&
                textfile.find("lol_tcp_disconnect_marked_read_closed_total 1\n") !=
                    std::string::npos;
        },
        std::chrono::milliseconds(1000)));
}

TEST(ServerIntegrationTests, Release1RuntimeMetricsCountsTcpRoomCommandVisibility) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    TwoPlayerRoomClients clients;
    ASSERT_TRUE(setupTwoPlayerRoomForRudpTest(runningServer, clients));

    const std::string textfile =
        runningServer.server.renderRelease1RuntimeMetricsTextfile();
    EXPECT_NE(
        textfile.find(
            "lol_tcp_room_command_request_received_total{command=\"create_room\"} 1\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_tcp_room_command_response_sent_total{command=\"create_room\"} 1\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_tcp_room_command_request_received_total{command=\"join_room\"} 1\n"),
        std::string::npos);
    EXPECT_NE(
        textfile.find(
            "lol_tcp_room_command_response_sent_total{command=\"join_room\"} 1\n"),
        std::string::npos);

    ::close(clients.clientB);
    ::close(clients.clientA);
}

TEST(ServerIntegrationTests, Release1RuntimeMetricsCountsAcceptedRudpAttack) {
    Core::Server server(0);
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(31901);
    ASSERT_EQ(
        Core::ServerTestAccess::bindRudpEndpoint(server, endpoint, 1001),
        Net::RudpSessionBindResult::kBoundNew);
    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        101,
        attackInputCommandPayloadForTest(77, 10, 1),
        timeAt(1000));

    EXPECT_EQ(server.release1RuntimeMetricsSnapshot().rudpAttackAcceptedCount, 1U);
    EXPECT_EQ(server.rudpBindingStats().attackAccepted, 1U);
}

TEST(ServerIntegrationTests, Release1RuntimeMetricsSamplesAcceptedRudpAttackLatency) {
    Core::Server server(0);
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(31902);
    ASSERT_EQ(
        Core::ServerTestAccess::bindRudpEndpoint(server, endpoint, 1001),
        Net::RudpSessionBindResult::kBoundNew);
    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(Core::ServerTestAccess::joinRoom(
        server,
        1002,
        created.room.roomId).ok);
    ASSERT_TRUE(Core::ServerTestAccess::markReady(server, 1001).ok);
    ASSERT_TRUE(Core::ServerTestAccess::markReady(server, 1002).ok);
    ASSERT_TRUE(
        Core::ServerTestAccess::hostStartBattle(server, 1001).battleJustStarted);
    ASSERT_TRUE(Core::ServerTestAccess::registerRoomForDispatch(
        server,
        created.room.roomId));
    const Game::RoomCommandResult spawned =
        Core::ServerTestAccess::spawnMonster(server, created.room.roomId);
    ASSERT_TRUE(spawned.ok);
    ASSERT_GT(spawned.monster.monsterId, 0u);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        101,
        attackInputCommandPayloadForTest(77, 10, spawned.monster.monsterId),
        Util::now());

    const Core::Release1RuntimeMetricsSnapshot metrics =
        server.release1RuntimeMetricsSnapshot();
    EXPECT_EQ(metrics.rudpAttackReceiveToApplyLatencySampleCount, 1U);
    EXPECT_EQ(metrics.rudpAttackReceiveToApplyLatencyBucketCounts.back(), 1U);
}

TEST(ServerIntegrationTests, Release1RuntimeMetricsSamplesAcceptedRudpMoveLatency) {
    Core::Server server(0);
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(31904);
    ASSERT_EQ(
        Core::ServerTestAccess::bindRudpEndpoint(server, endpoint, 1001),
        Net::RudpSessionBindResult::kBoundNew);
    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        101,
        moveInputCommandPayloadForTest(77, 10, 100, -100),
        Util::now());

    const Core::Release1RuntimeMetricsSnapshot metrics =
        server.release1RuntimeMetricsSnapshot();
    EXPECT_EQ(metrics.rudpMoveReceiveToApplyLatencySampleCount, 1U);
    EXPECT_EQ(metrics.rudpMoveReceiveToApplyLatencyBucketCounts.back(), 1U);
    EXPECT_EQ(metrics.rudpAttackReceiveToApplyLatencySampleCount, 0U);
}

TEST(ServerIntegrationTests, Release1RuntimeMetricsSamplesAcceptedRudpLootClaimLatency) {
    Core::Server server(0);
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(31906);
    ASSERT_EQ(
        Core::ServerTestAccess::bindRudpEndpoint(server, endpoint, 1001),
        Net::RudpSessionBindResult::kBoundNew);
    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(Core::ServerTestAccess::joinRoom(
        server,
        1002,
        created.room.roomId).ok);
    ASSERT_TRUE(Core::ServerTestAccess::markReady(server, 1001).ok);
    ASSERT_TRUE(Core::ServerTestAccess::markReady(server, 1002).ok);
    ASSERT_TRUE(
        Core::ServerTestAccess::hostStartBattle(server, 1001).battleJustStarted);
    ASSERT_TRUE(Core::ServerTestAccess::registerRoomForDispatch(
        server,
        created.room.roomId));
    const Game::RoomCommandResult spawned =
        Core::ServerTestAccess::spawnMonster(server, created.room.roomId);
    ASSERT_TRUE(spawned.ok);
    const Game::RoomCommandResult defeated =
        Core::ServerTestAccess::defeatMonster(
            server,
            1001,
            spawned.monster.monsterId);
    ASSERT_TRUE(defeated.ok);
    ASSERT_EQ(defeated.drops.size(), 1u);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        101,
        clickLootInputCommandPayloadForTest(77, 10, defeated.drops[0].dropId),
        Util::now());

    const Core::Release1RuntimeMetricsSnapshot metrics =
        server.release1RuntimeMetricsSnapshot();
    EXPECT_EQ(metrics.rudpLootClaimAcceptedCount, 1U);
    EXPECT_EQ(server.rudpBindingStats().lootClaimAccepted, 1U);
    EXPECT_EQ(metrics.rudpLootClaimReceiveToApplyLatencySampleCount, 1U);
    EXPECT_EQ(metrics.rudpLootClaimReceiveToApplyLatencyBucketCounts.back(), 1U);
    EXPECT_EQ(metrics.rudpMoveReceiveToApplyLatencySampleCount, 0U);
    EXPECT_EQ(metrics.rudpAttackReceiveToApplyLatencySampleCount, 0U);
}

TEST(ServerIntegrationTests, Release1RuntimeMetricsSkipsRejectedLootClaimLatency) {
    Core::Server server(0);
    const Net::UdpEndpoint endpointA = udpLoopbackEndpoint(31907);
    const Net::UdpEndpoint endpointB = udpLoopbackEndpoint(31908);
    ASSERT_EQ(
        Core::ServerTestAccess::bindRudpEndpoint(server, endpointA, 1001),
        Net::RudpSessionBindResult::kBoundNew);
    ASSERT_EQ(
        Core::ServerTestAccess::bindRudpEndpoint(server, endpointB, 1002),
        Net::RudpSessionBindResult::kBoundNew);
    const Game::RoomCommandResult created =
        Core::ServerTestAccess::createRoom(server, 1001);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(Core::ServerTestAccess::joinRoom(
        server,
        1002,
        created.room.roomId).ok);
    ASSERT_TRUE(Core::ServerTestAccess::markReady(server, 1001).ok);
    ASSERT_TRUE(Core::ServerTestAccess::markReady(server, 1002).ok);
    ASSERT_TRUE(
        Core::ServerTestAccess::hostStartBattle(server, 1001).battleJustStarted);
    ASSERT_TRUE(Core::ServerTestAccess::registerRoomForDispatch(
        server,
        created.room.roomId));
    const Game::RoomCommandResult spawned =
        Core::ServerTestAccess::spawnMonster(server, created.room.roomId);
    ASSERT_TRUE(spawned.ok);
    const Game::RoomCommandResult defeated =
        Core::ServerTestAccess::defeatMonster(
            server,
            1001,
            spawned.monster.monsterId);
    ASSERT_TRUE(defeated.ok);
    ASSERT_EQ(defeated.drops.size(), 1u);
    const uint32_t dropId = defeated.drops[0].dropId;

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpointA,
        101,
        clickLootInputCommandPayloadForTest(77, 10, dropId),
        Util::now());
    ASSERT_EQ(
        server.release1RuntimeMetricsSnapshot()
            .rudpLootClaimReceiveToApplyLatencySampleCount,
        1U);
    ASSERT_EQ(
        server.release1RuntimeMetricsSnapshot().rudpLootClaimAcceptedCount,
        1U);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpointB,
        102,
        clickLootInputCommandPayloadForTest(78, 10, dropId),
        Util::now());

    EXPECT_EQ(
        server.release1RuntimeMetricsSnapshot()
            .rudpLootClaimReceiveToApplyLatencySampleCount,
        1U);
    EXPECT_EQ(
        server.release1RuntimeMetricsSnapshot().rudpLootClaimAcceptedCount,
        2U);
    EXPECT_EQ(server.rudpBindingStats().lootClaimAccepted, 2U);
}

TEST(ServerIntegrationTests, Release1RuntimeMetricsSkipsRejectedMoveLatency) {
    Core::Server server(0);
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(31905);
    ASSERT_EQ(
        Core::ServerTestAccess::bindRudpEndpoint(server, endpoint, 1001),
        Net::RudpSessionBindResult::kBoundNew);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        101,
        moveInputCommandPayloadForTest(77, 10, 100, -100),
        Util::now());

    EXPECT_EQ(
        server.release1RuntimeMetricsSnapshot()
            .rudpMoveReceiveToApplyLatencySampleCount,
        0U);
}

TEST(ServerIntegrationTests, Release1RuntimeMetricsSkipsRejectedAttackLatency) {
    Core::Server server(0);
    const Net::UdpEndpoint endpoint = udpLoopbackEndpoint(31903);
    ASSERT_EQ(
        Core::ServerTestAccess::bindRudpEndpoint(server, endpoint, 1001),
        Net::RudpSessionBindResult::kBoundNew);

    Core::ServerTestAccess::processRudpInputCommandDelivery(
        server,
        endpoint,
        101,
        attackInputCommandPayloadForTest(77, 10, 1),
        Util::now());

    EXPECT_EQ(
        server.release1RuntimeMetricsSnapshot()
            .rudpAttackReceiveToApplyLatencySampleCount,
        0U);
}

TEST(ServerIntegrationTests, Release1RuntimeMetricsTextfileWritesImmediatelyThenHonorsInterval) {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path target =
        std::filesystem::temp_directory_path() /
        ("lol_release1_runtime_metrics_" + std::to_string(suffix) + ".prom");
    std::remove(target.string().c_str());
    std::remove((target.string() + ".tmp").c_str());

    Core::Server server(0);
    ASSERT_TRUE(server.configureRelease1MetricsTextfile(
        target.string(),
        std::chrono::milliseconds(1000)));

    Core::ServerTestAccess::processRuntimeTimerMaintenanceForRelease1Metrics(
        server,
        timeAt(1000));
    const std::string firstWrite = readTextFile(target);
    ASSERT_NE(
        firstWrite.find("lol_server_runtime_tick_total 1\n"),
        std::string::npos);

    Core::ServerTestAccess::processRuntimeTimerMaintenanceForRelease1Metrics(
        server,
        timeAt(1500));
    EXPECT_EQ(readTextFile(target), firstWrite);

    Core::ServerTestAccess::processRuntimeTimerMaintenanceForRelease1Metrics(
        server,
        timeAt(2000));
    const std::string secondWrite = readTextFile(target);
    EXPECT_NE(secondWrite.find("lol_server_runtime_tick_total 3\n"), std::string::npos);

    std::remove(target.string().c_str());
    std::remove((target.string() + ".tmp").c_str());
}
