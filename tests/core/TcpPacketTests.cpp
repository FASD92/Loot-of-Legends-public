#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "Net/TcpPacket.hpp"

TEST(TcpPacketTests, SerializeAndParseWelcomePacket) {
    std::array<uint8_t, Net::kWelcomePacketSize> packet{};
    ASSERT_TRUE(Net::serializeWelcomePacket(42, packet));

    Net::TcpPacketHeader header;
    uint64_t sessionId = 0;
    ASSERT_TRUE(Net::parseWelcomePacket(packet.data(), packet.size(), header, sessionId));

    EXPECT_EQ(header.size, Net::kWelcomePacketSize);
    EXPECT_EQ(header.type, Net::TcpPacketType::kWelcome);
    EXPECT_EQ(sessionId, 42u);
}

TEST(TcpPacketTests, RejectInvalidPacketSize) {
    std::array<uint8_t, Net::kWelcomePacketSize> packet{};
    ASSERT_TRUE(Net::serializeWelcomePacket(7, packet));

    Net::TcpPacketHeader header;
    EXPECT_FALSE(Net::parseTcpPacketHeader(packet.data(), packet.size() - 1, header));
}

TEST(TcpPacketTests, SerializeAndParseClientListSnapshotPacket) {
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeClientListSnapshotPacket({3, 7, 11}, packet));

    Net::TcpPacketHeader header;
    std::vector<uint64_t> sessionIds;
    ASSERT_TRUE(Net::parseClientListSnapshotPacket(packet.data(), packet.size(), header, sessionIds));

    EXPECT_EQ(header.type, Net::TcpPacketType::kClientListSnapshot);
    EXPECT_EQ(header.size, Net::clientListSnapshotPacketSize(3));
    EXPECT_EQ(sessionIds, (std::vector<uint64_t>{3, 7, 11}));
}

TEST(TcpPacketTests, RejectClientListSnapshotWithInvalidCountSize) {
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeClientListSnapshotPacket({1, 2}, packet));

    packet[4] = 0x00;
    packet[5] = 0x03;

    Net::TcpPacketHeader header;
    std::vector<uint64_t> sessionIds;
    EXPECT_FALSE(Net::parseClientListSnapshotPacket(packet.data(), packet.size(), header, sessionIds));
}

TEST(TcpPacketTests, SerializeAndParseCreateRoomResponsePacket) {
    std::array<uint8_t, Net::kRoomStatusPacketSize> packet{};
    ASSERT_TRUE(Net::serializeCreateRoomResponsePacket(17, 1, packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    uint16_t playerCount = 0;
    ASSERT_TRUE(Net::parseCreateRoomResponsePacket(
        packet.data(),
        packet.size(),
        header,
        roomId,
        playerCount));

    EXPECT_EQ(header.type, Net::TcpPacketType::kCreateRoomResponse);
    EXPECT_EQ(header.size, Net::kRoomStatusPacketSize);
    EXPECT_EQ(roomId, 17u);
    EXPECT_EQ(playerCount, 1u);
}

TEST(TcpPacketTests, SerializeAndParseJoinRoomRequestPacket) {
    std::array<uint8_t, Net::kRoomIdPacketSize> packet{};
    ASSERT_TRUE(Net::serializeJoinRoomRequestPacket(99, packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    ASSERT_TRUE(Net::parseJoinRoomRequestPacket(packet.data(), packet.size(), header, roomId));

    EXPECT_EQ(header.type, Net::TcpPacketType::kJoinRoomRequest);
    EXPECT_EQ(header.size, Net::kRoomIdPacketSize);
    EXPECT_EQ(roomId, 99u);
}

TEST(TcpPacketTests, SerializeAndParseReadyRoomResponsePacket) {
    std::array<uint8_t, Net::kReadyRoomStatusPacketSize> packet{};
    ASSERT_TRUE(Net::serializeReadyRoomResponsePacket(17, 2, 2, packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    uint16_t readyPlayerCount = 0;
    uint16_t totalPlayerCount = 0;
    ASSERT_TRUE(Net::parseReadyRoomResponsePacket(
        packet.data(),
        packet.size(),
        header,
        roomId,
        readyPlayerCount,
        totalPlayerCount));

    EXPECT_EQ(header.type, Net::TcpPacketType::kReadyRoomResponse);
    EXPECT_EQ(header.size, Net::kReadyRoomStatusPacketSize);
    EXPECT_EQ(roomId, 17u);
    EXPECT_EQ(readyPlayerCount, 2u);
    EXPECT_EQ(totalPlayerCount, 2u);
}

TEST(TcpPacketTests, RejectReadyRoomResponseWithInvalidSize) {
    std::array<uint8_t, Net::kReadyRoomStatusPacketSize> packet{};
    ASSERT_TRUE(Net::serializeReadyRoomResponsePacket(17, 1, 2, packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    uint16_t readyPlayerCount = 0;
    uint16_t totalPlayerCount = 0;
    EXPECT_FALSE(Net::parseReadyRoomResponsePacket(
        packet.data(),
        packet.size() - 1,
        header,
        roomId,
        readyPlayerCount,
        totalPlayerCount));
}

TEST(TcpPacketTests, SerializeAndParseRoomListSnapshotPacket) {
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeRoomListSnapshotPacket(
        {
            Net::TcpRoomEntry{1, 1, 2},
            Net::TcpRoomEntry{7, 2, 2},
        },
        packet));

    Net::TcpPacketHeader header;
    std::vector<Net::TcpRoomEntry> rooms;
    ASSERT_TRUE(Net::parseRoomListSnapshotPacket(packet.data(), packet.size(), header, rooms));

    EXPECT_EQ(header.type, Net::TcpPacketType::kRoomListSnapshot);
    EXPECT_EQ(header.size, Net::roomListSnapshotPacketSize(2));
    ASSERT_EQ(rooms.size(), 2u);
    EXPECT_EQ(rooms[0].roomId, 1u);
    EXPECT_EQ(rooms[0].playerCount, 1u);
    EXPECT_EQ(rooms[0].maxPlayers, 2u);
    EXPECT_EQ(rooms[1].roomId, 7u);
    EXPECT_EQ(rooms[1].playerCount, 2u);
    EXPECT_EQ(rooms[1].maxPlayers, 2u);
}

TEST(TcpPacketTests, RejectRoomListSnapshotWithInvalidCountSize) {
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeRoomListSnapshotPacket(
        {
            Net::TcpRoomEntry{1, 1, 2},
            Net::TcpRoomEntry{2, 2, 2},
        },
        packet));

    packet[4] = 0x00;
    packet[5] = 0x03;

    Net::TcpPacketHeader header;
    std::vector<Net::TcpRoomEntry> rooms;
    EXPECT_FALSE(Net::parseRoomListSnapshotPacket(packet.data(), packet.size(), header, rooms));
}

TEST(TcpPacketTests, RejectRoomListSnapshotThatExceedsMaxPacketSize) {
    std::vector<Net::TcpRoomEntry> rooms(128, Net::TcpRoomEntry{1, 1, 2});
    std::vector<uint8_t> packet;
    EXPECT_FALSE(Net::serializeRoomListSnapshotPacket(rooms, packet));
}

TEST(TcpPacketTests, SerializeAndParseErrorPacket) {
    std::array<uint8_t, Net::kErrorPacketSize> packet{};
    ASSERT_TRUE(Net::serializeErrorPacket(
        Net::TcpPacketType::kJoinRoomRequest,
        Net::TcpErrorCode::kFull,
        packet));

    Net::TcpPacketHeader header;
    Net::TcpPacketType failedType = Net::TcpPacketType::kWelcome;
    Net::TcpErrorCode errorCode = Net::TcpErrorCode::kNone;
    ASSERT_TRUE(Net::parseErrorPacket(
        packet.data(),
        packet.size(),
        header,
        failedType,
        errorCode));

    EXPECT_EQ(header.type, Net::TcpPacketType::kError);
    EXPECT_EQ(header.size, Net::kErrorPacketSize);
    EXPECT_EQ(failedType, Net::TcpPacketType::kJoinRoomRequest);
    EXPECT_EQ(errorCode, Net::TcpErrorCode::kFull);
}

TEST(TcpPacketTests, SerializeAndParseBattleStartPacket) {
    std::array<uint8_t, Net::kBattleStartPacketSize> packet{};
    ASSERT_TRUE(Net::serializeBattleStartPacket(42, 1001, 1002, packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    uint64_t playerASessionId = 0;
    uint64_t playerBSessionId = 0;
    ASSERT_TRUE(Net::parseBattleStartPacket(
        packet.data(),
        packet.size(),
        header,
        roomId,
        playerASessionId,
        playerBSessionId));

    EXPECT_EQ(header.type, Net::TcpPacketType::kBattleStart);
    EXPECT_EQ(header.size, Net::kBattleStartPacketSize);
    EXPECT_EQ(roomId, 42u);
    EXPECT_EQ(playerASessionId, 1001u);
    EXPECT_EQ(playerBSessionId, 1002u);
}

TEST(TcpPacketTests, SerializeAndParseMonsterSpawnPacket) {
    std::array<uint8_t, Net::kMonsterSpawnPacketSize> packet{};
    ASSERT_TRUE(Net::serializeMonsterSpawnPacket(42, 7, 100, 30, packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    uint32_t monsterId = 0;
    uint32_t monsterTypeId = 0;
    uint16_t maxHp = 0;
    ASSERT_TRUE(Net::parseMonsterSpawnPacket(
        packet.data(),
        packet.size(),
        header,
        roomId,
        monsterId,
        monsterTypeId,
        maxHp));

    EXPECT_EQ(header.type, Net::TcpPacketType::kMonsterSpawn);
    EXPECT_EQ(header.size, Net::kMonsterSpawnPacketSize);
    EXPECT_EQ(roomId, 42u);
    EXPECT_EQ(monsterId, 7u);
    EXPECT_EQ(monsterTypeId, 100u);
    EXPECT_EQ(maxHp, 30u);
}

TEST(TcpPacketTests, SerializeAndParseMonsterDeathRequestPacket) {
    std::array<uint8_t, Net::kMonsterDeathRequestPacketSize> packet{};
    ASSERT_TRUE(Net::serializeMonsterDeathRequestPacket(7, packet));

    Net::TcpPacketHeader header;
    uint32_t monsterId = 0;
    ASSERT_TRUE(Net::parseMonsterDeathRequestPacket(packet.data(), packet.size(), header, monsterId));

    EXPECT_EQ(header.type, Net::TcpPacketType::kMonsterDeathRequest);
    EXPECT_EQ(header.size, Net::kMonsterDeathRequestPacketSize);
    EXPECT_EQ(monsterId, 7u);
}

TEST(TcpPacketTests, RejectMonsterDeathRequestWithInvalidSize) {
    std::array<uint8_t, Net::kMonsterDeathRequestPacketSize> packet{};
    ASSERT_TRUE(Net::serializeMonsterDeathRequestPacket(7, packet));

    Net::TcpPacketHeader header;
    uint32_t monsterId = 0;
    EXPECT_FALSE(Net::parseMonsterDeathRequestPacket(
        packet.data(),
        packet.size() - 1,
        header,
        monsterId));
}

TEST(TcpPacketTests, SerializeAndParseMonsterDeathPacket) {
    std::array<uint8_t, Net::kMonsterDeathPacketSize> packet{};
    ASSERT_TRUE(Net::serializeMonsterDeathPacket(42, 7, packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    uint32_t monsterId = 0;
    ASSERT_TRUE(Net::parseMonsterDeathPacket(packet.data(), packet.size(), header, roomId, monsterId));

    EXPECT_EQ(header.type, Net::TcpPacketType::kMonsterDeath);
    EXPECT_EQ(header.size, Net::kMonsterDeathPacketSize);
    EXPECT_EQ(roomId, 42u);
    EXPECT_EQ(monsterId, 7u);
}

TEST(TcpPacketTests, SerializeAndParseDropListSnapshotPacket) {
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeDropListSnapshotPacket(
        42,
        {
            Net::TcpDropEntry{1, 1001, 1},
            Net::TcpDropEntry{2, 1002, 3},
        },
        packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    std::vector<Net::TcpDropEntry> drops;
    ASSERT_TRUE(Net::parseDropListSnapshotPacket(packet.data(), packet.size(), header, roomId, drops));

    EXPECT_EQ(header.type, Net::TcpPacketType::kDropListSnapshot);
    EXPECT_EQ(header.size, Net::dropListSnapshotPacketSize(2));
    EXPECT_EQ(roomId, 42u);
    ASSERT_EQ(drops.size(), 2u);
    EXPECT_EQ(drops[0].dropId, 1u);
    EXPECT_EQ(drops[0].itemId, 1001u);
    EXPECT_EQ(drops[0].quantity, 1u);
    EXPECT_EQ(drops[1].dropId, 2u);
    EXPECT_EQ(drops[1].itemId, 1002u);
    EXPECT_EQ(drops[1].quantity, 3u);
}

TEST(TcpPacketTests, RejectDropListSnapshotWithInvalidCountSize) {
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeDropListSnapshotPacket(
        42,
        {
            Net::TcpDropEntry{1, 1001, 1},
            Net::TcpDropEntry{2, 1002, 3},
        },
        packet));

    packet[Net::kTcpHeaderSize + Net::kRoomIdFieldSize] = 0x00;
    packet[Net::kTcpHeaderSize + Net::kRoomIdFieldSize + 1] = 0x03;

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    std::vector<Net::TcpDropEntry> drops;
    EXPECT_FALSE(Net::parseDropListSnapshotPacket(packet.data(), packet.size(), header, roomId, drops));
}

TEST(TcpPacketTests, RejectDropListSnapshotThatExceedsMaxPacketSize) {
    std::vector<Net::TcpDropEntry> drops(102, Net::TcpDropEntry{1, 1001, 1});
    std::vector<uint8_t> packet;
    EXPECT_FALSE(Net::serializeDropListSnapshotPacket(42, drops, packet));
}

TEST(TcpPacketTests, SerializeAndParseClickLootRequestPacket) {
    std::array<uint8_t, Net::kClickLootRequestPacketSize> packet{};
    ASSERT_TRUE(Net::serializeClickLootRequestPacket(77, packet));

    Net::TcpPacketHeader header;
    uint32_t dropId = 0;
    ASSERT_TRUE(Net::parseClickLootRequestPacket(packet.data(), packet.size(), header, dropId));

    EXPECT_EQ(header.type, Net::TcpPacketType::kClickLootRequest);
    EXPECT_EQ(header.size, Net::kClickLootRequestPacketSize);
    EXPECT_EQ(dropId, 77u);
}

TEST(TcpPacketTests, SerializeAndParseLootResolvedPacket) {
    std::array<uint8_t, Net::kLootResolvedPacketSize> packet{};
    ASSERT_TRUE(Net::serializeLootResolvedPacket(42, 77, 1001, 3001, 2, packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    uint32_t dropId = 0;
    uint64_t winnerSessionId = 0;
    uint32_t itemId = 0;
    uint16_t quantity = 0;
    ASSERT_TRUE(Net::parseLootResolvedPacket(
        packet.data(),
        packet.size(),
        header,
        roomId,
        dropId,
        winnerSessionId,
        itemId,
        quantity));

    EXPECT_EQ(header.type, Net::TcpPacketType::kLootResolved);
    EXPECT_EQ(header.size, Net::kLootResolvedPacketSize);
    EXPECT_EQ(roomId, 42u);
    EXPECT_EQ(dropId, 77u);
    EXPECT_EQ(winnerSessionId, 1001u);
    EXPECT_EQ(itemId, 3001u);
    EXPECT_EQ(quantity, 2u);
}

TEST(TcpPacketTests, SerializeAndParseLootRejectedPacket) {
    std::array<uint8_t, Net::kLootRejectedPacketSize> packet{};
    ASSERT_TRUE(Net::serializeLootRejectedPacket(
        42,
        77,
        Net::TcpLootRejectReason::kAlreadyClaimed,
        packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    uint32_t dropId = 0;
    Net::TcpLootRejectReason reason = Net::TcpLootRejectReason::kNone;
    ASSERT_TRUE(Net::parseLootRejectedPacket(
        packet.data(),
        packet.size(),
        header,
        roomId,
        dropId,
        reason));

    EXPECT_EQ(header.type, Net::TcpPacketType::kLootRejected);
    EXPECT_EQ(header.size, Net::kLootRejectedPacketSize);
    EXPECT_EQ(roomId, 42u);
    EXPECT_EQ(dropId, 77u);
    EXPECT_EQ(reason, Net::TcpLootRejectReason::kAlreadyClaimed);
}

TEST(TcpPacketTests, SerializeAndParseInventorySnapshotPacket) {
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeInventorySnapshotPacket(
        1001,
        3,
        10,
        {
            Net::TcpInventoryEntry{3001, 1},
            Net::TcpInventoryEntry{3002, 2},
        },
        packet));

    Net::TcpPacketHeader header;
    uint64_t sessionId = 0;
    uint16_t currentWeight = 0;
    uint16_t maxWeight = 0;
    std::vector<Net::TcpInventoryEntry> entries;
    ASSERT_TRUE(Net::parseInventorySnapshotPacket(
        packet.data(),
        packet.size(),
        header,
        sessionId,
        currentWeight,
        maxWeight,
        entries));

    EXPECT_EQ(header.type, Net::TcpPacketType::kInventorySnapshot);
    EXPECT_EQ(header.size, Net::inventorySnapshotPacketSize(2));
    EXPECT_EQ(sessionId, 1001u);
    EXPECT_EQ(currentWeight, 3u);
    EXPECT_EQ(maxWeight, 10u);
    ASSERT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].itemId, 3001u);
    EXPECT_EQ(entries[0].quantity, 1u);
    EXPECT_EQ(entries[1].itemId, 3002u);
    EXPECT_EQ(entries[1].quantity, 2u);
}

TEST(TcpPacketTests, RejectInventorySnapshotWithInvalidCountSize) {
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeInventorySnapshotPacket(
        1001,
        3,
        10,
        {
            Net::TcpInventoryEntry{3001, 1},
            Net::TcpInventoryEntry{3002, 2},
        },
        packet));

    const size_t countOffset =
        Net::kTcpHeaderSize + Net::kSessionIdFieldSize + (2 * Net::kWeightFieldSize);
    packet[countOffset] = 0x00;
    packet[countOffset + 1] = 0x03;

    Net::TcpPacketHeader header;
    uint64_t sessionId = 0;
    uint16_t currentWeight = 0;
    uint16_t maxWeight = 0;
    std::vector<Net::TcpInventoryEntry> entries;
    EXPECT_FALSE(Net::parseInventorySnapshotPacket(
        packet.data(),
        packet.size(),
        header,
        sessionId,
        currentWeight,
        maxWeight,
        entries));
}

TEST(TcpPacketTests, RejectInventorySnapshotThatExceedsMaxPacketSize) {
    std::vector<Net::TcpInventoryEntry> entries(168, Net::TcpInventoryEntry{3001, 1});
    std::vector<uint8_t> packet;
    EXPECT_FALSE(Net::serializeInventorySnapshotPacket(1001, 0, 10, entries, packet));
}

TEST(TcpPacketTests, SerializeAndParseFinishSessionRequestPacket) {
    std::array<uint8_t, Net::kFinishSessionRequestPacketSize> packet{};
    ASSERT_TRUE(Net::serializeFinishSessionRequestPacket(packet));

    Net::TcpPacketHeader header;
    ASSERT_TRUE(Net::parseFinishSessionRequestPacket(packet.data(), packet.size(), header));

    EXPECT_EQ(header.type, Net::TcpPacketType::kFinishSessionRequest);
    EXPECT_EQ(header.size, Net::kFinishSessionRequestPacketSize);
}

TEST(TcpPacketTests, SerializeAndParseSmokeCreateCenterDropRequestPacket) {
    static_assert(
        static_cast<uint16_t>(Net::TcpPacketType::kSmokeCreateCenterDropRequest) == 0x0116);

    std::array<uint8_t, Net::kSmokeCreateCenterDropRequestPacketSize> packet{};
    ASSERT_TRUE(Net::serializeSmokeCreateCenterDropRequestPacket(packet));

    Net::TcpPacketHeader header;
    ASSERT_TRUE(Net::parseSmokeCreateCenterDropRequestPacket(packet.data(), packet.size(), header));

    EXPECT_EQ(header.type, Net::TcpPacketType::kSmokeCreateCenterDropRequest);
    EXPECT_EQ(header.size, Net::kTcpHeaderSize);
}

TEST(TcpPacketTests, RejectSmokeCreateCenterDropRequestWithWrongSize) {
    std::array<uint8_t, Net::kSmokeCreateCenterDropRequestPacketSize> packet{};
    ASSERT_TRUE(Net::serializeSmokeCreateCenterDropRequestPacket(packet));
    std::vector<uint8_t> oversized(packet.begin(), packet.end());
    oversized.push_back(0);

    Net::TcpPacketHeader header;
    EXPECT_FALSE(Net::parseSmokeCreateCenterDropRequestPacket(
        oversized.data(),
        oversized.size(),
        header));
}

TEST(TcpPacketTests, SerializeAndParseSmokePlacePlayersAroundCenterDropRequestPacket) {
    static_assert(
        static_cast<uint16_t>(Net::TcpPacketType::kSmokePlacePlayersAroundCenterDropRequest) ==
        0x0117);

    std::array<uint8_t, Net::kSmokePlacePlayersAroundCenterDropRequestPacketSize> packet{};
    ASSERT_TRUE(Net::serializeSmokePlacePlayersAroundCenterDropRequestPacket(packet));

    Net::TcpPacketHeader header;
    ASSERT_TRUE(Net::parseSmokePlacePlayersAroundCenterDropRequestPacket(
        packet.data(),
        packet.size(),
        header));

    EXPECT_EQ(header.type, Net::TcpPacketType::kSmokePlacePlayersAroundCenterDropRequest);
    EXPECT_EQ(header.size, Net::kTcpHeaderSize);
}

TEST(TcpPacketTests, RejectSmokePlacePlayersAroundCenterDropRequestWithWrongSize) {
    std::array<uint8_t, Net::kSmokePlacePlayersAroundCenterDropRequestPacketSize> packet{};
    ASSERT_TRUE(Net::serializeSmokePlacePlayersAroundCenterDropRequestPacket(packet));
    std::vector<uint8_t> oversized(packet.begin(), packet.end());
    oversized.push_back(0);

    Net::TcpPacketHeader header;
    EXPECT_FALSE(Net::parseSmokePlacePlayersAroundCenterDropRequestPacket(
        oversized.data(),
        oversized.size(),
        header));
}

TEST(TcpPacketTests, SerializeAndParseSettlementResultPacket) {
    Net::TcpSettlementResult settlement;
    settlement.settlementId = "room-42-session-1001-finish-1";
    settlement.sessionId = 1001;
    settlement.accountId = 1001;
    settlement.roomId = 42;
    settlement.startedAtUnixMs = 1710000000000;
    settlement.finishedAtUnixMs = 1710000005000;
    settlement.goldDelta = -25;
    settlement.reason = Net::TcpSettlementReason::kNormal;
    settlement.inventoryDeltas = {
        Net::TcpSettlementInventoryDelta{3001, 2, 77},
        Net::TcpSettlementInventoryDelta{3002, -1, 78},
    };

    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeSettlementResultPacket(settlement, packet));

    Net::TcpPacketHeader header;
    Net::TcpSettlementResult parsed;
    ASSERT_TRUE(Net::parseSettlementResultPacket(packet.data(), packet.size(), header, parsed));

    EXPECT_EQ(header.type, Net::TcpPacketType::kSettlementResult);
    EXPECT_EQ(header.size, Net::settlementResultPacketSize(settlement.settlementId.size(), 2));
    EXPECT_EQ(parsed.settlementId, settlement.settlementId);
    EXPECT_EQ(parsed.sessionId, 1001u);
    EXPECT_EQ(parsed.accountId, 1001u);
    EXPECT_EQ(parsed.roomId, 42u);
    EXPECT_EQ(parsed.startedAtUnixMs, 1710000000000u);
    EXPECT_EQ(parsed.finishedAtUnixMs, 1710000005000u);
    EXPECT_EQ(parsed.goldDelta, -25);
    EXPECT_EQ(parsed.reason, Net::TcpSettlementReason::kNormal);
    ASSERT_EQ(parsed.inventoryDeltas.size(), 2u);
    EXPECT_EQ(parsed.inventoryDeltas[0].itemId, 3001u);
    EXPECT_EQ(parsed.inventoryDeltas[0].quantityDelta, 2);
    EXPECT_EQ(parsed.inventoryDeltas[0].sourceDropId, 77u);
    EXPECT_EQ(parsed.inventoryDeltas[1].itemId, 3002u);
    EXPECT_EQ(parsed.inventoryDeltas[1].quantityDelta, -1);
    EXPECT_EQ(parsed.inventoryDeltas[1].sourceDropId, 78u);
}

TEST(TcpPacketTests, RejectSettlementResultWithInvalidSettlementIdLength) {
    Net::TcpSettlementResult settlement;
    settlement.settlementId = std::string(Net::kSettlementIdMaxLength + 1, 'a');

    std::vector<uint8_t> packet;
    EXPECT_FALSE(Net::serializeSettlementResultPacket(settlement, packet));
}

TEST(TcpPacketTests, RejectSettlementResultWithInvalidDeltaCountSize) {
    Net::TcpSettlementResult settlement;
    settlement.settlementId = "room-42-session-1001-finish-1";
    settlement.sessionId = 1001;
    settlement.accountId = 1001;
    settlement.roomId = 42;
    settlement.startedAtUnixMs = 1710000000000;
    settlement.finishedAtUnixMs = 1710000005000;
    settlement.inventoryDeltas = {
        Net::TcpSettlementInventoryDelta{3001, 2, 77},
    };

    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeSettlementResultPacket(settlement, packet));

    const size_t countOffset = Net::kTcpHeaderSize + Net::kSettlementIdLengthFieldSize +
        settlement.settlementId.size() + (2 * Net::kSessionIdFieldSize) + Net::kRoomIdFieldSize +
        (2 * Net::kTimestampFieldSize) + Net::kGoldDeltaFieldSize + Net::kSettlementReasonFieldSize;
    packet[countOffset] = 0x00;
    packet[countOffset + 1] = 0x02;

    Net::TcpPacketHeader header;
    Net::TcpSettlementResult parsed;
    EXPECT_FALSE(Net::parseSettlementResultPacket(packet.data(), packet.size(), header, parsed));
}

TEST(TcpPacketTests, RejectSettlementResultThatExceedsMaxPacketSize) {
    Net::TcpSettlementResult settlement;
    settlement.settlementId = "settlement";
    settlement.inventoryDeltas.assign(
        81,
        Net::TcpSettlementInventoryDelta{3001, 1, 77});

    std::vector<uint8_t> packet;
    EXPECT_FALSE(Net::serializeSettlementResultPacket(settlement, packet));
}

TEST(TcpPacketTests, SerializeAndParseMetaResponsePacket) {
    Net::TcpMetaResponse response;
    response.op = Net::TcpMetaResponseOp::kSettlementRetryLater;
    response.settlementId = "room-42-session-1001-finish-1";
    response.status = Net::TcpMetaResponseStatus::kRetryLater;
    response.retryAfterMs = 1500;

    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeMetaResponsePacket(response, packet));

    Net::TcpPacketHeader header;
    Net::TcpMetaResponse parsed;
    ASSERT_TRUE(Net::parseMetaResponsePacket(packet.data(), packet.size(), header, parsed));

    EXPECT_EQ(header.type, Net::TcpPacketType::kMetaResponse);
    EXPECT_EQ(header.size, Net::metaResponsePacketSize(response.settlementId.size()));
    EXPECT_EQ(parsed.op, Net::TcpMetaResponseOp::kSettlementRetryLater);
    EXPECT_EQ(parsed.settlementId, response.settlementId);
    EXPECT_EQ(parsed.status, Net::TcpMetaResponseStatus::kRetryLater);
    EXPECT_EQ(parsed.retryAfterMs, 1500u);
}

TEST(TcpPacketTests, RejectMetaResponseWithInvalidSettlementIdLength) {
    Net::TcpMetaResponse response;
    response.settlementId = std::string(Net::kSettlementIdMaxLength + 1, 'a');

    std::vector<uint8_t> packet;
    EXPECT_FALSE(Net::serializeMetaResponsePacket(response, packet));
}
