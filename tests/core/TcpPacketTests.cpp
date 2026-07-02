#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "Net/TcpPacket.hpp"

namespace {
void writeTestU16BE(uint16_t value, std::vector<uint8_t>& packet, size_t offset) {
    packet[offset] = static_cast<uint8_t>((value >> 8) & 0xFF);
    packet[offset + 1] = static_cast<uint8_t>(value & 0xFF);
}

void writeTestU32BE(uint32_t value, std::vector<uint8_t>& packet, size_t offset) {
    packet[offset] = static_cast<uint8_t>((value >> 24) & 0xFF);
    packet[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    packet[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    packet[offset + 3] = static_cast<uint8_t>(value & 0xFF);
}

void writeTestU64BE(uint64_t value, std::vector<uint8_t>& packet, size_t offset) {
    for (size_t index = 0; index < 8; ++index) {
        packet[offset + index] =
            static_cast<uint8_t>((value >> ((7 - index) * 8)) & 0xFF);
    }
}

void writeTestI64BE(int64_t value, std::vector<uint8_t>& packet, size_t offset) {
    writeTestU64BE(static_cast<uint64_t>(value), packet, offset);
}

std::vector<uint8_t> battleLoadEntryWirePacket(
    uint32_t roomId,
    uint64_t battleInstanceId,
    const std::vector<uint64_t>& playerSessionIds) {
    std::vector<uint8_t> packet(Net::battleLoadEntryPacketSize(playerSessionIds.size()), 0);
    writeTestU16BE(static_cast<uint16_t>(packet.size()), packet, 0);
    writeTestU16BE(static_cast<uint16_t>(Net::TcpPacketType::kBattleLoadEntry), packet, 2);
    writeTestU32BE(roomId, packet, Net::kTcpHeaderSize);
    writeTestU64BE(
        battleInstanceId,
        packet,
        Net::kTcpHeaderSize + Net::kRoomIdFieldSize);
    writeTestU16BE(
        static_cast<uint16_t>(playerSessionIds.size()),
        packet,
        Net::kTcpHeaderSize + Net::kRoomIdFieldSize + Net::kBattleInstanceIdFieldSize);

    size_t offset = Net::kTcpHeaderSize + Net::kRoomIdFieldSize +
        Net::kBattleInstanceIdFieldSize + Net::kBattleLoadEntryCountFieldSize;
    for (uint64_t sessionId : playerSessionIds) {
        writeTestU64BE(sessionId, packet, offset);
        offset += Net::kBattleLoadEntryEntrySize;
    }
    return packet;
}

std::vector<uint8_t> battleFinalRankingWirePacket(
    uint32_t roomId,
    uint64_t battleInstanceId,
    const std::vector<Net::BattleFinalRankingEntry>& rows) {
    std::vector<uint8_t> packet(Net::battleFinalRankingPacketSize(rows), 0);
    writeTestU16BE(static_cast<uint16_t>(packet.size()), packet, 0);
    writeTestU16BE(static_cast<uint16_t>(Net::TcpPacketType::kBattleFinalRanking), packet, 2);
    writeTestU32BE(roomId, packet, Net::kTcpHeaderSize);
    writeTestU64BE(
        battleInstanceId,
        packet,
        Net::kTcpHeaderSize + Net::kRoomIdFieldSize);
    packet[Net::kTcpHeaderSize + Net::kRoomIdFieldSize + Net::kBattleInstanceIdFieldSize] =
        static_cast<uint8_t>(rows.size());

    size_t offset = Net::kTcpHeaderSize + Net::kRoomIdFieldSize +
        Net::kBattleInstanceIdFieldSize + Net::kBattleFinalRankingCountFieldSize;
    for (const Net::BattleFinalRankingEntry& row : rows) {
        writeTestU16BE(row.rank, packet, offset);
        offset += Net::kBattleFinalRankingRankFieldSize;
        writeTestU64BE(row.sessionId, packet, offset);
        offset += Net::kSessionIdFieldSize;
        packet[offset++] = static_cast<uint8_t>(row.nickname.size());
        std::copy(row.nickname.begin(), row.nickname.end(), packet.begin() + offset);
        offset += row.nickname.size();
        writeTestI64BE(row.totalAssetValue, packet, offset);
        offset += Net::kFinalAssetValueFieldSize;
    }

    return packet;
}
} // namespace

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

TEST(TcpPacketTests, AuthenticateGameSessionRoundTripsToken) {
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeAuthenticateGameSessionPacket("token-123", packet));

    Net::TcpPacketHeader header;
    std::string token;
    ASSERT_TRUE(Net::parseAuthenticateGameSessionPacket(
        packet.data(),
        packet.size(),
        header,
        token));

    EXPECT_EQ(header.type, Net::TcpPacketType::kAuthenticateGameSession);
    EXPECT_EQ(token, "token-123");
}

TEST(TcpPacketTests, AuthenticateGameSessionRejectsEmptyAndOversizedToken) {
    std::vector<uint8_t> packet;
    EXPECT_FALSE(Net::serializeAuthenticateGameSessionPacket("", packet));
    EXPECT_FALSE(Net::serializeAuthenticateGameSessionPacket(std::string(513, 'a'), packet));
}

TEST(TcpPacketTests, AuthenticateGameSessionRejectsMalformedTokenLength) {
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeAuthenticateGameSessionPacket("token-123", packet));

    packet[Net::kTcpHeaderSize] = 0x00;
    packet[Net::kTcpHeaderSize + 1] = 0x08;

    Net::TcpPacketHeader header;
    std::string token;
    EXPECT_FALSE(Net::parseAuthenticateGameSessionPacket(
        packet.data(),
        packet.size(),
        header,
        token));
}

TEST(TcpPacketTests, SessionReplacedIsHeaderOnly) {
    std::array<uint8_t, Net::kTcpHeaderSize> packet{};
    ASSERT_TRUE(Net::serializeSessionReplacedPacket(packet));

    Net::TcpPacketHeader header;
    ASSERT_TRUE(Net::parseTcpPacketHeader(packet.data(), packet.size(), header));
    EXPECT_EQ(header.size, Net::kTcpHeaderSize);
    EXPECT_EQ(header.type, Net::TcpPacketType::kSessionReplaced);
}

TEST(TcpPacketTests, HeartbeatRequestIsHeaderOnly) {
    std::array<uint8_t, Net::kTcpHeaderSize> packet{};
    ASSERT_TRUE(Net::serializeHeartbeatRequestPacket(packet));

    Net::TcpPacketHeader header;
    ASSERT_TRUE(Net::parseHeartbeatRequestPacket(packet.data(), packet.size(), header));
    EXPECT_EQ(header.size, Net::kTcpHeaderSize);
    EXPECT_EQ(header.type, Net::TcpPacketType::kHeartbeatRequest);

    std::array<uint8_t, Net::kTcpHeaderSize + 1> oversized{};
    std::copy(packet.begin(), packet.end(), oversized.begin());
    oversized[0] = 0x00;
    oversized[1] = static_cast<uint8_t>(oversized.size());
    EXPECT_FALSE(Net::parseHeartbeatRequestPacket(
        oversized.data(),
        oversized.size(),
        header));
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

TEST(TcpPacketTests, SerializeAndParseCreateRoomRequestWithTitleAndCapacity) {
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeCreateRoomRequestPacket("abc", 10, packet));

    Net::TcpPacketHeader header;
    std::string roomTitle;
    uint8_t maxPlayers = 0;
    ASSERT_TRUE(Net::parseCreateRoomRequestPacket(
        packet.data(),
        packet.size(),
        header,
        roomTitle,
        maxPlayers));

    EXPECT_EQ(header.type, Net::TcpPacketType::kCreateRoomRequest);
    EXPECT_EQ(header.size, Net::kTcpHeaderSize + 1 + 3 + 1);
    EXPECT_EQ(roomTitle, "abc");
    EXPECT_EQ(maxPlayers, 10);
    EXPECT_EQ(packet[Net::kTcpHeaderSize], 3);
    EXPECT_EQ(packet[Net::kTcpHeaderSize + 4], 10);
}

TEST(TcpPacketTests, CreateRoomRequestRejectsInvalidCapacityAndTitle) {
    std::vector<uint8_t> packet;

    EXPECT_FALSE(Net::serializeCreateRoomRequestPacket("abc", 1, packet));
    EXPECT_FALSE(Net::serializeCreateRoomRequestPacket("abc", 11, packet));
    EXPECT_FALSE(Net::serializeCreateRoomRequestPacket("", 10, packet));
    EXPECT_FALSE(Net::serializeCreateRoomRequestPacket("bad\nroom", 10, packet));
    EXPECT_FALSE(Net::serializeCreateRoomRequestPacket(std::string(65, 'a'), 10, packet));
}

TEST(TcpPacketTests, CreateRoomRequestRejectsMalformedPayload) {
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeCreateRoomRequestPacket("abc", 10, packet));

    Net::TcpPacketHeader header;
    std::string roomTitle;
    uint8_t maxPlayers = 0;

    std::vector<uint8_t> lengthMismatch = packet;
    lengthMismatch[Net::kTcpHeaderSize] = 4;
    EXPECT_FALSE(Net::parseCreateRoomRequestPacket(
        lengthMismatch.data(),
        lengthMismatch.size(),
        header,
        roomTitle,
        maxPlayers));

    std::vector<uint8_t> invalidCapacity = packet;
    invalidCapacity.back() = 1;
    EXPECT_FALSE(Net::parseCreateRoomRequestPacket(
        invalidCapacity.data(),
        invalidCapacity.size(),
        header,
        roomTitle,
        maxPlayers));

    std::vector<uint8_t> emptyTitle = {
        0x00,
        0x06,
        0x01,
        0x01,
        0x00,
        0x0A,
    };
    EXPECT_FALSE(Net::parseCreateRoomRequestPacket(
        emptyTitle.data(),
        emptyTitle.size(),
        header,
        roomTitle,
        maxPlayers));
}

TEST(TcpPacketTests, HeaderOnlyCreateRoomRequestIsInvalidInRelease0) {
    std::array<uint8_t, Net::kTcpHeaderSize> packet{
        0x00,
        0x04,
        0x01,
        0x01,
    };

    Net::TcpPacketHeader header;
    std::string roomTitle;
    uint8_t maxPlayers = 0;
    EXPECT_FALSE(Net::parseCreateRoomRequestPacket(
        packet.data(),
        packet.size(),
        header,
        roomTitle,
        maxPlayers));
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
            Net::TcpRoomEntry{1, 1, 2, Net::TcpRoomStatus::kOpen, "친구랑 한 판"},
            Net::TcpRoomEntry{7, 2, 2, Net::TcpRoomStatus::kOpen, "Boss Rush"},
        },
        packet));

    Net::TcpPacketHeader header;
    std::vector<Net::TcpRoomEntry> rooms;
    ASSERT_TRUE(Net::parseRoomListSnapshotPacket(packet.data(), packet.size(), header, rooms));

    EXPECT_EQ(header.type, Net::TcpPacketType::kRoomListSnapshot);
    EXPECT_EQ(header.size, packet.size());
    ASSERT_EQ(rooms.size(), 2u);
    EXPECT_EQ(rooms[0].roomId, 1u);
    EXPECT_EQ(rooms[0].playerCount, 1u);
    EXPECT_EQ(rooms[0].maxPlayers, 2u);
    EXPECT_EQ(rooms[0].title, "친구랑 한 판");
    EXPECT_EQ(rooms[1].roomId, 7u);
    EXPECT_EQ(rooms[1].playerCount, 2u);
    EXPECT_EQ(rooms[1].maxPlayers, 2u);
    EXPECT_EQ(rooms[1].title, "Boss Rush");
}

TEST(TcpPacketTests, RejectRoomListSnapshotWithInvalidCountSize) {
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeRoomListSnapshotPacket(
        {
            Net::TcpRoomEntry{1, 1, 2, Net::TcpRoomStatus::kOpen, "One"},
            Net::TcpRoomEntry{2, 2, 2, Net::TcpRoomStatus::kOpen, "Two"},
        },
        packet));

    packet[4] = 0x00;
    packet[5] = 0x03;

    Net::TcpPacketHeader header;
    std::vector<Net::TcpRoomEntry> rooms;
    EXPECT_FALSE(Net::parseRoomListSnapshotPacket(packet.data(), packet.size(), header, rooms));
}

TEST(TcpPacketTests, RejectRoomListSnapshotThatExceedsMaxPacketSize) {
    std::vector<Net::TcpRoomEntry> rooms(
        128,
        Net::TcpRoomEntry{1, 1, 2, Net::TcpRoomStatus::kOpen, "Too Many Rooms"});
    std::vector<uint8_t> packet;
    EXPECT_FALSE(Net::serializeRoomListSnapshotPacket(rooms, packet));
}

TEST(TcpPacketTests, SerializeAndParseRoomListSnapshotWithStatus) {
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeRoomListSnapshotPacket(
        {
            Net::TcpRoomEntry{1, 1, 10, Net::TcpRoomStatus::kOpen, "Open A"},
            Net::TcpRoomEntry{2, 10, 10, Net::TcpRoomStatus::kOpen, "Open B"},
            Net::TcpRoomEntry{3, 2, 10, Net::TcpRoomStatus::kInProgress, "Started"},
        },
        packet));

    Net::TcpPacketHeader header;
    std::vector<Net::TcpRoomEntry> rooms;
    ASSERT_TRUE(Net::parseRoomListSnapshotPacket(packet.data(), packet.size(), header, rooms));

    EXPECT_EQ(header.type, Net::TcpPacketType::kRoomListSnapshot);
    EXPECT_EQ(header.size, packet.size());
    ASSERT_EQ(rooms.size(), 3u);
    EXPECT_EQ(rooms[0].roomStatus, Net::TcpRoomStatus::kOpen);
    EXPECT_EQ(rooms[1].roomStatus, Net::TcpRoomStatus::kOpen);
    EXPECT_EQ(rooms[2].roomStatus, Net::TcpRoomStatus::kInProgress);
    EXPECT_EQ(rooms[2].title, "Started");
}

TEST(TcpPacketTests, RejectRoomListSnapshotWithInvalidTitleLength) {
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeRoomListSnapshotPacket(
        {Net::TcpRoomEntry{1, 1, 10, Net::TcpRoomStatus::kOpen, "Short"}},
        packet));
    packet[Net::kTcpHeaderSize + Net::kRoomListCountFieldSize + Net::kRoomEntrySize] = 0xFF;

    Net::TcpPacketHeader header;
    std::vector<Net::TcpRoomEntry> rooms;
    EXPECT_FALSE(Net::parseRoomListSnapshotPacket(packet.data(), packet.size(), header, rooms));
}

TEST(TcpPacketTests, RoomListSnapshotRejectsUnknownStatus) {
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeRoomListSnapshotPacket(
        {Net::TcpRoomEntry{1, 1, 10, Net::TcpRoomStatus::kOpen, "Open"}},
        packet));
    packet[Net::kTcpHeaderSize + Net::kRoomListCountFieldSize + 8] = 0x07;

    Net::TcpPacketHeader header;
    std::vector<Net::TcpRoomEntry> rooms;
    EXPECT_FALSE(Net::parseRoomListSnapshotPacket(packet.data(), packet.size(), header, rooms));
}

TEST(TcpPacketTests, SerializeAndParseRoomActionPackets) {
    std::array<uint8_t, Net::kTcpHeaderSize> unreadyRequest{};
    ASSERT_TRUE(Net::serializeUnreadyRoomRequestPacket(unreadyRequest));
    EXPECT_EQ(unreadyRequest[2], 0x01);
    EXPECT_EQ(unreadyRequest[3], 0x1E);

    std::array<uint8_t, Net::kRoomIdPacketSize> hostStartResponse{};
    ASSERT_TRUE(Net::serializeHostStartBattleResponsePacket(42, hostStartResponse));
    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    ASSERT_TRUE(Net::parseHostStartBattleResponsePacket(
        hostStartResponse.data(),
        hostStartResponse.size(),
        header,
        roomId));
    EXPECT_EQ(header.type, Net::TcpPacketType::kHostStartBattleResponse);
    EXPECT_EQ(roomId, 42u);

    std::array<uint8_t, Net::kHostKickRequestPacketSize> kickRequest{};
    ASSERT_TRUE(Net::serializeHostKickRequestPacket(77, kickRequest));
    uint32_t targetSessionId = 0;
    ASSERT_TRUE(Net::parseHostKickRequestPacket(
        kickRequest.data(),
        kickRequest.size(),
        header,
        targetSessionId));
    EXPECT_EQ(targetSessionId, 77u);
}

TEST(TcpPacketTests, SerializeAndParseLobbyReturnVisibility) {
    std::array<uint8_t, Net::kLobbyReturnVisibilityPacketSize> packet{};
    ASSERT_TRUE(Net::serializeLobbyReturnVisibilityPacket(
        42,
        Net::TcpLobbyReturnReason::kHostKick,
        packet));

    Net::TcpPacketHeader header;
    uint32_t previousRoomId = 0;
    Net::TcpLobbyReturnReason reason = Net::TcpLobbyReturnReason::kNone;
    ASSERT_TRUE(Net::parseLobbyReturnVisibilityPacket(
        packet.data(),
        packet.size(),
        header,
        previousRoomId,
        reason));
    EXPECT_EQ(header.type, Net::TcpPacketType::kLobbyReturnVisibility);
    EXPECT_EQ(previousRoomId, 42u);
    EXPECT_EQ(reason, Net::TcpLobbyReturnReason::kHostKick);
}

TEST(TcpPacketTests, LobbyReturnVisibilitySupportsResultGenerationFailureReason) {
    std::array<uint8_t, Net::kLobbyReturnVisibilityPacketSize> packet{};
    ASSERT_TRUE(Net::serializeLobbyReturnVisibilityPacket(
        42,
        Net::TcpLobbyReturnReason::kResultGenerationFailure,
        packet));

    Net::TcpPacketHeader header;
    uint32_t previousRoomId = 0;
    Net::TcpLobbyReturnReason reason = Net::TcpLobbyReturnReason::kNone;
    ASSERT_TRUE(Net::parseLobbyReturnVisibilityPacket(
        packet.data(),
        packet.size(),
        header,
        previousRoomId,
        reason));

    EXPECT_EQ(previousRoomId, 42u);
    EXPECT_EQ(reason, Net::TcpLobbyReturnReason::kResultGenerationFailure);
}

TEST(TcpPacketTests, BattleFinalRankingRoundTripsServerOrderedRows) {
    std::vector<uint8_t> packet;
    const std::vector<Net::BattleFinalRankingEntry> rows{
        Net::BattleFinalRankingEntry{1, 10, "PlayerA", 500},
        Net::BattleFinalRankingEntry{1, 11, "PlayerB", 500},
        Net::BattleFinalRankingEntry{3, 12, "PlayerC", 100},
    };
    ASSERT_TRUE(Net::serializeBattleFinalRankingPacket(42, 9001, rows, packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    uint64_t battleInstanceId = 0;
    std::vector<Net::BattleFinalRankingEntry> parsed;
    ASSERT_TRUE(Net::parseBattleFinalRankingPacket(
        packet.data(),
        packet.size(),
        header,
        roomId,
        battleInstanceId,
        parsed));

    EXPECT_EQ(header.type, Net::TcpPacketType::kBattleFinalRanking);
    EXPECT_EQ(roomId, 42u);
    EXPECT_EQ(battleInstanceId, 9001u);
    ASSERT_EQ(parsed.size(), 3u);
    EXPECT_EQ(parsed[0].rank, 1u);
    EXPECT_EQ(parsed[0].sessionId, 10u);
    EXPECT_EQ(parsed[0].nickname, "PlayerA");
    EXPECT_EQ(parsed[0].totalAssetValue, 500);
    EXPECT_EQ(parsed[1].rank, 1u);
    EXPECT_EQ(parsed[2].rank, 3u);
}

TEST(TcpPacketTests, BattleFinalRankingRejectsNegativeTotalAssetValue) {
    std::vector<uint8_t> packet;
    EXPECT_FALSE(Net::serializeBattleFinalRankingPacket(
        42,
        9001,
        {Net::BattleFinalRankingEntry{1, 10, "PlayerA", -1}},
        packet));

    const std::vector<Net::BattleFinalRankingEntry> rows{
        Net::BattleFinalRankingEntry{1, 10, "PlayerA", -1},
    };
    packet = battleFinalRankingWirePacket(42, 9001, rows);

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    uint64_t battleInstanceId = 0;
    std::vector<Net::BattleFinalRankingEntry> parsed;
    EXPECT_FALSE(Net::parseBattleFinalRankingPacket(
        packet.data(),
        packet.size(),
        header,
        roomId,
        battleInstanceId,
        parsed));
}

TEST(TcpPacketTests, BattleFinalRankingRejectsMalformedRows) {
    const std::vector<Net::BattleFinalRankingEntry> rows{
        Net::BattleFinalRankingEntry{1, 10, "PlayerA", 500},
    };
    const std::vector<uint8_t> valid = battleFinalRankingWirePacket(42, 9001, rows);

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    uint64_t battleInstanceId = 0;
    std::vector<Net::BattleFinalRankingEntry> parsed;

    std::vector<uint8_t> zeroSession = valid;
    writeTestU64BE(0, zeroSession, Net::kTcpHeaderSize + Net::kRoomIdFieldSize +
        Net::kBattleInstanceIdFieldSize + Net::kBattleFinalRankingCountFieldSize +
        Net::kBattleFinalRankingRankFieldSize);
    EXPECT_FALSE(Net::parseBattleFinalRankingPacket(
        zeroSession.data(),
        zeroSession.size(),
        header,
        roomId,
        battleInstanceId,
        parsed));

    std::vector<uint8_t> zeroRank = valid;
    writeTestU16BE(0, zeroRank, Net::kTcpHeaderSize + Net::kRoomIdFieldSize +
        Net::kBattleInstanceIdFieldSize + Net::kBattleFinalRankingCountFieldSize);
    EXPECT_FALSE(Net::parseBattleFinalRankingPacket(
        zeroRank.data(),
        zeroRank.size(),
        header,
        roomId,
        battleInstanceId,
        parsed));

    std::vector<uint8_t> invalidCount = valid;
    invalidCount[Net::kTcpHeaderSize + Net::kRoomIdFieldSize + Net::kBattleInstanceIdFieldSize] = 0;
    EXPECT_FALSE(Net::parseBattleFinalRankingPacket(
        invalidCount.data(),
        invalidCount.size(),
        header,
        roomId,
        battleInstanceId,
        parsed));

    std::vector<uint8_t> duplicateSession;
    EXPECT_FALSE(Net::serializeBattleFinalRankingPacket(
        42,
        9001,
        {
            Net::BattleFinalRankingEntry{1, 10, "PlayerA", 500},
            Net::BattleFinalRankingEntry{2, 10, "PlayerB", 100},
        },
        duplicateSession));
    duplicateSession = battleFinalRankingWirePacket(
        42,
        9001,
        {
            Net::BattleFinalRankingEntry{1, 10, "PlayerA", 500},
            Net::BattleFinalRankingEntry{2, 10, "PlayerB", 100},
        });
    EXPECT_FALSE(Net::parseBattleFinalRankingPacket(
        duplicateSession.data(),
        duplicateSession.size(),
        header,
        roomId,
        battleInstanceId,
        parsed));
}

TEST(TcpPacketTests, SerializeAndParseRoomDetailState) {
    Net::TcpRoomDetailState detail;
    detail.roomId = 42;
    detail.roomStatus = Net::TcpRoomStatus::kOpen;
    detail.roomTitle = "Room1";
    detail.maxPlayers = 10;
    detail.members = {
        Net::TcpRoomMemberEntry{1001, "Player1", true},
        Net::TcpRoomMemberEntry{1002, "Player2", false},
    };
    detail.selfActionMask =
        Net::kTcpRoomActionLeaveRoom |
        Net::kTcpRoomActionUnready |
        Net::kTcpRoomActionHostStartBattle;
    detail.targetActions = {
        Net::TcpTargetActionEntry{1002, Net::kTcpTargetActionHostKick},
    };

    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeRoomDetailStatePacket(detail, packet));

    Net::TcpPacketHeader header;
    Net::TcpRoomDetailState parsed;
    ASSERT_TRUE(Net::parseRoomDetailStatePacket(packet.data(), packet.size(), header, parsed));
    EXPECT_EQ(header.type, Net::TcpPacketType::kRoomDetailState);
    EXPECT_EQ(parsed.roomId, 42u);
    EXPECT_EQ(parsed.roomStatus, Net::TcpRoomStatus::kOpen);
    EXPECT_EQ(parsed.roomTitle, "Room1");
    ASSERT_EQ(parsed.members.size(), 2u);
    EXPECT_EQ(parsed.members[0].sessionId, 1001u);
    EXPECT_EQ(parsed.members[0].nickname, "Player1");
    EXPECT_TRUE(parsed.members[0].ready);
    ASSERT_EQ(parsed.targetActions.size(), 1u);
    EXPECT_EQ(parsed.targetActions[0].targetActionMask, Net::kTcpTargetActionHostKick);
}

TEST(TcpPacketTests, RoomDetailStateRejectsInvalidCapacityAndMemberOverflow) {
    Net::TcpRoomDetailState detail;
    detail.roomId = 42;
    detail.roomStatus = Net::TcpRoomStatus::kOpen;
    detail.roomTitle = "Room1";
    detail.maxPlayers = 3;
    detail.members = {
        Net::TcpRoomMemberEntry{1001, "Player1", true},
        Net::TcpRoomMemberEntry{1002, "Player2", false},
        Net::TcpRoomMemberEntry{1003, "Player3", false},
    };
    detail.selfActionMask = Net::kTcpRoomActionLeaveRoom;

    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeRoomDetailStatePacket(detail, packet));

    const size_t maxPlayersOffset =
        Net::kTcpHeaderSize +
        Net::kRoomIdFieldSize +
        Net::kRoomStatusFieldSize +
        Net::kRoomTitleLengthFieldSize +
        detail.roomTitle.size();

    std::vector<uint8_t> lowCapacityPacket = packet;
    lowCapacityPacket[maxPlayersOffset] = 1;
    Net::TcpPacketHeader header;
    Net::TcpRoomDetailState parsed;
    EXPECT_FALSE(Net::parseRoomDetailStatePacket(
        lowCapacityPacket.data(),
        lowCapacityPacket.size(),
        header,
        parsed));

    std::vector<uint8_t> highCapacityPacket = packet;
    highCapacityPacket[maxPlayersOffset] = 11;
    EXPECT_FALSE(Net::parseRoomDetailStatePacket(
        highCapacityPacket.data(),
        highCapacityPacket.size(),
        header,
        parsed));

    std::vector<uint8_t> memberOverflowPacket = packet;
    memberOverflowPacket[maxPlayersOffset] = 2;
    EXPECT_FALSE(Net::parseRoomDetailStatePacket(
        memberOverflowPacket.data(),
        memberOverflowPacket.size(),
        header,
        parsed));

    detail.maxPlayers = 1;
    EXPECT_FALSE(Net::serializeRoomDetailStatePacket(detail, packet));
    detail.maxPlayers = 11;
    EXPECT_FALSE(Net::serializeRoomDetailStatePacket(detail, packet));
    detail.maxPlayers = 2;
    EXPECT_FALSE(Net::serializeRoomDetailStatePacket(detail, packet));
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

TEST(TcpPacketTests, SerializeAndParseBattleStartRosterPacket) {
    const std::vector<uint64_t> players{1001, 1002, 1003};

    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeBattleStartRosterPacket(42, players, packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    std::vector<uint64_t> parsedPlayers;
    ASSERT_TRUE(Net::parseBattleStartRosterPacket(
        packet.data(),
        packet.size(),
        header,
        roomId,
        parsedPlayers));

    EXPECT_EQ(header.type, Net::TcpPacketType::kBattleStartRoster);
    EXPECT_EQ(header.size, Net::battleStartRosterPacketSize(players.size()));
    EXPECT_EQ(roomId, 42u);
    EXPECT_EQ(parsedPlayers, players);
}

TEST(TcpPacketTests, BattleStartRosterRejectsInvalidPlayerCounts) {
    std::vector<uint8_t> packet;
    EXPECT_FALSE(Net::serializeBattleStartRosterPacket(42, {1001}, packet));
    EXPECT_TRUE(packet.empty());

    const std::vector<uint64_t> tooMany{
        1001, 1002, 1003, 1004, 1005,
        1006, 1007, 1008, 1009, 1010, 1011};
    EXPECT_FALSE(Net::serializeBattleStartRosterPacket(42, tooMany, packet));
    EXPECT_TRUE(packet.empty());
}

TEST(TcpPacketTests, BattleStartRosterRejectsZeroOrDuplicateSessionIds) {
    std::vector<uint8_t> packet;
    EXPECT_FALSE(Net::serializeBattleStartRosterPacket(42, {1001, 0, 1003}, packet));
    EXPECT_TRUE(packet.empty());

    EXPECT_FALSE(Net::serializeBattleStartRosterPacket(42, {1001, 1002, 1001}, packet));
    EXPECT_TRUE(packet.empty());
}

TEST(TcpPacketTests, ParseBattleStartRosterRejectsMalformedPackets) {
    const std::vector<uint64_t> players{1001, 1002, 1003};
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeBattleStartRosterPacket(42, players, packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 99;
    std::vector<uint64_t> parsedPlayers{77};

    std::vector<uint8_t> tooShort = packet;
    tooShort.pop_back();
    EXPECT_FALSE(Net::parseBattleStartRosterPacket(
        tooShort.data(),
        tooShort.size(),
        header,
        roomId,
        parsedPlayers));
    EXPECT_EQ(roomId, 0u);
    EXPECT_TRUE(parsedPlayers.empty());

    std::vector<uint8_t> tooLong = packet;
    tooLong.push_back(0x00);
    EXPECT_FALSE(Net::parseBattleStartRosterPacket(
        tooLong.data(),
        tooLong.size(),
        header,
        roomId,
        parsedPlayers));
    EXPECT_EQ(roomId, 0u);
    EXPECT_TRUE(parsedPlayers.empty());
}

TEST(TcpPacketTests, SerializeAndParseBattleLoadEntryPacket) {
    const std::vector<uint64_t> players{1001, 1002, 1003};

    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeBattleLoadEntryPacket(42, 9001, players, packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    uint64_t battleInstanceId = 0;
    std::vector<uint64_t> parsedPlayers;
    ASSERT_TRUE(Net::parseBattleLoadEntryPacket(
        packet.data(),
        packet.size(),
        header,
        roomId,
        battleInstanceId,
        parsedPlayers));

    EXPECT_EQ(header.type, Net::TcpPacketType::kBattleLoadEntry);
    EXPECT_EQ(header.size, Net::battleLoadEntryPacketSize(players.size()));
    EXPECT_EQ(roomId, 42u);
    EXPECT_EQ(battleInstanceId, 9001u);
    EXPECT_EQ(parsedPlayers, players);
}

TEST(TcpPacketTests, BattleLoadEntryRejectsInvalidFields) {
    std::vector<uint8_t> packet;
    EXPECT_FALSE(Net::serializeBattleLoadEntryPacket(0, 9001, {1001, 1002}, packet));
    EXPECT_TRUE(packet.empty());

    EXPECT_FALSE(Net::serializeBattleLoadEntryPacket(42, 0, {1001, 1002}, packet));
    EXPECT_TRUE(packet.empty());

    EXPECT_FALSE(Net::serializeBattleLoadEntryPacket(42, 9001, {1001}, packet));
    EXPECT_TRUE(packet.empty());

    const std::vector<uint64_t> tooMany{
        1001, 1002, 1003, 1004, 1005,
        1006, 1007, 1008, 1009, 1010, 1011};
    EXPECT_FALSE(Net::serializeBattleLoadEntryPacket(42, 9001, tooMany, packet));
    EXPECT_TRUE(packet.empty());

    EXPECT_FALSE(Net::serializeBattleLoadEntryPacket(42, 9001, {1001, 0}, packet));
    EXPECT_TRUE(packet.empty());

    EXPECT_FALSE(Net::serializeBattleLoadEntryPacket(42, 9001, {1001, 1001}, packet));
    EXPECT_TRUE(packet.empty());
}

TEST(TcpPacketTests, ParseBattleLoadEntryRejectsMalformedPackets) {
    const std::vector<uint64_t> players{1001, 1002, 1003};
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeBattleLoadEntryPacket(42, 9001, players, packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 99;
    uint64_t battleInstanceId = 77;
    std::vector<uint64_t> parsedPlayers{88};

    std::vector<uint8_t> tooShort = packet;
    tooShort.pop_back();
    EXPECT_FALSE(Net::parseBattleLoadEntryPacket(
        tooShort.data(),
        tooShort.size(),
        header,
        roomId,
        battleInstanceId,
        parsedPlayers));
    EXPECT_EQ(roomId, 0u);
    EXPECT_EQ(battleInstanceId, 0u);
    EXPECT_TRUE(parsedPlayers.empty());

    std::vector<uint8_t> wrongType = packet;
    wrongType[3] = 0x18;
    EXPECT_FALSE(Net::parseBattleLoadEntryPacket(
        wrongType.data(),
        wrongType.size(),
        header,
        roomId,
        battleInstanceId,
        parsedPlayers));
    EXPECT_EQ(roomId, 0u);
    EXPECT_EQ(battleInstanceId, 0u);
    EXPECT_TRUE(parsedPlayers.empty());
}

TEST(TcpPacketTests, ParseBattleLoadEntryRejectsMalformedWireFields) {
    auto assertRejected = [](const std::vector<uint8_t>& packet) {
        Net::TcpPacketHeader header;
        uint32_t roomId = 99;
        uint64_t battleInstanceId = 77;
        std::vector<uint64_t> parsedPlayers{88};

        EXPECT_FALSE(Net::parseBattleLoadEntryPacket(
            packet.data(),
            packet.size(),
            header,
            roomId,
            battleInstanceId,
            parsedPlayers));
        EXPECT_EQ(roomId, 0u);
        EXPECT_EQ(battleInstanceId, 0u);
        EXPECT_TRUE(parsedPlayers.empty());
    };

    assertRejected(battleLoadEntryWirePacket(0, 9001, {1001, 1002}));
    assertRejected(battleLoadEntryWirePacket(42, 0, {1001, 1002}));
    assertRejected(battleLoadEntryWirePacket(42, 9001, {1001}));

    const std::vector<uint64_t> tooMany{
        1001, 1002, 1003, 1004, 1005,
        1006, 1007, 1008, 1009, 1010, 1011};
    assertRejected(battleLoadEntryWirePacket(42, 9001, tooMany));

    assertRejected(battleLoadEntryWirePacket(42, 9001, {1001, 0}));
    assertRejected(battleLoadEntryWirePacket(42, 9001, {1001, 1001}));

    std::vector<uint8_t> extraByte = battleLoadEntryWirePacket(42, 9001, {1001, 1002});
    extraByte.push_back(0x00);
    assertRejected(extraByte);
}

TEST(TcpPacketTests, SerializeAndParseArenaLoadCompletePacket) {
    std::array<uint8_t, Net::kArenaLoadCompletePacketSize> packet{};
    ASSERT_TRUE(Net::serializeArenaLoadCompletePacket(42, 9001, packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    uint64_t battleInstanceId = 0;
    ASSERT_TRUE(Net::parseArenaLoadCompletePacket(
        packet.data(),
        packet.size(),
        header,
        roomId,
        battleInstanceId));

    EXPECT_EQ(header.type, Net::TcpPacketType::kArenaLoadComplete);
    EXPECT_EQ(header.size, Net::kArenaLoadCompletePacketSize);
    EXPECT_EQ(roomId, 42u);
    EXPECT_EQ(battleInstanceId, 9001u);
}

TEST(TcpPacketTests, ArenaLoadCompleteRejectsInvalidFields) {
    std::array<uint8_t, Net::kArenaLoadCompletePacketSize> packet{};
    EXPECT_FALSE(Net::serializeArenaLoadCompletePacket(0, 9001, packet));
    EXPECT_FALSE(Net::serializeArenaLoadCompletePacket(42, 0, packet));

    ASSERT_TRUE(Net::serializeArenaLoadCompletePacket(42, 9001, packet));
    Net::TcpPacketHeader header;
    uint32_t roomId = 99;
    uint64_t battleInstanceId = 77;

    std::array<uint8_t, Net::kArenaLoadCompletePacketSize> wrongType = packet;
    wrongType[3] = static_cast<uint8_t>(Net::TcpPacketType::kBattleLoadEntry);
    EXPECT_FALSE(Net::parseArenaLoadCompletePacket(
        wrongType.data(),
        wrongType.size(),
        header,
        roomId,
        battleInstanceId));
    EXPECT_EQ(roomId, 0u);
    EXPECT_EQ(battleInstanceId, 0u);
}

TEST(TcpPacketTests, SerializeAndParseArenaGameplayStartPacket) {
    std::array<uint8_t, Net::kArenaGameplayStartPacketSize> packet{};
    ASSERT_TRUE(Net::serializeArenaGameplayStartPacket(42, 9001, packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    uint64_t battleInstanceId = 0;
    ASSERT_TRUE(Net::parseArenaGameplayStartPacket(
        packet.data(),
        packet.size(),
        header,
        roomId,
        battleInstanceId));

    EXPECT_EQ(header.type, Net::TcpPacketType::kArenaGameplayStart);
    EXPECT_EQ(header.size, Net::kArenaGameplayStartPacketSize);
    EXPECT_EQ(roomId, 42u);
    EXPECT_EQ(battleInstanceId, 9001u);
}

TEST(TcpPacketTests, ArenaGameplayStartRejectsInvalidFields) {
    std::array<uint8_t, Net::kArenaGameplayStartPacketSize> packet{};
    EXPECT_FALSE(Net::serializeArenaGameplayStartPacket(0, 9001, packet));
    EXPECT_FALSE(Net::serializeArenaGameplayStartPacket(42, 0, packet));

    ASSERT_TRUE(Net::serializeArenaGameplayStartPacket(42, 9001, packet));
    Net::TcpPacketHeader header;
    uint32_t roomId = 99;
    uint64_t battleInstanceId = 77;

    std::array<uint8_t, Net::kArenaGameplayStartPacketSize> wrongType = packet;
    wrongType[3] = static_cast<uint8_t>(Net::TcpPacketType::kBattleLoadEntry);
    EXPECT_FALSE(Net::parseArenaGameplayStartPacket(
        wrongType.data(),
        wrongType.size(),
        header,
        roomId,
        battleInstanceId));
    EXPECT_EQ(roomId, 0u);
    EXPECT_EQ(battleInstanceId, 0u);
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

TEST(TcpPacketTests, SerializeAndParseMonsterHealthSnapshotPacket) {
    std::array<uint8_t, Net::kMonsterHealthSnapshotPacketSize> packet{};
    ASSERT_TRUE(Net::serializeMonsterHealthSnapshotPacket(42, 7, 75, 100, packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    uint32_t monsterId = 0;
    uint16_t currentHp = 0;
    uint16_t maxHp = 0;
    ASSERT_TRUE(Net::parseMonsterHealthSnapshotPacket(
        packet.data(),
        packet.size(),
        header,
        roomId,
        monsterId,
        currentHp,
        maxHp));

    EXPECT_EQ(header.type, Net::TcpPacketType::kMonsterHealthSnapshot);
    EXPECT_EQ(header.size, Net::kMonsterHealthSnapshotPacketSize);
    EXPECT_EQ(roomId, 42u);
    EXPECT_EQ(monsterId, 7u);
    EXPECT_EQ(currentHp, 75u);
    EXPECT_EQ(maxHp, 100u);
}

TEST(TcpPacketTests, RejectMonsterHealthSnapshotWithInvalidSize) {
    std::array<uint8_t, Net::kMonsterHealthSnapshotPacketSize> packet{};
    ASSERT_TRUE(Net::serializeMonsterHealthSnapshotPacket(42, 7, 75, 100, packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    uint32_t monsterId = 0;
    uint16_t currentHp = 0;
    uint16_t maxHp = 0;
    EXPECT_FALSE(Net::parseMonsterHealthSnapshotPacket(
        packet.data(),
        packet.size() - 1,
        header,
        roomId,
        monsterId,
        currentHp,
        maxHp));
}

TEST(TcpPacketTests, SerializeAndParseDropListSnapshotV2Packet) {
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeDropListSnapshotV2Packet(
        42,
        12345,
        {
            Net::TcpDropEntryV2{1, 1001, 1, -500, 250},
            Net::TcpDropEntryV2{2, 1002, 3, 3000, -7000},
        },
        packet));

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    uint32_t scatterSeed = 0;
    std::vector<Net::TcpDropEntryV2> drops;
    ASSERT_TRUE(Net::parseDropListSnapshotV2Packet(
        packet.data(),
        packet.size(),
        header,
        roomId,
        scatterSeed,
        drops));

    EXPECT_EQ(header.type, Net::TcpPacketType::kDropListSnapshotV2);
    EXPECT_EQ(header.size, Net::dropListSnapshotV2PacketSize(2));
    EXPECT_EQ(roomId, 42u);
    EXPECT_EQ(scatterSeed, 12345u);
    ASSERT_EQ(drops.size(), 2u);
    EXPECT_EQ(drops[0].dropId, 1u);
    EXPECT_EQ(drops[0].itemId, 1001u);
    EXPECT_EQ(drops[0].quantity, 1u);
    EXPECT_EQ(drops[0].posX, -500);
    EXPECT_EQ(drops[0].posY, 250);
    EXPECT_EQ(drops[1].dropId, 2u);
    EXPECT_EQ(drops[1].itemId, 1002u);
    EXPECT_EQ(drops[1].quantity, 3u);
    EXPECT_EQ(drops[1].posX, 3000);
    EXPECT_EQ(drops[1].posY, -7000);
}

TEST(TcpPacketTests, RejectDropListSnapshotV2WithInvalidCountSize) {
    std::vector<uint8_t> packet;
    ASSERT_TRUE(Net::serializeDropListSnapshotV2Packet(
        42,
        12345,
        {
            Net::TcpDropEntryV2{1, 1001, 1, -500, 250},
            Net::TcpDropEntryV2{2, 1002, 3, 3000, -7000},
        },
        packet));

    packet[Net::kTcpHeaderSize + Net::kRoomIdFieldSize + Net::kScatterSeedFieldSize] = 0x00;
    packet[Net::kTcpHeaderSize + Net::kRoomIdFieldSize + Net::kScatterSeedFieldSize + 1] = 0x03;

    Net::TcpPacketHeader header;
    uint32_t roomId = 0;
    uint32_t scatterSeed = 0;
    std::vector<Net::TcpDropEntryV2> drops;
    EXPECT_FALSE(Net::parseDropListSnapshotV2Packet(
        packet.data(),
        packet.size(),
        header,
        roomId,
        scatterSeed,
        drops));
}

TEST(TcpPacketTests, RejectDropListSnapshotV2ThatExceedsMaxPacketSize) {
    std::vector<Net::TcpDropEntryV2> drops(57, Net::TcpDropEntryV2{1, 1001, 1, 0, 0});
    std::vector<uint8_t> packet;
    EXPECT_FALSE(Net::serializeDropListSnapshotV2Packet(42, 12345, drops, packet));
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
