#include <gtest/gtest.h>

#include <arpa/inet.h>
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

bool sendCreateRoomRequestPacket(int fd) {
    std::array<uint8_t, Net::kTcpHeaderSize> packet{};
    if (!Net::serializeCreateRoomRequestPacket(packet)) {
        return false;
    }
    return sendAll(fd, packet.data(), packet.size());
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

struct SpawnedBattleClients {
    int clientA{-1};
    int clientB{-1};
    uint64_t sessionA{0};
    uint64_t sessionB{0};
    uint32_t roomId{0};
    uint32_t monsterId{0};
    uint32_t monsterTypeId{0};
    uint16_t maxHp{0};
};

::testing::AssertionResult prepareTwoPlayerBattleWithMonster(
    uint16_t port,
    SpawnedBattleClients& out) {
    out.clientA = connectToServer(port);
    if (out.clientA < 0) {
        return ::testing::AssertionFailure() << "failed to connect client A";
    }
    if (!setReceiveTimeout(out.clientA, std::chrono::milliseconds(500))) {
        return ::testing::AssertionFailure() << "failed to set receive timeout for client A";
    }
    if (!recvWelcomePacket(out.clientA, out.sessionA)) {
        return ::testing::AssertionFailure() << "failed to receive welcome for client A";
    }

    std::vector<uint64_t> clientSnapshot;
    if (!recvClientListSnapshotPacket(out.clientA, clientSnapshot)) {
        return ::testing::AssertionFailure() << "failed to receive client snapshot for client A";
    }

    out.clientB = connectToServer(port);
    if (out.clientB < 0) {
        return ::testing::AssertionFailure() << "failed to connect client B";
    }
    if (!setReceiveTimeout(out.clientB, std::chrono::milliseconds(500))) {
        return ::testing::AssertionFailure() << "failed to set receive timeout for client B";
    }
    if (!recvWelcomePacket(out.clientB, out.sessionB)) {
        return ::testing::AssertionFailure() << "failed to receive welcome for client B";
    }
    if (!recvClientListSnapshotPacket(out.clientB, clientSnapshot) ||
        !recvClientListSnapshotPacket(out.clientA, clientSnapshot)) {
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

    std::vector<Net::TcpRoomEntry> roomSnapshot;
    if (!recvRoomListSnapshotPacket(out.clientA, roomSnapshot) ||
        !recvRoomListSnapshotPacket(out.clientB, roomSnapshot)) {
        return ::testing::AssertionFailure() << "failed to receive room snapshots after create";
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

    if (!recvRoomListSnapshotPacket(out.clientA, roomSnapshot) ||
        !recvRoomListSnapshotPacket(out.clientB, roomSnapshot)) {
        return ::testing::AssertionFailure() << "failed to receive room snapshots after join";
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
    std::vector<uint64_t> clientSnapshot;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientFd, clientSnapshot));
    EXPECT_EQ(clientSnapshot, (std::vector<uint64_t>{sessionId}));

    ASSERT_TRUE(sendCreateRoomRequestPacket(clientFd));

    uint32_t roomId = 0;
    uint16_t playerCount = 0;
    ASSERT_TRUE(recvCreateRoomResponsePacket(clientFd, roomId, playerCount));
    EXPECT_GT(roomId, 0u);
    EXPECT_EQ(playerCount, 1u);

    std::vector<Net::TcpRoomEntry> roomSnapshot;
    ASSERT_TRUE(recvRoomListSnapshotPacket(clientFd, roomSnapshot));
    ASSERT_EQ(roomSnapshot.size(), 1u);
    EXPECT_EQ(roomSnapshot[0].roomId, roomId);
    EXPECT_EQ(roomSnapshot[0].playerCount, 1u);
    EXPECT_EQ(roomSnapshot[0].maxPlayers, 2u);

    ASSERT_TRUE(sendLeaveRoomRequestPacket(clientFd));

    uint32_t leftRoomId = 0;
    ASSERT_TRUE(recvLeaveRoomResponsePacket(clientFd, leftRoomId));
    EXPECT_EQ(leftRoomId, roomId);

    ASSERT_TRUE(recvRoomListSnapshotPacket(clientFd, roomSnapshot));
    EXPECT_TRUE(roomSnapshot.empty());

    ::close(clientFd);
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
    ASSERT_TRUE(setReceiveTimeout(clientA, std::chrono::milliseconds(500)));
    uint64_t sessionA = 0;
    ASSERT_TRUE(recvWelcomePacket(clientA, sessionA));
    std::vector<uint64_t> clientSnapshot;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientA, clientSnapshot));

    int clientB = connectToServer(port);
    ASSERT_GE(clientB, 0);
    ASSERT_TRUE(setReceiveTimeout(clientB, std::chrono::milliseconds(500)));
    uint64_t sessionB = 0;
    ASSERT_TRUE(recvWelcomePacket(clientB, sessionB));
    ASSERT_TRUE(recvClientListSnapshotPacket(clientB, clientSnapshot));
    ASSERT_TRUE(recvClientListSnapshotPacket(clientA, clientSnapshot));

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

    std::vector<Net::TcpRoomEntry> roomSnapshot;
    ASSERT_TRUE(recvRoomListSnapshotPacket(clientA, roomSnapshot));
    ASSERT_EQ(roomSnapshot.size(), 1u);
    EXPECT_EQ(roomSnapshot[0].roomId, roomId);
    EXPECT_EQ(roomSnapshot[0].playerCount, 1u);

    ASSERT_TRUE(recvRoomListSnapshotPacket(clientB, roomSnapshot));
    ASSERT_EQ(roomSnapshot.size(), 1u);
    EXPECT_EQ(roomSnapshot[0].roomId, roomId);
    EXPECT_EQ(roomSnapshot[0].playerCount, 1u);

    ASSERT_TRUE(sendJoinRoomRequestPacket(clientB, roomId));
    ASSERT_TRUE(recvJoinRoomResponsePacket(clientB, roomId, playerCount));
    EXPECT_EQ(playerCount, 2u);

    ASSERT_TRUE(recvRoomListSnapshotPacket(clientA, roomSnapshot));
    ASSERT_EQ(roomSnapshot.size(), 1u);
    EXPECT_EQ(roomSnapshot[0].roomId, roomId);
    EXPECT_EQ(roomSnapshot[0].playerCount, 2u);
    EXPECT_EQ(roomSnapshot[0].maxPlayers, 2u);

    ASSERT_TRUE(recvRoomListSnapshotPacket(clientB, roomSnapshot));
    ASSERT_EQ(roomSnapshot.size(), 1u);
    EXPECT_EQ(roomSnapshot[0].roomId, roomId);
    EXPECT_EQ(roomSnapshot[0].playerCount, 2u);
    EXPECT_EQ(roomSnapshot[0].maxPlayers, 2u);

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

    auto expectClientSnapshot = [](int fd, std::vector<uint64_t> expectedSessionIds) {
        std::vector<uint64_t> actualSessionIds;
        if (!recvClientListSnapshotPacket(fd, actualSessionIds)) {
            return ::testing::AssertionFailure() << "failed to receive client snapshot";
        }

        std::sort(expectedSessionIds.begin(), expectedSessionIds.end());
        if (actualSessionIds != expectedSessionIds) {
            return ::testing::AssertionFailure()
                   << "unexpected client snapshot size=" << actualSessionIds.size();
        }

        return ::testing::AssertionSuccess();
    };

    auto expectSingleRoomSnapshot = [](int fd, uint32_t expectedRoomId, uint16_t expectedPlayerCount) {
        std::vector<Net::TcpRoomEntry> roomSnapshot;
        if (!recvRoomListSnapshotPacket(fd, roomSnapshot)) {
            return ::testing::AssertionFailure() << "failed to receive room snapshot";
        }

        if (roomSnapshot.size() != 1u) {
            return ::testing::AssertionFailure()
                   << "expected one room entry but received " << roomSnapshot.size();
        }

        if (roomSnapshot[0].roomId != expectedRoomId ||
            roomSnapshot[0].playerCount != expectedPlayerCount ||
            roomSnapshot[0].maxPlayers != 2u) {
            return ::testing::AssertionFailure()
                   << "unexpected room snapshot roomId=" << roomSnapshot[0].roomId
                   << " playerCount=" << roomSnapshot[0].playerCount
                   << " maxPlayers=" << roomSnapshot[0].maxPlayers;
        }

        return ::testing::AssertionSuccess();
    };

    auto expectEmptyRoomSnapshot = [](int fd) {
        std::vector<Net::TcpRoomEntry> roomSnapshot;
        if (!recvRoomListSnapshotPacket(fd, roomSnapshot)) {
            return ::testing::AssertionFailure() << "failed to receive empty room snapshot";
        }

        if (!roomSnapshot.empty()) {
            return ::testing::AssertionFailure()
                   << "expected empty room snapshot but received " << roomSnapshot.size()
                   << " entries";
        }

        return ::testing::AssertionSuccess();
    };

    int clientA = connectToServer(port);
    ASSERT_GE(clientA, 0);
    ASSERT_TRUE(setReceiveTimeout(clientA, std::chrono::milliseconds(500)));

    uint64_t sessionA = 0;
    ASSERT_TRUE(recvWelcomePacket(clientA, sessionA));
    ASSERT_TRUE(expectClientSnapshot(clientA, {sessionA}));

    int clientB = connectToServer(port);
    ASSERT_GE(clientB, 0);
    ASSERT_TRUE(setReceiveTimeout(clientB, std::chrono::milliseconds(500)));

    uint64_t sessionB = 0;
    ASSERT_TRUE(recvWelcomePacket(clientB, sessionB));
    ASSERT_TRUE(expectClientSnapshot(clientB, {sessionA, sessionB}));
    ASSERT_TRUE(expectClientSnapshot(clientA, {sessionA, sessionB}));

    ASSERT_TRUE(sendCreateRoomRequestPacket(clientA));

    uint32_t roomId = 0;
    uint16_t playerCount = 0;
    ASSERT_TRUE(recvCreateRoomResponsePacket(clientA, roomId, playerCount));
    EXPECT_GT(roomId, 0u);
    EXPECT_EQ(playerCount, 1u);

    ASSERT_TRUE(expectSingleRoomSnapshot(clientA, roomId, 1u));
    ASSERT_TRUE(expectSingleRoomSnapshot(clientB, roomId, 1u));

    uint32_t joinedRoomId = 0;
    ASSERT_TRUE(sendJoinRoomRequestPacket(clientB, roomId));
    ASSERT_TRUE(recvJoinRoomResponsePacket(clientB, joinedRoomId, playerCount));
    EXPECT_EQ(joinedRoomId, roomId);
    EXPECT_EQ(playerCount, 2u);

    ASSERT_TRUE(expectSingleRoomSnapshot(clientA, roomId, 2u));
    ASSERT_TRUE(expectSingleRoomSnapshot(clientB, roomId, 2u));

    int clientC = connectToServer(port);
    ASSERT_GE(clientC, 0);
    ASSERT_TRUE(setReceiveTimeout(clientC, std::chrono::milliseconds(500)));

    uint64_t sessionC = 0;
    ASSERT_TRUE(recvWelcomePacket(clientC, sessionC));
    ASSERT_TRUE(expectClientSnapshot(clientC, {sessionA, sessionB, sessionC}));
    ASSERT_TRUE(expectClientSnapshot(clientA, {sessionA, sessionB, sessionC}));
    ASSERT_TRUE(expectClientSnapshot(clientB, {sessionA, sessionB, sessionC}));

    ASSERT_TRUE(sendJoinRoomRequestPacket(clientC, roomId));
    Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
    Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
    ASSERT_TRUE(recvErrorPacket(clientC, failedType, errorCode));
    EXPECT_EQ(failedType, Net::TcpPacketType::kJoinRoomRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kFull);
    EXPECT_TRUE(waitUntil(
        [&runningServer]() {
            return runningServer.server.activeConnectionCount() == 3u &&
                   runningServer.server.sessionCount() == 3u;
        }));

    uint32_t leftRoomId = 0;
    ASSERT_TRUE(sendLeaveRoomRequestPacket(clientB));
    ASSERT_TRUE(recvLeaveRoomResponsePacket(clientB, leftRoomId));
    EXPECT_EQ(leftRoomId, roomId);

    ASSERT_TRUE(expectSingleRoomSnapshot(clientA, roomId, 1u));
    ASSERT_TRUE(expectSingleRoomSnapshot(clientB, roomId, 1u));
    ASSERT_TRUE(expectSingleRoomSnapshot(clientC, roomId, 1u));

    ASSERT_TRUE(sendLeaveRoomRequestPacket(clientA));
    ASSERT_TRUE(recvLeaveRoomResponsePacket(clientA, leftRoomId));
    EXPECT_EQ(leftRoomId, roomId);

    ASSERT_TRUE(expectEmptyRoomSnapshot(clientA));
    ASSERT_TRUE(expectEmptyRoomSnapshot(clientB));
    ASSERT_TRUE(expectEmptyRoomSnapshot(clientC));

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
    ASSERT_TRUE(setReceiveTimeout(clientA, std::chrono::milliseconds(500)));
    uint64_t sessionA = 0;
    ASSERT_TRUE(recvWelcomePacket(clientA, sessionA));
    std::vector<uint64_t> clientSnapshot;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientA, clientSnapshot));

    int clientB = connectToServer(port);
    ASSERT_GE(clientB, 0);
    ASSERT_TRUE(setReceiveTimeout(clientB, std::chrono::milliseconds(500)));
    uint64_t sessionB = 0;
    ASSERT_TRUE(recvWelcomePacket(clientB, sessionB));
    ASSERT_TRUE(recvClientListSnapshotPacket(clientB, clientSnapshot));
    ASSERT_TRUE(recvClientListSnapshotPacket(clientA, clientSnapshot));

    ASSERT_TRUE(sendCreateRoomRequestPacket(clientA));
    uint32_t roomId = 0;
    uint16_t playerCount = 0;
    ASSERT_TRUE(recvCreateRoomResponsePacket(clientA, roomId, playerCount));
    EXPECT_EQ(playerCount, 1u);

    std::vector<Net::TcpRoomEntry> roomSnapshot;
    ASSERT_TRUE(recvRoomListSnapshotPacket(clientA, roomSnapshot));
    ASSERT_TRUE(recvRoomListSnapshotPacket(clientB, roomSnapshot));

    ASSERT_TRUE(sendJoinRoomRequestPacket(clientB, roomId));
    uint32_t joinedRoomId = 0;
    ASSERT_TRUE(recvJoinRoomResponsePacket(clientB, joinedRoomId, playerCount));
    EXPECT_EQ(joinedRoomId, roomId);
    EXPECT_EQ(playerCount, 2u);

    ASSERT_TRUE(recvRoomListSnapshotPacket(clientA, roomSnapshot));
    ASSERT_TRUE(recvRoomListSnapshotPacket(clientB, roomSnapshot));

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
    ASSERT_TRUE(recvReadyRoomResponsePacket(
        clientA,
        readyRoomId,
        readyPlayerCount,
        totalPlayerCount));
    EXPECT_EQ(readyRoomId, roomId);
    EXPECT_EQ(readyPlayerCount, 2u);
    EXPECT_EQ(totalPlayerCount, 2u);

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

TEST(ServerRoomIntegrationTests, ReproducesWeek4MonsterDropScenario) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    SpawnedBattleClients battle;
    ASSERT_TRUE(prepareTwoPlayerBattleWithMonster(port, battle));

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
    ASSERT_TRUE(prepareTwoPlayerBattleWithMonster(port, battle));

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

    ASSERT_TRUE(setReceiveTimeout(battle.clientB, std::chrono::milliseconds(50)));
    std::vector<uint8_t> unexpectedPacket;
    EXPECT_FALSE(recvPacket(battle.clientB, unexpectedPacket));
    ASSERT_TRUE(setReceiveTimeout(battle.clientB, std::chrono::milliseconds(500)));

    ASSERT_TRUE(sendClickLootRequestPacket(battle.clientB, dropId));

    uint32_t rejectedRoomId = 0;
    uint32_t rejectedDropId = 0;
    Net::TcpLootRejectReason rejectReason = Net::TcpLootRejectReason::kNone;
    ASSERT_TRUE(recvLootRejectedPacket(
        battle.clientB,
        rejectedRoomId,
        rejectedDropId,
        rejectReason));
    EXPECT_EQ(rejectedRoomId, battle.roomId);
    EXPECT_EQ(rejectedDropId, dropId);
    EXPECT_EQ(rejectReason, Net::TcpLootRejectReason::kAlreadyClaimed);

    ASSERT_TRUE(setReceiveTimeout(battle.clientA, std::chrono::milliseconds(50)));
    ASSERT_TRUE(setReceiveTimeout(battle.clientB, std::chrono::milliseconds(50)));
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

TEST(ServerRoomIntegrationTests, FinishSessionReturnsIdempotentSettlementResult) {
    RunningServer runningServer(0);
    ASSERT_TRUE(runningServer.server.start());
    const uint16_t port = runningServer.server.boundPort();
    ASSERT_GT(port, 0);

    runningServer.thread = std::thread([&runningServer]() { runningServer.server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    SpawnedBattleClients battle;
    ASSERT_TRUE(prepareTwoPlayerBattleWithMonster(port, battle));

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
    std::vector<uint64_t> clientSnapshot;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientFd, clientSnapshot));

    ASSERT_TRUE(sendCreateRoomRequestPacket(clientFd));

    uint32_t roomId = 0;
    uint16_t playerCount = 0;
    ASSERT_TRUE(recvCreateRoomResponsePacket(clientFd, roomId, playerCount));
    EXPECT_GT(roomId, 0u);
    EXPECT_EQ(playerCount, 1u);

    std::vector<Net::TcpRoomEntry> roomSnapshot;
    ASSERT_TRUE(recvRoomListSnapshotPacket(clientFd, roomSnapshot));
    ASSERT_EQ(roomSnapshot.size(), 1u);
    EXPECT_EQ(roomSnapshot[0].roomId, roomId);
    EXPECT_EQ(roomSnapshot[0].playerCount, 1u);

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
    std::vector<uint64_t> clientSnapshot;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientFd, clientSnapshot));

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
    std::vector<uint64_t> clientSnapshot;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientFd, clientSnapshot));

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
    std::vector<uint64_t> clientSnapshot;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientFd, clientSnapshot));

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
    std::vector<uint64_t> clientSnapshot;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientFd, clientSnapshot));

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
    ASSERT_TRUE(prepareTwoPlayerBattleWithMonster(port, battle));

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
    ASSERT_TRUE(prepareTwoPlayerBattleWithMonster(port, battle));

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
    ASSERT_TRUE(prepareTwoPlayerBattleWithMonster(port, battle));

    uint32_t dropId = 0;
    uint32_t itemId = 0;
    uint16_t quantity = 0;
    ASSERT_TRUE(defeatMonsterAndReceiveDrop(battle, battle.clientA, dropId, itemId, quantity));

    ASSERT_TRUE(sendLeaveRoomRequestPacket(battle.clientB));
    uint32_t leftRoomId = 0;
    ASSERT_TRUE(recvLeaveRoomResponsePacket(battle.clientB, leftRoomId));
    EXPECT_EQ(leftRoomId, battle.roomId);

    std::vector<Net::TcpRoomEntry> roomSnapshot;
    ASSERT_TRUE(recvRoomListSnapshotPacket(battle.clientA, roomSnapshot));
    ASSERT_EQ(roomSnapshot.size(), 1u);
    EXPECT_EQ(roomSnapshot[0].roomId, battle.roomId);
    EXPECT_EQ(roomSnapshot[0].playerCount, 1u);

    ASSERT_TRUE(recvRoomListSnapshotPacket(battle.clientB, roomSnapshot));
    ASSERT_EQ(roomSnapshot.size(), 1u);
    EXPECT_EQ(roomSnapshot[0].roomId, battle.roomId);
    EXPECT_EQ(roomSnapshot[0].playerCount, 1u);

    ASSERT_TRUE(sendClickLootRequestPacket(battle.clientA, dropId));
    Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
    Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
    ASSERT_TRUE(recvErrorPacket(battle.clientA, failedType, errorCode));
    EXPECT_EQ(failedType, Net::TcpPacketType::kClickLootRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kNotFound);

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
    std::vector<uint64_t> clientSnapshot;
    ASSERT_TRUE(recvClientListSnapshotPacket(clientA, clientSnapshot));

    int clientB = connectToServer(port);
    ASSERT_GE(clientB, 0);
    ASSERT_TRUE(setReceiveTimeout(clientB, std::chrono::milliseconds(500)));
    uint64_t sessionB = 0;
    ASSERT_TRUE(recvWelcomePacket(clientB, sessionB));
    ASSERT_TRUE(recvClientListSnapshotPacket(clientB, clientSnapshot));
    ASSERT_TRUE(recvClientListSnapshotPacket(clientA, clientSnapshot));

    ASSERT_TRUE(sendCreateRoomRequestPacket(clientA));
    uint32_t roomId = 0;
    uint16_t playerCount = 0;
    ASSERT_TRUE(recvCreateRoomResponsePacket(clientA, roomId, playerCount));

    std::vector<Net::TcpRoomEntry> roomSnapshot;
    ASSERT_TRUE(recvRoomListSnapshotPacket(clientA, roomSnapshot));
    ASSERT_TRUE(recvRoomListSnapshotPacket(clientB, roomSnapshot));

    ASSERT_TRUE(sendJoinRoomRequestPacket(clientB, roomId));
    uint32_t joinedRoomId = 0;
    ASSERT_TRUE(recvJoinRoomResponsePacket(clientB, joinedRoomId, playerCount));
    EXPECT_EQ(joinedRoomId, roomId);

    ASSERT_TRUE(recvRoomListSnapshotPacket(clientA, roomSnapshot));
    ASSERT_TRUE(recvRoomListSnapshotPacket(clientB, roomSnapshot));

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

    ASSERT_TRUE(sendLeaveRoomRequestPacket(clientB));
    uint32_t leftRoomId = 0;
    ASSERT_TRUE(recvLeaveRoomResponsePacket(clientB, leftRoomId));
    EXPECT_EQ(leftRoomId, roomId);

    ASSERT_TRUE(recvRoomListSnapshotPacket(clientA, roomSnapshot));
    ASSERT_EQ(roomSnapshot.size(), 1u);
    EXPECT_EQ(roomSnapshot[0].roomId, roomId);
    EXPECT_EQ(roomSnapshot[0].playerCount, 1u);

    ASSERT_TRUE(recvRoomListSnapshotPacket(clientB, roomSnapshot));
    ASSERT_EQ(roomSnapshot.size(), 1u);
    EXPECT_EQ(roomSnapshot[0].roomId, roomId);
    EXPECT_EQ(roomSnapshot[0].playerCount, 1u);

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
    ASSERT_TRUE(prepareTwoPlayerBattleWithMonster(port, battle));

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
    ASSERT_TRUE(prepareTwoPlayerBattleWithMonster(port, battle));

    ASSERT_TRUE(sendLeaveRoomRequestPacket(battle.clientB));
    uint32_t leftRoomId = 0;
    ASSERT_TRUE(recvLeaveRoomResponsePacket(battle.clientB, leftRoomId));
    EXPECT_EQ(leftRoomId, battle.roomId);

    std::vector<Net::TcpRoomEntry> roomSnapshot;
    ASSERT_TRUE(recvRoomListSnapshotPacket(battle.clientA, roomSnapshot));
    ASSERT_EQ(roomSnapshot.size(), 1u);
    EXPECT_EQ(roomSnapshot[0].roomId, battle.roomId);
    EXPECT_EQ(roomSnapshot[0].playerCount, 1u);

    ASSERT_TRUE(recvRoomListSnapshotPacket(battle.clientB, roomSnapshot));
    ASSERT_EQ(roomSnapshot.size(), 1u);
    EXPECT_EQ(roomSnapshot[0].roomId, battle.roomId);
    EXPECT_EQ(roomSnapshot[0].playerCount, 1u);

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
