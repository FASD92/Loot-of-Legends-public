#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Core/MetaSessionClaimClient.hpp"
#include "Core/Server.hpp"
#include "Game/Room.hpp"
#include "Net/TcpPacket.hpp"

namespace {
bool sendAll(int fd, const uint8_t* data, size_t size);

uint64_t unixTimeMsForTest(int64_t offsetMs = 0) {
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
    return static_cast<uint64_t>(nowMs + offsetMs);
}

class TestMetaSessionClaimClient final : public Core::IMetaSessionClaimClient {
public:
    void claimGameSessionAsync(
        const Core::MetaSessionClaimRequest& request,
        ClaimCallback callback) override {
        Core::MetaSessionClaimResult result;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            claimRequests_.push_back(request);
            result.accepted = true;
            result.profile.accountId = nextAccountId_++;
            result.profile.nickname = "Player" + std::to_string(result.profile.accountId);
            result.reservationExpiresAtUnixMs = unixTimeMsForTest(10'000);
        }

        callback(std::move(result));
    }

    void releaseGameSessionAsync(const Core::MetaSessionReleaseRequest& request) override {
        std::lock_guard<std::mutex> lock(mutex_);
        releaseRequests_.push_back(request);
    }

    void renewGameSessionAsync(
        const Core::MetaSessionRenewRequest& /*request*/) override {}

private:
    std::mutex mutex_;
    uint64_t nextAccountId_{1000};
    std::vector<Core::MetaSessionClaimRequest> claimRequests_;
    std::vector<Core::MetaSessionReleaseRequest> releaseRequests_;
};

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

    static std::atomic<uint64_t> nextTokenId{1};
    std::vector<uint8_t> packet;
    const std::string token =
        "room-integration-token-" + std::to_string(nextTokenId.fetch_add(1));
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
        const ssize_t received = ::recv(fd, buffer + receivedTotal, size - receivedTotal, 0);
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

bool recvWelcomePacket(int fd, uint64_t& outSessionId) {
    std::array<uint8_t, Net::kWelcomePacketSize> packet{};
    if (!recvAll(fd, packet.data(), packet.size())) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseWelcomePacket(packet.data(), packet.size(), header, outSessionId);
}

bool recvClientListSnapshotPacket(int fd, std::vector<uint64_t>& outSessionIds) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseClientListSnapshotPacket(packet.data(), packet.size(), header, outSessionIds);
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

bool recvLeaveRoomResponsePacket(int fd, uint32_t& outRoomId) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseLeaveRoomResponsePacket(
        packet.data(),
        packet.size(),
        header,
        outRoomId);
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

bool recvBattleStartRosterPacket(
    int fd,
    uint32_t& outRoomId,
    std::vector<uint64_t>& outPlayerSessionIds) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseBattleStartRosterPacket(
        packet.data(),
        packet.size(),
        header,
        outRoomId,
        outPlayerSessionIds);
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

bool recvMonsterDeathPacket(int fd, uint32_t& outRoomId, uint32_t& outMonsterId) {
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

bool recvLootRejectedPacket(
    int fd,
    uint32_t& outRoomId,
    uint32_t& outDropId,
    Net::TcpLootRejectReason& outReason) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseLootRejectedPacket(
        packet.data(),
        packet.size(),
        header,
        outRoomId,
        outDropId,
        outReason);
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

bool recvSettlementResultPacket(int fd, Net::TcpSettlementResult& outSettlement) {
    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return false;
    }

    Net::TcpPacketHeader header;
    return Net::parseSettlementResultPacket(
        packet.data(),
        packet.size(),
        header,
        outSettlement);
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

bool sendCreateRoomRequestPacket(int fd, const std::string& title) {
    std::vector<uint8_t> packet;
    if (!Net::serializeCreateRoomRequestPacket(title, Game::Room::kDefaultMaxPlayers, packet)) {
        return false;
    }
    return sendAll(fd, packet.data(), packet.size());
}

bool sendCreateRoomRequestPacket(int fd) {
    return sendCreateRoomRequestPacket(fd, "Room");
}

bool sendJoinRoomRequestPacket(int fd, uint32_t roomId) {
    std::array<uint8_t, Net::kRoomIdPacketSize> packet{};
    if (!Net::serializeJoinRoomRequestPacket(roomId, packet)) {
        return false;
    }
    return sendAll(fd, packet.data(), packet.size());
}

bool sendLeaveRoomRequestPacket(int fd) {
    std::array<uint8_t, Net::kTcpHeaderSize> packet{};
    if (!Net::serializeLeaveRoomRequestPacket(packet)) {
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

bool sendMonsterDeathRequestPacket(int fd, uint32_t monsterId) {
    std::array<uint8_t, Net::kMonsterDeathRequestPacketSize> packet{};
    if (!Net::serializeMonsterDeathRequestPacket(monsterId, packet)) {
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

bool sendFinishSessionRequestPacket(int fd) {
    std::array<uint8_t, Net::kFinishSessionRequestPacketSize> packet{};
    if (!Net::serializeFinishSessionRequestPacket(packet)) {
        return false;
    }
    return sendAll(fd, packet.data(), packet.size());
}

constexpr std::chrono::milliseconds kIntegrationReceiveTimeout(1000);
constexpr std::chrono::milliseconds kIntegrationAcceptWaitTimeout(1000);

bool setReceiveTimeout(int fd, std::chrono::milliseconds timeout) {
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(timeout);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(timeout - seconds);

    timeval value{};
    value.tv_sec = static_cast<decltype(value.tv_sec)>(seconds.count());
    value.tv_usec = static_cast<decltype(value.tv_usec)>(micros.count());
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value)) == 0;
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
    return predicate();
}

struct RunningServer {
    explicit RunningServer(uint16_t port) : meta(), server(port, &meta) {}

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

::testing::AssertionResult waitForAcceptedConnections(
    const RunningServer& runningServer,
    size_t expectedCount,
    const char* clientLabel) {
    if (waitUntil(
            [&runningServer, expectedCount]() {
                return runningServer.server.activeConnectionCount() >= expectedCount;
            },
            kIntegrationAcceptWaitTimeout)) {
        return ::testing::AssertionSuccess();
    }

    return ::testing::AssertionFailure()
           << "client " << clientLabel
           << " connected but server accepted "
           << runningServer.server.activeConnectionCount()
           << "/" << expectedCount
           << " connections before welcome";
}

::testing::AssertionResult expectSingleRoomListSnapshot(
    int fd,
    uint32_t expectedRoomId,
    uint16_t expectedPlayerCount,
    Net::TcpRoomStatus expectedStatus = Net::TcpRoomStatus::kOpen,
    const std::string& expectedTitle = "") {
    std::vector<Net::TcpRoomEntry> roomSnapshot;
    if (!recvRoomListSnapshotPacket(fd, roomSnapshot)) {
        return ::testing::AssertionFailure() << "failed to receive room list snapshot";
    }

    if (roomSnapshot.size() != 1u) {
        return ::testing::AssertionFailure()
               << "expected one room entry but received " << roomSnapshot.size();
    }

    const Net::TcpRoomEntry& room = roomSnapshot[0];
    if (room.roomId != expectedRoomId ||
        room.playerCount != expectedPlayerCount ||
        room.maxPlayers != Game::Room::kDefaultMaxPlayers ||
        room.roomStatus != expectedStatus) {
        return ::testing::AssertionFailure()
               << "unexpected room list entry roomId=" << room.roomId
               << " playerCount=" << room.playerCount
               << " maxPlayers=" << room.maxPlayers
               << " status=" << static_cast<int>(room.roomStatus);
    }

    if (!expectedTitle.empty() && room.title != expectedTitle) {
        return ::testing::AssertionFailure()
               << "unexpected room title '" << room.title
               << "', expected '" << expectedTitle << "'";
    }

    return ::testing::AssertionSuccess();
}

::testing::AssertionResult expectEmptyRoomListSnapshot(int fd) {
    std::vector<Net::TcpRoomEntry> roomSnapshot;
    if (!recvRoomListSnapshotPacket(fd, roomSnapshot)) {
        return ::testing::AssertionFailure() << "failed to receive empty room list snapshot";
    }

    if (!roomSnapshot.empty()) {
        return ::testing::AssertionFailure()
               << "expected empty room list snapshot but received "
               << roomSnapshot.size()
               << " entries";
    }

    return ::testing::AssertionSuccess();
}

::testing::AssertionResult drainOptionalClientListSnapshot(
    int fd,
    std::vector<uint64_t> expectedSessionIds) {
    if (!setReceiveTimeout(fd, std::chrono::milliseconds(50))) {
        return ::testing::AssertionFailure()
               << "failed to set short receive timeout for optional client snapshot";
    }

    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        if (!setReceiveTimeout(fd, kIntegrationReceiveTimeout)) {
            return ::testing::AssertionFailure()
                   << "failed to restore receive timeout after optional client snapshot";
        }
        return ::testing::AssertionSuccess();
    }

    if (!setReceiveTimeout(fd, kIntegrationReceiveTimeout)) {
        return ::testing::AssertionFailure()
               << "failed to restore receive timeout after optional client snapshot";
    }

    std::vector<uint64_t> actualSessionIds;
    Net::TcpPacketHeader header;
    if (!Net::parseClientListSnapshotPacket(
            packet.data(),
            packet.size(),
            header,
            actualSessionIds)) {
        return ::testing::AssertionFailure() << "failed to receive client snapshot";
    }

    std::sort(actualSessionIds.begin(), actualSessionIds.end());
    std::sort(expectedSessionIds.begin(), expectedSessionIds.end());
    if (actualSessionIds != expectedSessionIds) {
        return ::testing::AssertionFailure()
               << "unexpected client snapshot size=" << actualSessionIds.size();
    }

    return ::testing::AssertionSuccess();
}

::testing::AssertionResult expectRoomDetailState(
    int fd,
    uint32_t expectedRoomId,
    const std::vector<uint64_t>& expectedMembers,
    Net::TcpRoomStatus expectedStatus = Net::TcpRoomStatus::kOpen) {
    Net::TcpRoomDetailState detail;
    if (!recvRoomDetailStatePacket(fd, detail)) {
        return ::testing::AssertionFailure() << "failed to receive room detail state";
    }

    if (detail.roomId != expectedRoomId ||
        detail.roomStatus != expectedStatus ||
        detail.maxPlayers != Game::Room::kDefaultMaxPlayers ||
        detail.members.size() != expectedMembers.size()) {
        return ::testing::AssertionFailure()
               << "unexpected room detail roomId=" << detail.roomId
               << " status=" << static_cast<int>(detail.roomStatus)
               << " maxPlayers=" << static_cast<int>(detail.maxPlayers)
               << " members=" << detail.members.size();
    }

    for (size_t i = 0; i < expectedMembers.size(); ++i) {
        if (detail.members[i].sessionId != expectedMembers[i]) {
            return ::testing::AssertionFailure()
                   << "unexpected member at index " << i
                   << " sessionId=" << detail.members[i].sessionId
                   << " expected=" << expectedMembers[i];
        }
    }

    return ::testing::AssertionSuccess();
}

::testing::AssertionResult expectNoPacketWithin(
    int fd,
    std::chrono::milliseconds timeout,
    const char* clientLabel) {
    if (!setReceiveTimeout(fd, timeout)) {
        return ::testing::AssertionFailure()
               << "failed to set short receive timeout for client " << clientLabel;
    }

    std::vector<uint8_t> packet;
    if (!recvPacket(fd, packet)) {
        return ::testing::AssertionSuccess();
    }

    Net::TcpPacketHeader header;
    if (Net::peekTcpPacketHeader(packet.data(), packet.size(), header)) {
        return ::testing::AssertionFailure()
               << "client " << clientLabel
               << " received unexpected packet type=0x"
               << std::hex << static_cast<uint16_t>(header.type);
    }

    return ::testing::AssertionFailure()
           << "client " << clientLabel << " received malformed unexpected packet";
}

struct SpawnedBattleClients {
    int clientA{-1};
    int clientB{-1};
    uint64_t sessionA{0};
    uint64_t sessionB{0};
    uint32_t roomId{0};
    uint64_t battleInstanceId{0};
    uint32_t monsterId{0};
    uint32_t monsterTypeId{0};
    uint16_t maxHp{0};
};

::testing::AssertionResult prepareTwoPlayerBattleWithMonster(
    RunningServer& runningServer,
    SpawnedBattleClients& out) {
    const uint16_t port = runningServer.server.boundPort();
    if (port == 0) {
        return ::testing::AssertionFailure() << "server TCP port is not bound";
    }

    out.clientA = connectToServer(port);
    if (out.clientA < 0) {
        return ::testing::AssertionFailure() << "failed to connect client A";
    }
    const ::testing::AssertionResult acceptedA =
        waitForAcceptedConnections(runningServer, 1u, "A");
    if (!acceptedA) {
        return acceptedA;
    }
    if (!setReceiveTimeout(out.clientA, kIntegrationReceiveTimeout)) {
        return ::testing::AssertionFailure() << "failed to set receive timeout for client A";
    }
    if (!recvWelcomePacket(out.clientA, out.sessionA)) {
        return ::testing::AssertionFailure() << "failed to receive welcome for client A";
    }

    if (!expectEmptyRoomListSnapshot(out.clientA)) {
        return ::testing::AssertionFailure() << "failed to receive initial room list for client A";
    }
    if (!drainOptionalClientListSnapshot(out.clientA, {out.sessionA})) {
        return ::testing::AssertionFailure() << "failed to receive client snapshot for client A";
    }

    out.clientB = connectToServer(port);
    if (out.clientB < 0) {
        return ::testing::AssertionFailure() << "failed to connect client B";
    }
    const ::testing::AssertionResult acceptedB =
        waitForAcceptedConnections(runningServer, 2u, "B");
    if (!acceptedB) {
        return acceptedB;
    }
    if (!setReceiveTimeout(out.clientB, kIntegrationReceiveTimeout)) {
        return ::testing::AssertionFailure() << "failed to set receive timeout for client B";
    }
    if (!recvWelcomePacket(out.clientB, out.sessionB)) {
        return ::testing::AssertionFailure() << "failed to receive welcome for client B";
    }
    if (!expectEmptyRoomListSnapshot(out.clientB)) {
        return ::testing::AssertionFailure() << "failed to receive initial room list for client B";
    }
    if (!drainOptionalClientListSnapshot(out.clientB, {out.sessionA, out.sessionB}) ||
        !drainOptionalClientListSnapshot(out.clientA, {out.sessionA, out.sessionB})) {
        return ::testing::AssertionFailure() << "failed to receive two-player client snapshots";
    }

    if (!sendCreateRoomRequestPacket(out.clientA)) {
        return ::testing::AssertionFailure() << "failed to send create room request";
    }

    uint16_t playerCount = 0;
    if (!recvCreateRoomResponsePacket(out.clientA, out.roomId, playerCount)) {
        return ::testing::AssertionFailure() << "failed to receive create room response";
    }
    if (playerCount != 1u) {
        return ::testing::AssertionFailure() << "unexpected creator playerCount=" << playerCount;
    }

    Net::TcpRoomDetailState detail;
    if (!recvRoomDetailStatePacket(out.clientA, detail)) {
        return ::testing::AssertionFailure() << "failed to receive creator room detail after create";
    }
    if (detail.roomId != out.roomId || detail.members.size() != 1u ||
        detail.members[0].sessionId != out.sessionA) {
        return ::testing::AssertionFailure() << "unexpected creator room detail after create";
    }

    std::vector<Net::TcpRoomEntry> roomSnapshot;
    if (!recvRoomListSnapshotPacket(out.clientB, roomSnapshot)) {
        return ::testing::AssertionFailure() << "failed to receive lobby room snapshot after create";
    }

    if (!sendJoinRoomRequestPacket(out.clientB, out.roomId)) {
        return ::testing::AssertionFailure() << "failed to send join room request";
    }

    uint32_t joinedRoomId = 0;
    if (!recvJoinRoomResponsePacket(out.clientB, joinedRoomId, playerCount)) {
        return ::testing::AssertionFailure() << "failed to receive join room response";
    }
    if (joinedRoomId != out.roomId || playerCount != 2u) {
        return ::testing::AssertionFailure()
               << "unexpected join response roomId=" << joinedRoomId
               << " playerCount=" << playerCount;
    }

    if (!recvRoomDetailStatePacket(out.clientA, detail) ||
        !recvRoomDetailStatePacket(out.clientB, detail)) {
        return ::testing::AssertionFailure() << "failed to receive room detail after join";
    }

    if (!sendReadyRoomRequestPacket(out.clientA)) {
        return ::testing::AssertionFailure() << "failed to send ready request from client A";
    }

    uint32_t readyRoomId = 0;
    uint16_t readyPlayerCount = 0;
    uint16_t totalPlayerCount = 0;
    if (!recvReadyRoomResponsePacket(
            out.clientA,
            readyRoomId,
            readyPlayerCount,
            totalPlayerCount)) {
        return ::testing::AssertionFailure() << "failed to receive ready response for client A";
    }
    if (readyRoomId != out.roomId || readyPlayerCount != 1u || totalPlayerCount != 2u) {
        return ::testing::AssertionFailure()
               << "unexpected client A ready response roomId=" << readyRoomId
               << " readyPlayerCount=" << readyPlayerCount
               << " totalPlayerCount=" << totalPlayerCount;
    }
    if (!recvRoomDetailStatePacket(out.clientA, detail) ||
        !recvRoomDetailStatePacket(out.clientB, detail)) {
        return ::testing::AssertionFailure() << "failed to receive room detail after client A ready";
    }

    if (!sendReadyRoomRequestPacket(out.clientB)) {
        return ::testing::AssertionFailure() << "failed to send ready request from client B";
    }

    if (!recvReadyRoomResponsePacket(
            out.clientB,
            readyRoomId,
            readyPlayerCount,
            totalPlayerCount)) {
        return ::testing::AssertionFailure() << "failed to receive ready response for client B";
    }
    if (readyRoomId != out.roomId || readyPlayerCount != 2u || totalPlayerCount != 2u) {
        return ::testing::AssertionFailure()
               << "unexpected client B ready response roomId=" << readyRoomId
               << " readyPlayerCount=" << readyPlayerCount
               << " totalPlayerCount=" << totalPlayerCount;
    }
    if (!recvRoomDetailStatePacket(out.clientA, detail) ||
        !recvRoomDetailStatePacket(out.clientB, detail)) {
        return ::testing::AssertionFailure() << "failed to receive room detail after client B ready";
    }

    if (!sendHostStartBattleRequestPacket(out.clientA)) {
        return ::testing::AssertionFailure() << "failed to send HostStartBattleRequest";
    }
    uint32_t hostStartedRoomId = 0;
    if (!recvHostStartBattleResponsePacket(out.clientA, hostStartedRoomId)) {
        return ::testing::AssertionFailure() << "failed to receive HostStartBattleResponse";
    }
    if (hostStartedRoomId != out.roomId) {
        return ::testing::AssertionFailure() << "unexpected HostStartBattleResponse roomId";
    }

    uint32_t startedRoomIdA = 0;
    uint64_t playerASessionIdA = 0;
    uint64_t playerBSessionIdA = 0;
    if (!recvBattleStartPacket(
            out.clientA,
            startedRoomIdA,
            playerASessionIdA,
            playerBSessionIdA)) {
        return ::testing::AssertionFailure() << "failed to receive BattleStart for client A";
    }
    if (startedRoomIdA != out.roomId ||
        playerASessionIdA != out.sessionA ||
        playerBSessionIdA != out.sessionB) {
        return ::testing::AssertionFailure()
               << "unexpected BattleStart for client A roomId=" << startedRoomIdA
               << " playerA=" << playerASessionIdA
               << " playerB=" << playerBSessionIdA;
    }

    uint32_t startedRoomIdB = 0;
    uint64_t playerASessionIdB = 0;
    uint64_t playerBSessionIdB = 0;
    if (!recvBattleStartPacket(
            out.clientB,
            startedRoomIdB,
            playerASessionIdB,
            playerBSessionIdB)) {
        return ::testing::AssertionFailure() << "failed to receive BattleStart for client B";
    }
    if (startedRoomIdB != out.roomId ||
        playerASessionIdB != out.sessionA ||
        playerBSessionIdB != out.sessionB) {
        return ::testing::AssertionFailure()
               << "unexpected BattleStart for client B roomId=" << startedRoomIdB
               << " playerA=" << playerASessionIdB
               << " playerB=" << playerBSessionIdB;
    }

    const std::vector<uint64_t> expectedPlayers{out.sessionA, out.sessionB};
    uint32_t loadEntryRoomId = 0;
    std::vector<uint64_t> loadEntryPlayers;
    if (!recvBattleLoadEntryPacket(
            out.clientA,
            loadEntryRoomId,
            out.battleInstanceId,
            loadEntryPlayers)) {
        return ::testing::AssertionFailure() << "failed to receive BattleLoadEntry for client A";
    }
    if (loadEntryRoomId != out.roomId ||
        out.battleInstanceId == 0u ||
        loadEntryPlayers != expectedPlayers) {
        return ::testing::AssertionFailure()
               << "unexpected BattleLoadEntry for client A roomId=" << loadEntryRoomId
               << " battleInstanceId=" << out.battleInstanceId
               << " players=" << loadEntryPlayers.size();
    }

    uint64_t battleInstanceIdB = 0;
    if (!recvBattleLoadEntryPacket(
            out.clientB,
            loadEntryRoomId,
            battleInstanceIdB,
            loadEntryPlayers)) {
        return ::testing::AssertionFailure() << "failed to receive BattleLoadEntry for client B";
    }
    if (loadEntryRoomId != out.roomId ||
        battleInstanceIdB != out.battleInstanceId ||
        loadEntryPlayers != expectedPlayers) {
        return ::testing::AssertionFailure()
               << "unexpected BattleLoadEntry for client B roomId=" << loadEntryRoomId
               << " battleInstanceId=" << battleInstanceIdB
               << " players=" << loadEntryPlayers.size();
    }

    const auto noPacketA = expectNoPacketWithin(
        out.clientA,
        std::chrono::milliseconds(50),
        "A before load complete");
    if (!noPacketA) {
        return noPacketA;
    }
    const auto noPacketB = expectNoPacketWithin(
        out.clientB,
        std::chrono::milliseconds(50),
        "B before load complete");
    if (!noPacketB) {
        return noPacketB;
    }
    if (!setReceiveTimeout(out.clientA, kIntegrationReceiveTimeout) ||
        !setReceiveTimeout(out.clientB, kIntegrationReceiveTimeout)) {
        return ::testing::AssertionFailure() << "failed to restore receive timeout";
    }

    if (!sendArenaLoadCompletePacket(out.clientA, out.roomId, out.battleInstanceId)) {
        return ::testing::AssertionFailure() << "failed to send ArenaLoadComplete for client A";
    }
    const auto noGameplayStartA = expectNoPacketWithin(
        out.clientA,
        std::chrono::milliseconds(50),
        "A after first load complete");
    if (!noGameplayStartA) {
        return noGameplayStartA;
    }
    const auto noGameplayStartB = expectNoPacketWithin(
        out.clientB,
        std::chrono::milliseconds(50),
        "B after first load complete");
    if (!noGameplayStartB) {
        return noGameplayStartB;
    }
    if (!setReceiveTimeout(out.clientA, kIntegrationReceiveTimeout) ||
        !setReceiveTimeout(out.clientB, kIntegrationReceiveTimeout)) {
        return ::testing::AssertionFailure() << "failed to restore receive timeout after first load";
    }

    if (!sendArenaLoadCompletePacket(out.clientB, out.roomId, out.battleInstanceId)) {
        return ::testing::AssertionFailure() << "failed to send ArenaLoadComplete for client B";
    }
    uint32_t arenaStartRoomId = 0;
    uint64_t arenaStartBattleInstanceId = 0;
    if (!recvArenaGameplayStartPacket(out.clientA, arenaStartRoomId, arenaStartBattleInstanceId)) {
        return ::testing::AssertionFailure() << "failed to receive ArenaGameplayStart for client A";
    }
    if (arenaStartRoomId != out.roomId ||
        arenaStartBattleInstanceId != out.battleInstanceId) {
        return ::testing::AssertionFailure()
               << "unexpected ArenaGameplayStart for client A roomId=" << arenaStartRoomId
               << " battleInstanceId=" << arenaStartBattleInstanceId;
    }

    if (!recvArenaGameplayStartPacket(out.clientB, arenaStartRoomId, arenaStartBattleInstanceId)) {
        return ::testing::AssertionFailure() << "failed to receive ArenaGameplayStart for client B";
    }
    if (arenaStartRoomId != out.roomId ||
        arenaStartBattleInstanceId != out.battleInstanceId) {
        return ::testing::AssertionFailure()
               << "unexpected ArenaGameplayStart for client B roomId=" << arenaStartRoomId
               << " battleInstanceId=" << arenaStartBattleInstanceId;
    }

    uint32_t spawnedRoomId = 0;
    if (!recvMonsterSpawnPacket(
            out.clientA,
            spawnedRoomId,
            out.monsterId,
            out.monsterTypeId,
            out.maxHp)) {
        return ::testing::AssertionFailure() << "failed to receive MonsterSpawn for client A";
    }
    if (spawnedRoomId != out.roomId ||
        out.monsterId == 0u ||
        out.monsterTypeId == 0u ||
        out.maxHp == 0u) {
        return ::testing::AssertionFailure()
               << "unexpected MonsterSpawn for client A roomId=" << spawnedRoomId
               << " monsterId=" << out.monsterId
               << " monsterTypeId=" << out.monsterTypeId
               << " maxHp=" << out.maxHp;
    }

    uint32_t monsterIdB = 0;
    uint32_t monsterTypeIdB = 0;
    uint16_t maxHpB = 0;
    if (!recvMonsterSpawnPacket(
            out.clientB,
            spawnedRoomId,
            monsterIdB,
            monsterTypeIdB,
            maxHpB)) {
        return ::testing::AssertionFailure() << "failed to receive MonsterSpawn for client B";
    }
    if (spawnedRoomId != out.roomId ||
        monsterIdB != out.monsterId ||
        monsterTypeIdB != out.monsterTypeId ||
        maxHpB != out.maxHp) {
        return ::testing::AssertionFailure()
               << "MonsterSpawn mismatch for client B roomId=" << spawnedRoomId
               << " monsterId=" << monsterIdB
               << " monsterTypeId=" << monsterTypeIdB
               << " maxHp=" << maxHpB;
    }

    return ::testing::AssertionSuccess();
}

::testing::AssertionResult defeatMonsterAndReceiveDrop(
    const SpawnedBattleClients& battle,
    int attackerFd,
    uint32_t& outDropId,
    uint32_t& outItemId,
    uint16_t& outQuantity) {
    if (!sendMonsterDeathRequestPacket(attackerFd, battle.monsterId)) {
        return ::testing::AssertionFailure() << "failed to send MonsterDeathRequest";
    }

    uint32_t deathRoomId = 0;
    uint32_t deadMonsterId = 0;
    if (!recvMonsterDeathPacket(battle.clientA, deathRoomId, deadMonsterId)) {
        return ::testing::AssertionFailure() << "failed to receive MonsterDeath for client A";
    }
    if (deathRoomId != battle.roomId || deadMonsterId != battle.monsterId) {
        return ::testing::AssertionFailure()
               << "unexpected MonsterDeath for client A roomId=" << deathRoomId
               << " monsterId=" << deadMonsterId;
    }

    if (!recvMonsterDeathPacket(battle.clientB, deathRoomId, deadMonsterId)) {
        return ::testing::AssertionFailure() << "failed to receive MonsterDeath for client B";
    }
    if (deathRoomId != battle.roomId || deadMonsterId != battle.monsterId) {
        return ::testing::AssertionFailure()
               << "unexpected MonsterDeath for client B roomId=" << deathRoomId
               << " monsterId=" << deadMonsterId;
    }

    uint32_t dropRoomIdA = 0;
    std::vector<Net::TcpDropEntry> dropsA;
    if (!recvDropListSnapshotPacket(battle.clientA, dropRoomIdA, dropsA)) {
        return ::testing::AssertionFailure() << "failed to receive DropListSnapshot for client A";
    }
    if (dropRoomIdA != battle.roomId || dropsA.size() != 1u) {
        return ::testing::AssertionFailure()
               << "unexpected DropListSnapshot for client A roomId=" << dropRoomIdA
               << " dropCount=" << dropsA.size();
    }

    uint32_t dropRoomIdB = 0;
    std::vector<Net::TcpDropEntry> dropsB;
    if (!recvDropListSnapshotPacket(battle.clientB, dropRoomIdB, dropsB)) {
        return ::testing::AssertionFailure() << "failed to receive DropListSnapshot for client B";
    }
    if (dropRoomIdB != battle.roomId || dropsB.size() != 1u) {
        return ::testing::AssertionFailure()
               << "unexpected DropListSnapshot for client B roomId=" << dropRoomIdB
               << " dropCount=" << dropsB.size();
    }

    if (dropsA[0].dropId != dropsB[0].dropId ||
        dropsA[0].itemId != dropsB[0].itemId ||
        dropsA[0].quantity != dropsB[0].quantity) {
        return ::testing::AssertionFailure() << "drop snapshots differ between clients";
    }

    outDropId = dropsA[0].dropId;
    outItemId = dropsA[0].itemId;
    outQuantity = dropsA[0].quantity;
    return ::testing::AssertionSuccess();
}
}  // namespace

TEST(ServerRoomIntegrationTests, ProcessesCreateAndLeaveRoomRequests) {
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
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientFd));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientFd, {sessionId}));

    ASSERT_TRUE(sendCreateRoomRequestPacket(clientFd));

    uint32_t roomId = 0;
    uint16_t playerCount = 0;
    ASSERT_TRUE(recvCreateRoomResponsePacket(clientFd, roomId, playerCount));
    EXPECT_GT(roomId, 0u);
    EXPECT_EQ(playerCount, 1u);

    ASSERT_TRUE(expectRoomDetailState(clientFd, roomId, {sessionId}));

    ASSERT_TRUE(sendLeaveRoomRequestPacket(clientFd));

    uint32_t leftRoomId = 0;
    ASSERT_TRUE(recvLeaveRoomResponsePacket(clientFd, leftRoomId));
    EXPECT_EQ(leftRoomId, roomId);

    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientFd));

    ::close(clientFd);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, RoomDetailStateUsesAuthenticatedNickname) {
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
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientFd));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientFd, {sessionId}));

    ASSERT_TRUE(sendCreateRoomRequestPacket(clientFd));

    uint32_t roomId = 0;
    uint16_t playerCount = 0;
    ASSERT_TRUE(recvCreateRoomResponsePacket(clientFd, roomId, playerCount));
    ASSERT_GT(roomId, 0u);
    ASSERT_EQ(playerCount, 1u);

    Net::TcpRoomDetailState detail;
    ASSERT_TRUE(recvRoomDetailStatePacket(clientFd, detail));
    ASSERT_EQ(detail.roomId, roomId);
    ASSERT_EQ(detail.members.size(), 1u);
    EXPECT_EQ(detail.members[0].sessionId, sessionId);
    EXPECT_EQ(detail.members[0].nickname, "Player1000");

    ::close(clientFd);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, LateJoinerReceivesCurrentRoomListSnapshotAfterConnect) {
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
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientA));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientA, {sessionA}));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientA, {sessionA}));

    const std::string roomTitle = "친구랑 한 판";
    ASSERT_TRUE(sendCreateRoomRequestPacket(clientA, roomTitle));

    uint32_t roomId = 0;
    uint16_t playerCount = 0;
    ASSERT_TRUE(recvCreateRoomResponsePacket(clientA, roomId, playerCount));
    ASSERT_GT(roomId, 0u);
    EXPECT_EQ(playerCount, 1u);

    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA}));

    int clientB = connectToServer(port);
    ASSERT_GE(clientB, 0);
    ASSERT_TRUE(setReceiveTimeout(clientB, std::chrono::milliseconds(500)));

    uint64_t sessionB = 0;
    ASSERT_TRUE(recvWelcomePacket(clientB, sessionB));
    ASSERT_NE(sessionA, sessionB);
    ASSERT_TRUE(expectSingleRoomListSnapshot(
        clientB,
        roomId,
        1u,
        Net::TcpRoomStatus::kOpen,
        roomTitle));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientB, {sessionA, sessionB}));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientA, {sessionA, sessionB}));

    ::close(clientB);
    ::close(clientA);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, ProcessesJoinRoomRequestAndErrorResponse) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    int clientA = connectToServer(port);
    ASSERT_GE(clientA, 0);
    ASSERT_TRUE(waitForAcceptedConnections(runningServer, 1u, "A"));
    ASSERT_TRUE(setReceiveTimeout(clientA, std::chrono::milliseconds(500)));
    uint64_t sessionA = 0;
    ASSERT_TRUE(recvWelcomePacket(clientA, sessionA));
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientA));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientA, {sessionA}));

    int clientB = connectToServer(port);
    ASSERT_GE(clientB, 0);
    ASSERT_TRUE(waitForAcceptedConnections(runningServer, 2u, "B"));
    ASSERT_TRUE(setReceiveTimeout(clientB, std::chrono::milliseconds(500)));
    uint64_t sessionB = 0;
    ASSERT_TRUE(recvWelcomePacket(clientB, sessionB));
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientB));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientB, {sessionA, sessionB}));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientA, {sessionA, sessionB}));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientB, {sessionA, sessionB}));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientA, {sessionA, sessionB}));

    ASSERT_TRUE(sendJoinRoomRequestPacket(clientB, 9999));
    Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
    Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
    ASSERT_TRUE(recvErrorPacket(clientB, failedType, errorCode));
    EXPECT_EQ(failedType, Net::TcpPacketType::kJoinRoomRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kNotFound);

    ASSERT_TRUE(sendCreateRoomRequestPacket(clientA));

    uint32_t roomId = 0;
    uint16_t playerCount = 0;
    ASSERT_TRUE(recvCreateRoomResponsePacket(clientA, roomId, playerCount));
    EXPECT_GT(roomId, 0u);
    EXPECT_EQ(playerCount, 1u);

    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA}));
    ASSERT_TRUE(expectSingleRoomListSnapshot(clientB, roomId, 1u));

    ASSERT_TRUE(sendJoinRoomRequestPacket(clientB, roomId));
    ASSERT_TRUE(recvJoinRoomResponsePacket(clientB, roomId, playerCount));
    EXPECT_EQ(playerCount, 2u);

    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA, sessionB}));
    ASSERT_TRUE(expectRoomDetailState(clientB, roomId, {sessionA, sessionB}));

    ::close(clientB);
    ::close(clientA);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, ReproducesWeek2RoomLifecycleScenario) {
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
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientA));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientA, {sessionA}));

    int clientB = connectToServer(port);
    ASSERT_GE(clientB, 0);
    ASSERT_TRUE(setReceiveTimeout(clientB, std::chrono::milliseconds(500)));

    uint64_t sessionB = 0;
    ASSERT_TRUE(recvWelcomePacket(clientB, sessionB));
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientB));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientB, {sessionA, sessionB}));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientA, {sessionA, sessionB}));

    ASSERT_TRUE(sendCreateRoomRequestPacket(clientA));

    uint32_t roomId = 0;
    uint16_t playerCount = 0;
    ASSERT_TRUE(recvCreateRoomResponsePacket(clientA, roomId, playerCount));
    EXPECT_GT(roomId, 0u);
    EXPECT_EQ(playerCount, 1u);

    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA}));
    ASSERT_TRUE(expectSingleRoomListSnapshot(clientB, roomId, 1u));

    uint32_t joinedRoomId = 0;
    ASSERT_TRUE(sendJoinRoomRequestPacket(clientB, roomId));
    ASSERT_TRUE(recvJoinRoomResponsePacket(clientB, joinedRoomId, playerCount));
    EXPECT_EQ(joinedRoomId, roomId);
    EXPECT_EQ(playerCount, 2u);

    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA, sessionB}));
    ASSERT_TRUE(expectRoomDetailState(clientB, roomId, {sessionA, sessionB}));

    int clientC = connectToServer(port);
    ASSERT_GE(clientC, 0);
    ASSERT_TRUE(setReceiveTimeout(clientC, std::chrono::milliseconds(500)));

    uint64_t sessionC = 0;
    ASSERT_TRUE(recvWelcomePacket(clientC, sessionC));
    ASSERT_TRUE(expectSingleRoomListSnapshot(clientC, roomId, 2u));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientC, {sessionA, sessionB, sessionC}));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientA, {sessionA, sessionB, sessionC}));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientB, {sessionA, sessionB, sessionC}));

    ASSERT_TRUE(sendJoinRoomRequestPacket(clientC, roomId));
    ASSERT_TRUE(recvJoinRoomResponsePacket(clientC, joinedRoomId, playerCount));
    EXPECT_EQ(joinedRoomId, roomId);
    EXPECT_EQ(playerCount, 3u);
    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA, sessionB, sessionC}));
    ASSERT_TRUE(expectRoomDetailState(clientB, roomId, {sessionA, sessionB, sessionC}));
    ASSERT_TRUE(expectRoomDetailState(clientC, roomId, {sessionA, sessionB, sessionC}));
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 3u &&
                   runningServer.server.sessionCount() == 3u;
        }));

    uint32_t leftRoomId = 0;
    ASSERT_TRUE(sendLeaveRoomRequestPacket(clientB));
    ASSERT_TRUE(recvLeaveRoomResponsePacket(clientB, leftRoomId));
    EXPECT_EQ(leftRoomId, roomId);

    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA, sessionC}));
    ASSERT_TRUE(expectSingleRoomListSnapshot(clientB, roomId, 2u));
    ASSERT_TRUE(expectRoomDetailState(clientC, roomId, {sessionA, sessionC}));

    ASSERT_TRUE(sendLeaveRoomRequestPacket(clientA));
    ASSERT_TRUE(recvLeaveRoomResponsePacket(clientA, leftRoomId));
    EXPECT_EQ(leftRoomId, roomId);

    ASSERT_TRUE(expectSingleRoomListSnapshot(clientA, roomId, 1u));
    ASSERT_TRUE(expectSingleRoomListSnapshot(clientB, roomId, 1u));
    ASSERT_TRUE(expectRoomDetailState(clientC, roomId, {sessionC}));

    ASSERT_TRUE(sendLeaveRoomRequestPacket(clientC));
    ASSERT_TRUE(recvLeaveRoomResponsePacket(clientC, leftRoomId));
    EXPECT_EQ(leftRoomId, roomId);

    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientA));
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientB));
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientC));

    ::close(clientC);
    ::close(clientB);
    ::close(clientA);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, TriggersBattleStartAfterBothRoomPlayersReady) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    int clientA = connectToServer(port);
    ASSERT_GE(clientA, 0);
    ASSERT_TRUE(waitForAcceptedConnections(runningServer, 1u, "A"));
    ASSERT_TRUE(setReceiveTimeout(clientA, std::chrono::milliseconds(500)));
    uint64_t sessionA = 0;
    ASSERT_TRUE(recvWelcomePacket(clientA, sessionA));
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientA));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientA, {sessionA}));

    int clientB = connectToServer(port);
    ASSERT_GE(clientB, 0);
    ASSERT_TRUE(waitForAcceptedConnections(runningServer, 2u, "B"));
    ASSERT_TRUE(setReceiveTimeout(clientB, std::chrono::milliseconds(500)));
    uint64_t sessionB = 0;
    ASSERT_TRUE(recvWelcomePacket(clientB, sessionB));
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientB));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientB, {sessionA, sessionB}));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientA, {sessionA, sessionB}));

    ASSERT_TRUE(sendCreateRoomRequestPacket(clientA));
    uint32_t roomId = 0;
    uint16_t playerCount = 0;
    ASSERT_TRUE(recvCreateRoomResponsePacket(clientA, roomId, playerCount));
    EXPECT_EQ(playerCount, 1u);

    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA}));
    ASSERT_TRUE(expectSingleRoomListSnapshot(clientB, roomId, 1u));

    ASSERT_TRUE(sendJoinRoomRequestPacket(clientB, roomId));
    uint32_t joinedRoomId = 0;
    ASSERT_TRUE(recvJoinRoomResponsePacket(clientB, joinedRoomId, playerCount));
    EXPECT_EQ(joinedRoomId, roomId);
    EXPECT_EQ(playerCount, 2u);

    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA, sessionB}));
    ASSERT_TRUE(expectRoomDetailState(clientB, roomId, {sessionA, sessionB}));

    ASSERT_TRUE(sendReadyRoomRequestPacket(clientA));
    uint32_t readyRoomId = 0;
    uint16_t readyPlayerCount = 0;
    uint16_t totalPlayerCount = 0;
    ASSERT_TRUE(recvReadyRoomResponsePacket(
        clientA,
        readyRoomId,
        readyPlayerCount,
        totalPlayerCount));
    EXPECT_EQ(readyRoomId, roomId);
    EXPECT_EQ(readyPlayerCount, 1u);
    EXPECT_EQ(totalPlayerCount, 2u);
    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA, sessionB}));
    ASSERT_TRUE(expectRoomDetailState(clientB, roomId, {sessionA, sessionB}));

    ASSERT_TRUE(setReceiveTimeout(clientA, std::chrono::milliseconds(50)));
    ASSERT_TRUE(setReceiveTimeout(clientB, std::chrono::milliseconds(50)));
    std::vector<uint8_t> unexpectedPacket;
    EXPECT_FALSE(recvPacket(clientA, unexpectedPacket));
    EXPECT_FALSE(recvPacket(clientB, unexpectedPacket));

    ASSERT_TRUE(setReceiveTimeout(clientA, std::chrono::milliseconds(500)));
    ASSERT_TRUE(setReceiveTimeout(clientB, std::chrono::milliseconds(500)));

    ASSERT_TRUE(sendReadyRoomRequestPacket(clientB));
    ASSERT_TRUE(recvReadyRoomResponsePacket(
        clientB,
        readyRoomId,
        readyPlayerCount,
        totalPlayerCount));
    EXPECT_EQ(readyRoomId, roomId);
    EXPECT_EQ(readyPlayerCount, 2u);
    EXPECT_EQ(totalPlayerCount, 2u);
    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA, sessionB}));
    ASSERT_TRUE(expectRoomDetailState(clientB, roomId, {sessionA, sessionB}));

    ASSERT_TRUE(sendHostStartBattleRequestPacket(clientA));
    uint32_t hostStartedRoomId = 0;
    ASSERT_TRUE(recvHostStartBattleResponsePacket(clientA, hostStartedRoomId));
    EXPECT_EQ(hostStartedRoomId, roomId);

    uint32_t startedRoomId = 0;
    uint64_t playerASessionId = 0;
    uint64_t playerBSessionId = 0;
    ASSERT_TRUE(recvBattleStartPacket(
        clientA,
        startedRoomId,
        playerASessionId,
        playerBSessionId));
    EXPECT_EQ(startedRoomId, roomId);
    EXPECT_EQ(playerASessionId, sessionA);
    EXPECT_EQ(playerBSessionId, sessionB);

    ASSERT_TRUE(recvBattleStartPacket(
        clientB,
        startedRoomId,
        playerASessionId,
        playerBSessionId));
    EXPECT_EQ(startedRoomId, roomId);
    EXPECT_EQ(playerASessionId, sessionA);
    EXPECT_EQ(playerBSessionId, sessionB);

    const std::vector<uint64_t> expectedPlayers{sessionA, sessionB};
    uint32_t loadEntryRoomId = 0;
    uint64_t battleInstanceId = 0;
    std::vector<uint64_t> loadEntryPlayers;
    ASSERT_TRUE(recvBattleLoadEntryPacket(
        clientA,
        loadEntryRoomId,
        battleInstanceId,
        loadEntryPlayers));
    EXPECT_EQ(loadEntryRoomId, roomId);
    EXPECT_GT(battleInstanceId, 0u);
    EXPECT_EQ(loadEntryPlayers, expectedPlayers);

    uint64_t battleInstanceIdB = 0;
    ASSERT_TRUE(recvBattleLoadEntryPacket(
        clientB,
        loadEntryRoomId,
        battleInstanceIdB,
        loadEntryPlayers));
    EXPECT_EQ(loadEntryRoomId, roomId);
    EXPECT_EQ(battleInstanceIdB, battleInstanceId);
    EXPECT_EQ(loadEntryPlayers, expectedPlayers);

    ASSERT_TRUE(expectNoPacketWithin(clientA, std::chrono::milliseconds(50), "A before load"));
    ASSERT_TRUE(expectNoPacketWithin(clientB, std::chrono::milliseconds(50), "B before load"));
    ASSERT_TRUE(setReceiveTimeout(clientA, std::chrono::milliseconds(500)));
    ASSERT_TRUE(setReceiveTimeout(clientB, std::chrono::milliseconds(500)));

    ASSERT_TRUE(sendArenaLoadCompletePacket(clientA, roomId, battleInstanceId));
    ASSERT_TRUE(expectNoPacketWithin(clientA, std::chrono::milliseconds(50), "A after first load"));
    ASSERT_TRUE(expectNoPacketWithin(clientB, std::chrono::milliseconds(50), "B after first load"));
    ASSERT_TRUE(setReceiveTimeout(clientA, std::chrono::milliseconds(500)));
    ASSERT_TRUE(setReceiveTimeout(clientB, std::chrono::milliseconds(500)));

    ASSERT_TRUE(sendArenaLoadCompletePacket(clientB, roomId, battleInstanceId));
    uint32_t arenaStartRoomId = 0;
    uint64_t arenaStartBattleInstanceId = 0;
    ASSERT_TRUE(recvArenaGameplayStartPacket(clientA, arenaStartRoomId, arenaStartBattleInstanceId));
    EXPECT_EQ(arenaStartRoomId, roomId);
    EXPECT_EQ(arenaStartBattleInstanceId, battleInstanceId);

    ASSERT_TRUE(recvArenaGameplayStartPacket(clientB, arenaStartRoomId, arenaStartBattleInstanceId));
    EXPECT_EQ(arenaStartRoomId, roomId);
    EXPECT_EQ(arenaStartBattleInstanceId, battleInstanceId);

    uint32_t spawnedRoomId = 0;
    uint32_t monsterId = 0;
    uint32_t monsterTypeId = 0;
    uint16_t maxHp = 0;
    ASSERT_TRUE(recvMonsterSpawnPacket(
        clientA,
        spawnedRoomId,
        monsterId,
        monsterTypeId,
        maxHp));
    EXPECT_EQ(spawnedRoomId, roomId);
    EXPECT_GT(monsterId, 0u);
    EXPECT_GT(monsterTypeId, 0u);
    EXPECT_GT(maxHp, 0u);

    uint32_t spawnedMonsterIdB = 0;
    uint32_t monsterTypeIdB = 0;
    uint16_t maxHpB = 0;
    ASSERT_TRUE(recvMonsterSpawnPacket(
        clientB,
        spawnedRoomId,
        spawnedMonsterIdB,
        monsterTypeIdB,
        maxHpB));
    EXPECT_EQ(spawnedRoomId, roomId);
    EXPECT_EQ(spawnedMonsterIdB, monsterId);
    EXPECT_EQ(monsterTypeIdB, monsterTypeId);
    EXPECT_EQ(maxHpB, maxHp);

    ASSERT_TRUE(sendReadyRoomRequestPacket(clientA));
    Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
    Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
    ASSERT_TRUE(recvErrorPacket(clientA, failedType, errorCode));
    EXPECT_EQ(failedType, Net::TcpPacketType::kReadyRoomRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kAlreadyStarted);

    ASSERT_TRUE(setReceiveTimeout(clientA, std::chrono::milliseconds(50)));
    ASSERT_TRUE(setReceiveTimeout(clientB, std::chrono::milliseconds(50)));
    EXPECT_FALSE(recvPacket(clientA, unexpectedPacket));
    EXPECT_FALSE(recvPacket(clientB, unexpectedPacket));

    ::close(clientB);
    ::close(clientA);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, TriggersBattleStartRosterAfterThreeRoomPlayersReady) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    int clientA = connectToServer(port);
    ASSERT_GE(clientA, 0);
    ASSERT_TRUE(waitForAcceptedConnections(runningServer, 1u, "A"));
    ASSERT_TRUE(setReceiveTimeout(clientA, std::chrono::milliseconds(500)));
    uint64_t sessionA = 0;
    ASSERT_TRUE(recvWelcomePacket(clientA, sessionA));
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientA));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientA, {sessionA}));

    int clientB = connectToServer(port);
    ASSERT_GE(clientB, 0);
    ASSERT_TRUE(waitForAcceptedConnections(runningServer, 2u, "B"));
    ASSERT_TRUE(setReceiveTimeout(clientB, std::chrono::milliseconds(500)));
    uint64_t sessionB = 0;
    ASSERT_TRUE(recvWelcomePacket(clientB, sessionB));
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientB));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientB, {sessionA, sessionB}));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientA, {sessionA, sessionB}));

    int clientC = connectToServer(port);
    ASSERT_GE(clientC, 0);
    ASSERT_TRUE(waitForAcceptedConnections(runningServer, 3u, "C"));
    ASSERT_TRUE(setReceiveTimeout(clientC, std::chrono::milliseconds(500)));
    uint64_t sessionC = 0;
    ASSERT_TRUE(recvWelcomePacket(clientC, sessionC));
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientC));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientC, {sessionA, sessionB, sessionC}));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientA, {sessionA, sessionB, sessionC}));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientB, {sessionA, sessionB, sessionC}));

    ASSERT_TRUE(sendCreateRoomRequestPacket(clientA));
    uint32_t roomId = 0;
    uint16_t playerCount = 0;
    ASSERT_TRUE(recvCreateRoomResponsePacket(clientA, roomId, playerCount));
    EXPECT_EQ(playerCount, 1u);

    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA}));
    ASSERT_TRUE(expectSingleRoomListSnapshot(clientB, roomId, 1u));
    ASSERT_TRUE(expectSingleRoomListSnapshot(clientC, roomId, 1u));

    uint32_t joinedRoomId = 0;
    ASSERT_TRUE(sendJoinRoomRequestPacket(clientB, roomId));
    ASSERT_TRUE(recvJoinRoomResponsePacket(clientB, joinedRoomId, playerCount));
    EXPECT_EQ(joinedRoomId, roomId);
    EXPECT_EQ(playerCount, 2u);
    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA, sessionB}));
    ASSERT_TRUE(expectRoomDetailState(clientB, roomId, {sessionA, sessionB}));
    ASSERT_TRUE(expectSingleRoomListSnapshot(clientC, roomId, 2u));

    ASSERT_TRUE(sendJoinRoomRequestPacket(clientC, roomId));
    ASSERT_TRUE(recvJoinRoomResponsePacket(clientC, joinedRoomId, playerCount));
    EXPECT_EQ(joinedRoomId, roomId);
    EXPECT_EQ(playerCount, 3u);
    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA, sessionB, sessionC}));
    ASSERT_TRUE(expectRoomDetailState(clientB, roomId, {sessionA, sessionB, sessionC}));
    ASSERT_TRUE(expectRoomDetailState(clientC, roomId, {sessionA, sessionB, sessionC}));

    uint32_t readyRoomId = 0;
    uint16_t readyPlayerCount = 0;
    uint16_t totalPlayerCount = 0;
    ASSERT_TRUE(sendReadyRoomRequestPacket(clientA));
    ASSERT_TRUE(recvReadyRoomResponsePacket(clientA, readyRoomId, readyPlayerCount, totalPlayerCount));
    EXPECT_EQ(readyPlayerCount, 1u);
    EXPECT_EQ(totalPlayerCount, 3u);
    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA, sessionB, sessionC}));
    ASSERT_TRUE(expectRoomDetailState(clientB, roomId, {sessionA, sessionB, sessionC}));
    ASSERT_TRUE(expectRoomDetailState(clientC, roomId, {sessionA, sessionB, sessionC}));

    ASSERT_TRUE(sendReadyRoomRequestPacket(clientB));
    ASSERT_TRUE(recvReadyRoomResponsePacket(clientB, readyRoomId, readyPlayerCount, totalPlayerCount));
    EXPECT_EQ(readyPlayerCount, 2u);
    EXPECT_EQ(totalPlayerCount, 3u);
    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA, sessionB, sessionC}));
    ASSERT_TRUE(expectRoomDetailState(clientB, roomId, {sessionA, sessionB, sessionC}));
    ASSERT_TRUE(expectRoomDetailState(clientC, roomId, {sessionA, sessionB, sessionC}));

    ASSERT_TRUE(sendReadyRoomRequestPacket(clientC));
    ASSERT_TRUE(recvReadyRoomResponsePacket(clientC, readyRoomId, readyPlayerCount, totalPlayerCount));
    EXPECT_EQ(readyPlayerCount, 3u);
    EXPECT_EQ(totalPlayerCount, 3u);
    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA, sessionB, sessionC}));
    ASSERT_TRUE(expectRoomDetailState(clientB, roomId, {sessionA, sessionB, sessionC}));
    ASSERT_TRUE(expectRoomDetailState(clientC, roomId, {sessionA, sessionB, sessionC}));

    ASSERT_TRUE(sendHostStartBattleRequestPacket(clientA));
    uint32_t hostStartedRoomId = 0;
    ASSERT_TRUE(recvHostStartBattleResponsePacket(clientA, hostStartedRoomId));
    EXPECT_EQ(hostStartedRoomId, roomId);

    const std::vector<uint64_t> expectedPlayers{sessionA, sessionB, sessionC};
    uint32_t rosterRoomId = 0;
    std::vector<uint64_t> rosterPlayers;

    ASSERT_TRUE(recvBattleStartRosterPacket(clientA, rosterRoomId, rosterPlayers));
    EXPECT_EQ(rosterRoomId, roomId);
    EXPECT_EQ(rosterPlayers, expectedPlayers);

    ASSERT_TRUE(recvBattleStartRosterPacket(clientB, rosterRoomId, rosterPlayers));
    EXPECT_EQ(rosterRoomId, roomId);
    EXPECT_EQ(rosterPlayers, expectedPlayers);

    ASSERT_TRUE(recvBattleStartRosterPacket(clientC, rosterRoomId, rosterPlayers));
    EXPECT_EQ(rosterRoomId, roomId);
    EXPECT_EQ(rosterPlayers, expectedPlayers);

    ::close(clientC);
    ::close(clientB);
    ::close(clientA);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, ReproducesWeek4MonsterDropScenario) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    SpawnedBattleClients battle;
    ASSERT_TRUE(prepareTwoPlayerBattleWithMonster(runningServer, battle));

    ASSERT_TRUE(sendMonsterDeathRequestPacket(battle.clientA, battle.monsterId));

    uint32_t deathRoomId = 0;
    uint32_t deadMonsterId = 0;
    ASSERT_TRUE(recvMonsterDeathPacket(battle.clientA, deathRoomId, deadMonsterId));
    EXPECT_EQ(deathRoomId, battle.roomId);
    EXPECT_EQ(deadMonsterId, battle.monsterId);

    ASSERT_TRUE(recvMonsterDeathPacket(battle.clientB, deathRoomId, deadMonsterId));
    EXPECT_EQ(deathRoomId, battle.roomId);
    EXPECT_EQ(deadMonsterId, battle.monsterId);

    uint32_t dropRoomIdA = 0;
    std::vector<Net::TcpDropEntry> dropsA;
    ASSERT_TRUE(recvDropListSnapshotPacket(battle.clientA, dropRoomIdA, dropsA));
    EXPECT_EQ(dropRoomIdA, battle.roomId);
    ASSERT_EQ(dropsA.size(), 1u);
    EXPECT_GT(dropsA[0].dropId, 0u);
    EXPECT_GT(dropsA[0].itemId, 0u);
    EXPECT_GT(dropsA[0].quantity, 0u);

    uint32_t dropRoomIdB = 0;
    std::vector<Net::TcpDropEntry> dropsB;
    ASSERT_TRUE(recvDropListSnapshotPacket(battle.clientB, dropRoomIdB, dropsB));
    EXPECT_EQ(dropRoomIdB, battle.roomId);
    ASSERT_EQ(dropsB.size(), 1u);
    EXPECT_EQ(dropsB[0].dropId, dropsA[0].dropId);
    EXPECT_EQ(dropsB[0].itemId, dropsA[0].itemId);
    EXPECT_EQ(dropsB[0].quantity, dropsA[0].quantity);

    ASSERT_TRUE(setReceiveTimeout(battle.clientA, std::chrono::milliseconds(50)));
    ASSERT_TRUE(setReceiveTimeout(battle.clientB, std::chrono::milliseconds(50)));
    std::vector<uint8_t> unexpectedPacket;
    EXPECT_FALSE(recvPacket(battle.clientA, unexpectedPacket));
    EXPECT_FALSE(recvPacket(battle.clientB, unexpectedPacket));

    ::close(battle.clientB);
    ::close(battle.clientA);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, RejectsJoinRoomRequestAfterBattleStarted) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    SpawnedBattleClients battle;
    ASSERT_TRUE(prepareTwoPlayerBattleWithMonster(runningServer, battle));

    int clientC = connectToServer(port);
    ASSERT_GE(clientC, 0);
    ASSERT_TRUE(waitForAcceptedConnections(runningServer, 3u, "C"));
    ASSERT_TRUE(setReceiveTimeout(clientC, kIntegrationReceiveTimeout));

    uint64_t sessionC = 0;
    ASSERT_TRUE(recvWelcomePacket(clientC, sessionC));
    ASSERT_TRUE(expectSingleRoomListSnapshot(
        clientC,
        battle.roomId,
        2u,
        Net::TcpRoomStatus::kInProgress));
    ASSERT_TRUE(drainOptionalClientListSnapshot(
        clientC,
        {battle.sessionA, battle.sessionB, sessionC}));
    ASSERT_TRUE(drainOptionalClientListSnapshot(
        battle.clientA,
        {battle.sessionA, battle.sessionB, sessionC}));
    ASSERT_TRUE(drainOptionalClientListSnapshot(
        battle.clientB,
        {battle.sessionA, battle.sessionB, sessionC}));

    ASSERT_TRUE(sendJoinRoomRequestPacket(clientC, battle.roomId));

    Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
    Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
    ASSERT_TRUE(recvErrorPacket(clientC, failedType, errorCode));
    EXPECT_EQ(failedType, Net::TcpPacketType::kJoinRoomRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kAlreadyStarted);

    ::close(clientC);
    ::close(battle.clientB);
    ::close(battle.clientA);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, ReproducesWeek5ClickLootScenario) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    SpawnedBattleClients battle;
    ASSERT_TRUE(prepareTwoPlayerBattleWithMonster(runningServer, battle));

    uint32_t dropId = 0;
    uint32_t itemId = 0;
    uint16_t quantity = 0;
    ASSERT_TRUE(defeatMonsterAndReceiveDrop(battle, battle.clientA, dropId, itemId, quantity));

    ASSERT_TRUE(sendClickLootRequestPacket(battle.clientA, dropId));

    uint32_t lootRoomId = 0;
    uint32_t resolvedDropId = 0;
    uint64_t winnerSessionId = 0;
    uint32_t resolvedItemId = 0;
    uint16_t resolvedQuantity = 0;
    ASSERT_TRUE(recvLootResolvedPacket(
        battle.clientA,
        lootRoomId,
        resolvedDropId,
        winnerSessionId,
        resolvedItemId,
        resolvedQuantity));
    EXPECT_EQ(lootRoomId, battle.roomId);
    EXPECT_EQ(resolvedDropId, dropId);
    EXPECT_EQ(winnerSessionId, battle.sessionA);
    EXPECT_EQ(resolvedItemId, itemId);
    EXPECT_EQ(resolvedQuantity, quantity);

    ASSERT_TRUE(recvLootResolvedPacket(
        battle.clientB,
        lootRoomId,
        resolvedDropId,
        winnerSessionId,
        resolvedItemId,
        resolvedQuantity));
    EXPECT_EQ(lootRoomId, battle.roomId);
    EXPECT_EQ(resolvedDropId, dropId);
    EXPECT_EQ(winnerSessionId, battle.sessionA);
    EXPECT_EQ(resolvedItemId, itemId);
    EXPECT_EQ(resolvedQuantity, quantity);

    uint64_t inventorySessionId = 0;
    uint16_t currentWeight = 0;
    uint16_t maxWeight = 0;
    std::vector<Net::TcpInventoryEntry> inventoryEntries;
    ASSERT_TRUE(recvInventorySnapshotPacket(
        battle.clientA,
        inventorySessionId,
        currentWeight,
        maxWeight,
        inventoryEntries));
    EXPECT_EQ(inventorySessionId, battle.sessionA);
    EXPECT_EQ(currentWeight, quantity);
    EXPECT_EQ(maxWeight, 10u);
    ASSERT_EQ(inventoryEntries.size(), 1u);
    EXPECT_EQ(inventoryEntries[0].itemId, itemId);
    EXPECT_EQ(inventoryEntries[0].quantity, quantity);

    uint32_t rankingRoomId = 0;
    uint64_t rankingBattleInstanceId = 0;
    std::vector<Net::BattleFinalRankingEntry> rankingRows;
    ASSERT_TRUE(recvBattleFinalRankingPacket(
        battle.clientA,
        rankingRoomId,
        rankingBattleInstanceId,
        rankingRows));
    EXPECT_EQ(rankingRoomId, battle.roomId);
    ASSERT_TRUE(recvBattleFinalRankingPacket(
        battle.clientB,
        rankingRoomId,
        rankingBattleInstanceId,
        rankingRows));

    uint32_t previousRoomId = 0;
    Net::TcpLobbyReturnReason returnReason = Net::TcpLobbyReturnReason::kHostKick;
    ASSERT_TRUE(recvLobbyReturnVisibilityPacket(battle.clientA, previousRoomId, returnReason));
    EXPECT_EQ(previousRoomId, battle.roomId);
    EXPECT_EQ(returnReason, Net::TcpLobbyReturnReason::kNone);
    ASSERT_TRUE(recvLobbyReturnVisibilityPacket(battle.clientB, previousRoomId, returnReason));
    EXPECT_EQ(previousRoomId, battle.roomId);
    EXPECT_EQ(returnReason, Net::TcpLobbyReturnReason::kNone);

    ASSERT_TRUE(expectEmptyRoomListSnapshot(battle.clientA));
    ASSERT_TRUE(expectEmptyRoomListSnapshot(battle.clientB));

    ASSERT_TRUE(setReceiveTimeout(battle.clientA, std::chrono::milliseconds(50)));
    ASSERT_TRUE(setReceiveTimeout(battle.clientB, std::chrono::milliseconds(50)));
    std::vector<uint8_t> unexpectedPacket;
    EXPECT_FALSE(recvPacket(battle.clientA, unexpectedPacket));
    EXPECT_FALSE(recvPacket(battle.clientB, unexpectedPacket));

    ::close(battle.clientB);
    ::close(battle.clientA);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, BroadcastsFinalRankingAndLobbyReturnAfterAllDropsResolved) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    SpawnedBattleClients battle;
    ASSERT_TRUE(prepareTwoPlayerBattleWithMonster(runningServer, battle));

    uint32_t dropId = 0;
    uint32_t itemId = 0;
    uint16_t quantity = 0;
    ASSERT_TRUE(defeatMonsterAndReceiveDrop(battle, battle.clientA, dropId, itemId, quantity));

    ASSERT_TRUE(sendClickLootRequestPacket(battle.clientA, dropId));

    uint32_t lootRoomId = 0;
    uint32_t resolvedDropId = 0;
    uint64_t winnerSessionId = 0;
    uint32_t resolvedItemId = 0;
    uint16_t resolvedQuantity = 0;
    ASSERT_TRUE(recvLootResolvedPacket(
        battle.clientA,
        lootRoomId,
        resolvedDropId,
        winnerSessionId,
        resolvedItemId,
        resolvedQuantity));
    ASSERT_TRUE(recvLootResolvedPacket(
        battle.clientB,
        lootRoomId,
        resolvedDropId,
        winnerSessionId,
        resolvedItemId,
        resolvedQuantity));

    uint64_t inventorySessionId = 0;
    uint16_t currentWeight = 0;
    uint16_t maxWeight = 0;
    std::vector<Net::TcpInventoryEntry> inventoryEntries;
    ASSERT_TRUE(recvInventorySnapshotPacket(
        battle.clientA,
        inventorySessionId,
        currentWeight,
        maxWeight,
        inventoryEntries));
    EXPECT_EQ(inventorySessionId, battle.sessionA);

    uint32_t rankingRoomId = 0;
    uint64_t rankingBattleInstanceId = 0;
    std::vector<Net::BattleFinalRankingEntry> rankingRowsA;
    ASSERT_TRUE(recvBattleFinalRankingPacket(
        battle.clientA,
        rankingRoomId,
        rankingBattleInstanceId,
        rankingRowsA));
    EXPECT_EQ(rankingRoomId, battle.roomId);
    EXPECT_EQ(rankingBattleInstanceId, battle.battleInstanceId);

    std::vector<Net::BattleFinalRankingEntry> rankingRowsB;
    ASSERT_TRUE(recvBattleFinalRankingPacket(
        battle.clientB,
        rankingRoomId,
        rankingBattleInstanceId,
        rankingRowsB));

    ASSERT_EQ(rankingRowsA.size(), 2u);
    ASSERT_EQ(rankingRowsB.size(), rankingRowsA.size());
    for (std::size_t index = 0; index < rankingRowsA.size(); ++index) {
        EXPECT_EQ(rankingRowsB[index].rank, rankingRowsA[index].rank);
        EXPECT_EQ(rankingRowsB[index].sessionId, rankingRowsA[index].sessionId);
        EXPECT_EQ(rankingRowsB[index].nickname, rankingRowsA[index].nickname);
        EXPECT_EQ(rankingRowsB[index].totalAssetValue, rankingRowsA[index].totalAssetValue);
    }
    EXPECT_EQ(rankingRowsA[0].rank, 1u);
    EXPECT_EQ(rankingRowsA[0].sessionId, battle.sessionA);
    EXPECT_EQ(rankingRowsA[0].nickname, "Player1000");
    EXPECT_EQ(rankingRowsA[0].totalAssetValue, 100);
    EXPECT_EQ(rankingRowsA[1].rank, 2u);
    EXPECT_EQ(rankingRowsA[1].sessionId, battle.sessionB);
    EXPECT_EQ(rankingRowsA[1].nickname, "Player1001");
    EXPECT_EQ(rankingRowsA[1].totalAssetValue, 0);

    uint32_t previousRoomId = 0;
    Net::TcpLobbyReturnReason returnReason = Net::TcpLobbyReturnReason::kHostKick;
    ASSERT_TRUE(recvLobbyReturnVisibilityPacket(battle.clientA, previousRoomId, returnReason));
    EXPECT_EQ(previousRoomId, battle.roomId);
    EXPECT_EQ(returnReason, Net::TcpLobbyReturnReason::kNone);
    ASSERT_TRUE(recvLobbyReturnVisibilityPacket(battle.clientB, previousRoomId, returnReason));
    EXPECT_EQ(previousRoomId, battle.roomId);
    EXPECT_EQ(returnReason, Net::TcpLobbyReturnReason::kNone);

    ASSERT_TRUE(expectEmptyRoomListSnapshot(battle.clientA));
    ASSERT_TRUE(expectEmptyRoomListSnapshot(battle.clientB));

    ::close(battle.clientB);
    ::close(battle.clientA);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, FinishSessionReturnsIdempotentSettlementResult) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    SpawnedBattleClients battle;
    ASSERT_TRUE(prepareTwoPlayerBattleWithMonster(runningServer, battle));

    uint32_t dropId = 0;
    uint32_t itemId = 0;
    uint16_t quantity = 0;
    ASSERT_TRUE(defeatMonsterAndReceiveDrop(battle, battle.clientA, dropId, itemId, quantity));

    ASSERT_TRUE(sendClickLootRequestPacket(battle.clientA, dropId));

    uint32_t lootRoomId = 0;
    uint32_t resolvedDropId = 0;
    uint64_t winnerSessionId = 0;
    uint32_t resolvedItemId = 0;
    uint16_t resolvedQuantity = 0;
    ASSERT_TRUE(recvLootResolvedPacket(
        battle.clientA,
        lootRoomId,
        resolvedDropId,
        winnerSessionId,
        resolvedItemId,
        resolvedQuantity));
    ASSERT_TRUE(recvLootResolvedPacket(
        battle.clientB,
        lootRoomId,
        resolvedDropId,
        winnerSessionId,
        resolvedItemId,
        resolvedQuantity));

    uint64_t inventorySessionId = 0;
    uint16_t currentWeight = 0;
    uint16_t maxWeight = 0;
    std::vector<Net::TcpInventoryEntry> inventoryEntries;
    ASSERT_TRUE(recvInventorySnapshotPacket(
        battle.clientA,
        inventorySessionId,
        currentWeight,
        maxWeight,
        inventoryEntries));
    EXPECT_EQ(inventorySessionId, battle.sessionA);

    uint32_t rankingRoomId = 0;
    uint64_t rankingBattleInstanceId = 0;
    std::vector<Net::BattleFinalRankingEntry> rankingRows;
    ASSERT_TRUE(recvBattleFinalRankingPacket(
        battle.clientA,
        rankingRoomId,
        rankingBattleInstanceId,
        rankingRows));
    ASSERT_TRUE(recvBattleFinalRankingPacket(
        battle.clientB,
        rankingRoomId,
        rankingBattleInstanceId,
        rankingRows));

    uint32_t previousRoomId = 0;
    Net::TcpLobbyReturnReason returnReason = Net::TcpLobbyReturnReason::kHostKick;
    ASSERT_TRUE(recvLobbyReturnVisibilityPacket(battle.clientA, previousRoomId, returnReason));
    EXPECT_EQ(returnReason, Net::TcpLobbyReturnReason::kNone);
    ASSERT_TRUE(recvLobbyReturnVisibilityPacket(battle.clientB, previousRoomId, returnReason));
    EXPECT_EQ(returnReason, Net::TcpLobbyReturnReason::kNone);
    ASSERT_TRUE(expectEmptyRoomListSnapshot(battle.clientA));
    ASSERT_TRUE(expectEmptyRoomListSnapshot(battle.clientB));

    ASSERT_TRUE(sendFinishSessionRequestPacket(battle.clientA));
    Net::TcpSettlementResult firstSettlement;
    ASSERT_TRUE(recvSettlementResultPacket(battle.clientA, firstSettlement));
    EXPECT_FALSE(firstSettlement.settlementId.empty());
    EXPECT_EQ(firstSettlement.sessionId, battle.sessionA);
    EXPECT_EQ(firstSettlement.accountId, battle.sessionA);
    EXPECT_EQ(firstSettlement.roomId, battle.roomId);
    EXPECT_EQ(firstSettlement.reason, Net::TcpSettlementReason::kNormal);
    EXPECT_EQ(firstSettlement.goldDelta, 0);
    ASSERT_EQ(firstSettlement.inventoryDeltas.size(), 1u);
    EXPECT_EQ(firstSettlement.inventoryDeltas[0].itemId, itemId);
    EXPECT_EQ(firstSettlement.inventoryDeltas[0].quantityDelta, quantity);
    EXPECT_EQ(firstSettlement.inventoryDeltas[0].sourceDropId, dropId);

    ASSERT_TRUE(sendFinishSessionRequestPacket(battle.clientA));
    Net::TcpSettlementResult repeatedSettlement;
    ASSERT_TRUE(recvSettlementResultPacket(battle.clientA, repeatedSettlement));
    EXPECT_EQ(repeatedSettlement.settlementId, firstSettlement.settlementId);
    EXPECT_EQ(repeatedSettlement.sessionId, firstSettlement.sessionId);
    EXPECT_EQ(repeatedSettlement.accountId, firstSettlement.accountId);
    EXPECT_EQ(repeatedSettlement.roomId, firstSettlement.roomId);
    EXPECT_EQ(repeatedSettlement.startedAtUnixMs, firstSettlement.startedAtUnixMs);
    EXPECT_EQ(repeatedSettlement.finishedAtUnixMs, firstSettlement.finishedAtUnixMs);
    EXPECT_EQ(repeatedSettlement.goldDelta, firstSettlement.goldDelta);
    EXPECT_EQ(repeatedSettlement.reason, firstSettlement.reason);
    ASSERT_EQ(repeatedSettlement.inventoryDeltas.size(), firstSettlement.inventoryDeltas.size());
    EXPECT_EQ(
        repeatedSettlement.inventoryDeltas[0].itemId,
        firstSettlement.inventoryDeltas[0].itemId);
    EXPECT_EQ(
        repeatedSettlement.inventoryDeltas[0].quantityDelta,
        firstSettlement.inventoryDeltas[0].quantityDelta);
    EXPECT_EQ(
        repeatedSettlement.inventoryDeltas[0].sourceDropId,
        firstSettlement.inventoryDeltas[0].sourceDropId);

    ::close(battle.clientB);
    ::close(battle.clientA);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, FinishSessionBeforeActorGameplayReturnsIdempotentEmptySettlement) {
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
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientFd));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientFd, {sessionId}));

    ASSERT_TRUE(sendCreateRoomRequestPacket(clientFd));

    uint32_t roomId = 0;
    uint16_t playerCount = 0;
    ASSERT_TRUE(recvCreateRoomResponsePacket(clientFd, roomId, playerCount));
    EXPECT_GT(roomId, 0u);
    EXPECT_EQ(playerCount, 1u);

    ASSERT_TRUE(expectRoomDetailState(clientFd, roomId, {sessionId}));

    ASSERT_TRUE(sendFinishSessionRequestPacket(clientFd));
    Net::TcpSettlementResult firstSettlement;
    ASSERT_TRUE(recvSettlementResultPacket(clientFd, firstSettlement));
    EXPECT_FALSE(firstSettlement.settlementId.empty());
    EXPECT_EQ(firstSettlement.sessionId, sessionId);
    EXPECT_EQ(firstSettlement.accountId, sessionId);
    EXPECT_EQ(firstSettlement.roomId, roomId);
    EXPECT_EQ(firstSettlement.reason, Net::TcpSettlementReason::kNormal);
    EXPECT_EQ(firstSettlement.goldDelta, 0);
    EXPECT_TRUE(firstSettlement.inventoryDeltas.empty());

    ASSERT_TRUE(sendFinishSessionRequestPacket(clientFd));
    Net::TcpSettlementResult repeatedSettlement;
    ASSERT_TRUE(recvSettlementResultPacket(clientFd, repeatedSettlement));
    EXPECT_EQ(repeatedSettlement.settlementId, firstSettlement.settlementId);
    EXPECT_EQ(repeatedSettlement.sessionId, firstSettlement.sessionId);
    EXPECT_EQ(repeatedSettlement.accountId, firstSettlement.accountId);
    EXPECT_EQ(repeatedSettlement.roomId, firstSettlement.roomId);
    EXPECT_EQ(repeatedSettlement.startedAtUnixMs, firstSettlement.startedAtUnixMs);
    EXPECT_EQ(repeatedSettlement.finishedAtUnixMs, firstSettlement.finishedAtUnixMs);
    EXPECT_EQ(repeatedSettlement.goldDelta, firstSettlement.goldDelta);
    EXPECT_EQ(repeatedSettlement.reason, firstSettlement.reason);
    EXPECT_TRUE(repeatedSettlement.inventoryDeltas.empty());

    ::close(clientFd);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, RejectsFinishSessionRequestOutsideRoom) {
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
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientFd));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientFd, {sessionId}));

    ASSERT_TRUE(sendFinishSessionRequestPacket(clientFd));
    Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
    Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
    ASSERT_TRUE(recvErrorPacket(clientFd, failedType, errorCode));
    EXPECT_EQ(failedType, Net::TcpPacketType::kFinishSessionRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kNotInRoom);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 1u &&
                   runningServer.server.sessionCount() == 1u;
        }));

    ::close(clientFd);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, RejectsReadyRoomRequestOutsideRoom) {
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
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientFd));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientFd, {sessionId}));

    ASSERT_TRUE(sendReadyRoomRequestPacket(clientFd));
    Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
    Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
    ASSERT_TRUE(recvErrorPacket(clientFd, failedType, errorCode));
    EXPECT_EQ(failedType, Net::TcpPacketType::kReadyRoomRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kNotInRoom);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 1u &&
                   runningServer.server.sessionCount() == 1u;
        }));

    ::close(clientFd);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, RejectsClickLootRequestOutsideRoom) {
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
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientFd));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientFd, {sessionId}));

    ASSERT_TRUE(sendClickLootRequestPacket(clientFd, 1));
    Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
    Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
    ASSERT_TRUE(recvErrorPacket(clientFd, failedType, errorCode));
    EXPECT_EQ(failedType, Net::TcpPacketType::kClickLootRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kNotInRoom);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 1u &&
                   runningServer.server.sessionCount() == 1u;
        }));

    ::close(clientFd);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, RejectsMonsterDeathRequestOutsideRoom) {
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
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientFd));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientFd, {sessionId}));

    ASSERT_TRUE(sendMonsterDeathRequestPacket(clientFd, 1));
    Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
    Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
    ASSERT_TRUE(recvErrorPacket(clientFd, failedType, errorCode));
    EXPECT_EQ(failedType, Net::TcpPacketType::kMonsterDeathRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kNotInRoom);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 1u &&
                   runningServer.server.sessionCount() == 1u;
        }));

    ::close(clientFd);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, RejectsZeroMonsterDeathRequestInsideRoom) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    SpawnedBattleClients battle;
    ASSERT_TRUE(prepareTwoPlayerBattleWithMonster(runningServer, battle));

    ASSERT_TRUE(sendMonsterDeathRequestPacket(battle.clientA, 0));
    Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
    Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
    ASSERT_TRUE(recvErrorPacket(battle.clientA, failedType, errorCode));
    EXPECT_EQ(failedType, Net::TcpPacketType::kMonsterDeathRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kNotFound);

    ASSERT_TRUE(setReceiveTimeout(battle.clientB, std::chrono::milliseconds(50)));
    std::vector<uint8_t> unexpectedPacket;
    EXPECT_FALSE(recvPacket(battle.clientB, unexpectedPacket));

    ::close(battle.clientB);
    ::close(battle.clientA);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, RejectsZeroClickLootRequestInsideRoomWithoutConsumingDrop) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    SpawnedBattleClients battle;
    ASSERT_TRUE(prepareTwoPlayerBattleWithMonster(runningServer, battle));

    uint32_t dropId = 0;
    uint32_t itemId = 0;
    uint16_t quantity = 0;
    ASSERT_TRUE(defeatMonsterAndReceiveDrop(battle, battle.clientA, dropId, itemId, quantity));

    ASSERT_TRUE(sendClickLootRequestPacket(battle.clientA, 0));
    Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
    Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
    ASSERT_TRUE(recvErrorPacket(battle.clientA, failedType, errorCode));
    EXPECT_EQ(failedType, Net::TcpPacketType::kClickLootRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kNotFound);

    ASSERT_TRUE(setReceiveTimeout(battle.clientB, std::chrono::milliseconds(50)));
    std::vector<uint8_t> unexpectedPacket;
    EXPECT_FALSE(recvPacket(battle.clientB, unexpectedPacket));
    ASSERT_TRUE(setReceiveTimeout(battle.clientB, std::chrono::milliseconds(500)));

    ASSERT_TRUE(sendClickLootRequestPacket(battle.clientA, dropId));

    uint32_t lootRoomId = 0;
    uint32_t resolvedDropId = 0;
    uint64_t winnerSessionId = 0;
    uint32_t resolvedItemId = 0;
    uint16_t resolvedQuantity = 0;
    ASSERT_TRUE(recvLootResolvedPacket(
        battle.clientA,
        lootRoomId,
        resolvedDropId,
        winnerSessionId,
        resolvedItemId,
        resolvedQuantity));
    EXPECT_EQ(lootRoomId, battle.roomId);
    EXPECT_EQ(resolvedDropId, dropId);
    EXPECT_EQ(winnerSessionId, battle.sessionA);
    EXPECT_EQ(resolvedItemId, itemId);
    EXPECT_EQ(resolvedQuantity, quantity);

    ASSERT_TRUE(recvLootResolvedPacket(
        battle.clientB,
        lootRoomId,
        resolvedDropId,
        winnerSessionId,
        resolvedItemId,
        resolvedQuantity));
    EXPECT_EQ(lootRoomId, battle.roomId);
    EXPECT_EQ(resolvedDropId, dropId);
    EXPECT_EQ(winnerSessionId, battle.sessionA);

    uint64_t inventorySessionId = 0;
    uint16_t currentWeight = 0;
    uint16_t maxWeight = 0;
    std::vector<Net::TcpInventoryEntry> inventoryEntries;
    ASSERT_TRUE(recvInventorySnapshotPacket(
        battle.clientA,
        inventorySessionId,
        currentWeight,
        maxWeight,
        inventoryEntries));
    EXPECT_EQ(inventorySessionId, battle.sessionA);
    EXPECT_EQ(currentWeight, quantity);
    EXPECT_EQ(maxWeight, 10u);
    ASSERT_EQ(inventoryEntries.size(), 1u);
    EXPECT_EQ(inventoryEntries[0].itemId, itemId);
    EXPECT_EQ(inventoryEntries[0].quantity, quantity);

    ::close(battle.clientB);
    ::close(battle.clientA);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, ResetsLootStateWhenPlayerLeavesBeforeClick) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    SpawnedBattleClients battle;
    ASSERT_TRUE(prepareTwoPlayerBattleWithMonster(runningServer, battle));

    uint32_t dropId = 0;
    uint32_t itemId = 0;
    uint16_t quantity = 0;
    ASSERT_TRUE(defeatMonsterAndReceiveDrop(battle, battle.clientA, dropId, itemId, quantity));

    ASSERT_TRUE(sendLeaveRoomRequestPacket(battle.clientB));
    uint32_t leftRoomId = 0;
    ASSERT_TRUE(recvLeaveRoomResponsePacket(battle.clientB, leftRoomId));
    EXPECT_EQ(leftRoomId, battle.roomId);

    ASSERT_TRUE(expectRoomDetailState(
        battle.clientA,
        battle.roomId,
        {battle.sessionA},
        Net::TcpRoomStatus::kInProgress));
    ASSERT_TRUE(expectSingleRoomListSnapshot(
        battle.clientB,
        battle.roomId,
        1u,
        Net::TcpRoomStatus::kInProgress));

    ASSERT_TRUE(sendClickLootRequestPacket(battle.clientA, dropId));
    uint32_t lootRoomId = 0;
    uint32_t resolvedDropId = 0;
    uint64_t winnerSessionId = 0;
    uint32_t resolvedItemId = 0;
    uint16_t resolvedQuantity = 0;
    ASSERT_TRUE(recvLootResolvedPacket(
        battle.clientA,
        lootRoomId,
        resolvedDropId,
        winnerSessionId,
        resolvedItemId,
        resolvedQuantity));
    EXPECT_EQ(lootRoomId, battle.roomId);
    EXPECT_EQ(resolvedDropId, dropId);
    EXPECT_EQ(winnerSessionId, battle.sessionA);

    uint64_t inventorySessionId = 0;
    uint16_t currentWeight = 0;
    uint16_t maxWeight = 0;
    std::vector<Net::TcpInventoryEntry> inventoryEntries;
    ASSERT_TRUE(recvInventorySnapshotPacket(
        battle.clientA,
        inventorySessionId,
        currentWeight,
        maxWeight,
        inventoryEntries));
    EXPECT_EQ(inventorySessionId, battle.sessionA);

    uint32_t rankingRoomId = 0;
    uint64_t rankingBattleInstanceId = 0;
    std::vector<Net::BattleFinalRankingEntry> rankingRows;
    ASSERT_TRUE(recvBattleFinalRankingPacket(
        battle.clientA,
        rankingRoomId,
        rankingBattleInstanceId,
        rankingRows));
    ASSERT_TRUE(recvBattleFinalRankingPacket(
        battle.clientB,
        rankingRoomId,
        rankingBattleInstanceId,
        rankingRows));
    ASSERT_EQ(rankingRows.size(), 2u);
    EXPECT_EQ(rankingRows[0].sessionId, battle.sessionA);
    EXPECT_EQ(rankingRows[1].sessionId, battle.sessionB);

    uint32_t previousRoomId = 0;
    Net::TcpLobbyReturnReason returnReason = Net::TcpLobbyReturnReason::kHostKick;
    ASSERT_TRUE(recvLobbyReturnVisibilityPacket(battle.clientA, previousRoomId, returnReason));
    EXPECT_EQ(previousRoomId, battle.roomId);
    EXPECT_EQ(returnReason, Net::TcpLobbyReturnReason::kNone);
    ASSERT_TRUE(recvLobbyReturnVisibilityPacket(battle.clientB, previousRoomId, returnReason));
    EXPECT_EQ(previousRoomId, battle.roomId);
    EXPECT_EQ(returnReason, Net::TcpLobbyReturnReason::kNone);

    ASSERT_TRUE(expectEmptyRoomListSnapshot(battle.clientA));
    ASSERT_TRUE(expectEmptyRoomListSnapshot(battle.clientB));

    ::close(battle.clientB);
    ::close(battle.clientA);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, DoesNotTriggerBattleStartWhenPlayerLeavesBeforeAllReady) {
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
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientA));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientA, {sessionA}));

    int clientB = connectToServer(port);
    ASSERT_GE(clientB, 0);
    ASSERT_TRUE(setReceiveTimeout(clientB, std::chrono::milliseconds(500)));
    uint64_t sessionB = 0;
    ASSERT_TRUE(recvWelcomePacket(clientB, sessionB));
    ASSERT_TRUE(expectEmptyRoomListSnapshot(clientB));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientB, {sessionA, sessionB}));
    ASSERT_TRUE(drainOptionalClientListSnapshot(clientA, {sessionA, sessionB}));

    ASSERT_TRUE(sendCreateRoomRequestPacket(clientA));
    uint32_t roomId = 0;
    uint16_t playerCount = 0;
    ASSERT_TRUE(recvCreateRoomResponsePacket(clientA, roomId, playerCount));

    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA}));
    ASSERT_TRUE(expectSingleRoomListSnapshot(clientB, roomId, 1u));

    ASSERT_TRUE(sendJoinRoomRequestPacket(clientB, roomId));
    uint32_t joinedRoomId = 0;
    ASSERT_TRUE(recvJoinRoomResponsePacket(clientB, joinedRoomId, playerCount));
    EXPECT_EQ(joinedRoomId, roomId);

    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA, sessionB}));
    ASSERT_TRUE(expectRoomDetailState(clientB, roomId, {sessionA, sessionB}));

    uint32_t readyRoomId = 0;
    uint16_t readyPlayerCount = 0;
    uint16_t totalPlayerCount = 0;
    ASSERT_TRUE(sendReadyRoomRequestPacket(clientA));
    ASSERT_TRUE(recvReadyRoomResponsePacket(
        clientA,
        readyRoomId,
        readyPlayerCount,
        totalPlayerCount));
    EXPECT_EQ(readyRoomId, roomId);
    EXPECT_EQ(readyPlayerCount, 1u);
    EXPECT_EQ(totalPlayerCount, 2u);
    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA, sessionB}));
    ASSERT_TRUE(expectRoomDetailState(clientB, roomId, {sessionA, sessionB}));

    ASSERT_TRUE(sendLeaveRoomRequestPacket(clientB));
    uint32_t leftRoomId = 0;
    ASSERT_TRUE(recvLeaveRoomResponsePacket(clientB, leftRoomId));
    EXPECT_EQ(leftRoomId, roomId);

    ASSERT_TRUE(expectRoomDetailState(clientA, roomId, {sessionA}));
    ASSERT_TRUE(expectSingleRoomListSnapshot(clientB, roomId, 1u));

    ASSERT_TRUE(setReceiveTimeout(clientA, std::chrono::milliseconds(50)));
    ASSERT_TRUE(setReceiveTimeout(clientB, std::chrono::milliseconds(50)));
    std::vector<uint8_t> unexpectedPacket;
    EXPECT_FALSE(recvPacket(clientA, unexpectedPacket));
    EXPECT_FALSE(recvPacket(clientB, unexpectedPacket));

    ASSERT_TRUE(setReceiveTimeout(clientB, std::chrono::milliseconds(500)));
    ASSERT_TRUE(sendReadyRoomRequestPacket(clientB));
    Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
    Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
    ASSERT_TRUE(recvErrorPacket(clientB, failedType, errorCode));
    EXPECT_EQ(failedType, Net::TcpPacketType::kReadyRoomRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kNotInRoom);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 2u &&
                   runningServer.server.sessionCount() == 2u;
        }));

    ::close(clientB);
    ::close(clientA);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, RejectsWrongAndAlreadyDeadMonsterDeathRequests) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    SpawnedBattleClients battle;
    ASSERT_TRUE(prepareTwoPlayerBattleWithMonster(runningServer, battle));

    ASSERT_TRUE(sendMonsterDeathRequestPacket(battle.clientA, battle.monsterId + 1));
    Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
    Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
    ASSERT_TRUE(recvErrorPacket(battle.clientA, failedType, errorCode));
    EXPECT_EQ(failedType, Net::TcpPacketType::kMonsterDeathRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kNotFound);

    ASSERT_TRUE(setReceiveTimeout(battle.clientB, std::chrono::milliseconds(50)));
    std::vector<uint8_t> unexpectedPacket;
    EXPECT_FALSE(recvPacket(battle.clientB, unexpectedPacket));
    ASSERT_TRUE(setReceiveTimeout(battle.clientB, std::chrono::milliseconds(500)));

    ASSERT_TRUE(sendMonsterDeathRequestPacket(battle.clientA, battle.monsterId));

    uint32_t deathRoomId = 0;
    uint32_t deadMonsterId = 0;
    ASSERT_TRUE(recvMonsterDeathPacket(battle.clientA, deathRoomId, deadMonsterId));
    EXPECT_EQ(deathRoomId, battle.roomId);
    EXPECT_EQ(deadMonsterId, battle.monsterId);
    ASSERT_TRUE(recvMonsterDeathPacket(battle.clientB, deathRoomId, deadMonsterId));
    EXPECT_EQ(deathRoomId, battle.roomId);
    EXPECT_EQ(deadMonsterId, battle.monsterId);

    uint32_t dropRoomId = 0;
    std::vector<Net::TcpDropEntry> drops;
    ASSERT_TRUE(recvDropListSnapshotPacket(battle.clientA, dropRoomId, drops));
    EXPECT_EQ(dropRoomId, battle.roomId);
    ASSERT_TRUE(recvDropListSnapshotPacket(battle.clientB, dropRoomId, drops));
    EXPECT_EQ(dropRoomId, battle.roomId);

    ASSERT_TRUE(sendMonsterDeathRequestPacket(battle.clientB, battle.monsterId));
    ASSERT_TRUE(recvErrorPacket(battle.clientB, failedType, errorCode));
    EXPECT_EQ(failedType, Net::TcpPacketType::kMonsterDeathRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kNotFound);

    ::close(battle.clientB);
    ::close(battle.clientA);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}

TEST(ServerRoomIntegrationTests, ResetsMonsterStateWhenPlayerLeavesBeforeDeath) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    SpawnedBattleClients battle;
    ASSERT_TRUE(prepareTwoPlayerBattleWithMonster(runningServer, battle));

    ASSERT_TRUE(sendLeaveRoomRequestPacket(battle.clientB));
    uint32_t leftRoomId = 0;
    ASSERT_TRUE(recvLeaveRoomResponsePacket(battle.clientB, leftRoomId));
    EXPECT_EQ(leftRoomId, battle.roomId);

    ASSERT_TRUE(expectRoomDetailState(
        battle.clientA,
        battle.roomId,
        {battle.sessionA},
        Net::TcpRoomStatus::kInProgress));
    ASSERT_TRUE(expectSingleRoomListSnapshot(
        battle.clientB,
        battle.roomId,
        1u,
        Net::TcpRoomStatus::kInProgress));

    ASSERT_TRUE(sendMonsterDeathRequestPacket(battle.clientA, battle.monsterId));
    Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
    Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
    ASSERT_TRUE(recvErrorPacket(battle.clientA, failedType, errorCode));
    EXPECT_EQ(failedType, Net::TcpPacketType::kMonsterDeathRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kNotFound);

    ::close(battle.clientB);
    ::close(battle.clientA);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 0u &&
                   runningServer.server.sessionCount() == 0u;
        }));
}
