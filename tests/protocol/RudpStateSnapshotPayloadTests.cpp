#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "Net/RudpStateSnapshotPayload.hpp"

namespace {
std::vector<uint8_t> validPayloadBytes() {
    return std::vector<uint8_t>{
        0x01,
        0x01,
        0x01, 0x02, 0x03, 0x04,
        0x10, 0x20, 0x30, 0x40,
        0x00, 0x02,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0xFF, 0xFF, 0xFC, 0x18,
        0x00, 0x00, 0x03, 0xE8,
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80,
        0x80, 0x00, 0x00, 0x00,
        0x7F, 0xFF, 0xFF, 0xFF};
}

std::vector<uint8_t> emptyPayloadBytes() {
    return std::vector<uint8_t>{
        0x01,
        0x01,
        0x00, 0x00, 0x00, 0x2A,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00};
}

Net::RudpStateSnapshotPayload validPayload() {
    return Net::RudpStateSnapshotPayload{
        Net::kRudpStateSnapshotVersion,
        Net::kRudpStateSnapshotKindRoomMovementPlayers,
        0x01020304U,
        0x10203040U,
        {
            Net::RudpStateSnapshotPlayer{
                0x0102030405060708ULL,
                -1000,
                1000},
            Net::RudpStateSnapshotPlayer{
                0x1020304050607080ULL,
                std::numeric_limits<int32_t>::min(),
                std::numeric_limits<int32_t>::max()},
        }};
}
}  // namespace

TEST(RudpStateSnapshotPayloadTests, SerializesValidV1InBigEndianOrder) {
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(Net::serializeRudpStateSnapshotPayload(validPayload(), bytes));

    EXPECT_EQ(bytes, validPayloadBytes());
}

TEST(RudpStateSnapshotPayloadTests, ParsesValidV1Payload) {
    const std::vector<uint8_t> bytes = validPayloadBytes();

    Net::RudpStateSnapshotPayload parsed;
    ASSERT_TRUE(Net::parseRudpStateSnapshotPayload(bytes.data(), bytes.size(), parsed));

    EXPECT_EQ(parsed.snapshotVersion, Net::kRudpStateSnapshotVersion);
    EXPECT_EQ(parsed.snapshotKind, Net::kRudpStateSnapshotKindRoomMovementPlayers);
    EXPECT_EQ(parsed.roomId, 0x01020304U);
    EXPECT_EQ(parsed.serverTick, 0x10203040U);
    ASSERT_EQ(parsed.players.size(), 2U);
    EXPECT_EQ(parsed.players[0].sessionId, 0x0102030405060708ULL);
    EXPECT_EQ(parsed.players[0].posX, -1000);
    EXPECT_EQ(parsed.players[0].posY, 1000);
    EXPECT_EQ(parsed.players[1].sessionId, 0x1020304050607080ULL);
    EXPECT_EQ(parsed.players[1].posX, std::numeric_limits<int32_t>::min());
    EXPECT_EQ(parsed.players[1].posY, std::numeric_limits<int32_t>::max());
}

TEST(RudpStateSnapshotPayloadTests, RoundTripsMultiplePlayers) {
    const Net::RudpStateSnapshotPayload payload = validPayload();

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(Net::serializeRudpStateSnapshotPayload(payload, bytes));

    Net::RudpStateSnapshotPayload parsed;
    ASSERT_TRUE(Net::parseRudpStateSnapshotPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.snapshotVersion, payload.snapshotVersion);
    EXPECT_EQ(parsed.snapshotKind, payload.snapshotKind);
    EXPECT_EQ(parsed.roomId, payload.roomId);
    EXPECT_EQ(parsed.serverTick, payload.serverTick);
    ASSERT_EQ(parsed.players.size(), payload.players.size());
    for (size_t i = 0; i < payload.players.size(); ++i) {
        EXPECT_EQ(parsed.players[i].sessionId, payload.players[i].sessionId);
        EXPECT_EQ(parsed.players[i].posX, payload.players[i].posX);
        EXPECT_EQ(parsed.players[i].posY, payload.players[i].posY);
    }
}

TEST(RudpStateSnapshotPayloadTests, RoundTripsEmptySnapshot) {
    const Net::RudpStateSnapshotPayload payload{
        Net::kRudpStateSnapshotVersion,
        Net::kRudpStateSnapshotKindRoomMovementPlayers,
        42,
        0,
        {}};

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(Net::serializeRudpStateSnapshotPayload(payload, bytes));
    EXPECT_EQ(bytes, emptyPayloadBytes());

    Net::RudpStateSnapshotPayload parsed;
    ASSERT_TRUE(Net::parseRudpStateSnapshotPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.roomId, 42U);
    EXPECT_EQ(parsed.serverTick, 0U);
    EXPECT_TRUE(parsed.players.empty());
}

TEST(RudpStateSnapshotPayloadTests, RejectsNullInputAndMalformedLength) {
    Net::RudpStateSnapshotPayload parsed = validPayload();
    EXPECT_FALSE(Net::parseRudpStateSnapshotPayload(
        nullptr,
        Net::kRudpStateSnapshotFixedPayloadSize,
        parsed));
    EXPECT_EQ(parsed.roomId, 0U);

    std::vector<uint8_t> tooShort = validPayloadBytes();
    tooShort.pop_back();
    EXPECT_FALSE(Net::parseRudpStateSnapshotPayload(
        tooShort.data(),
        tooShort.size(),
        parsed));
    EXPECT_EQ(parsed.roomId, 0U);

    std::vector<uint8_t> tooLong = validPayloadBytes();
    tooLong.push_back(0x00);
    EXPECT_FALSE(Net::parseRudpStateSnapshotPayload(
        tooLong.data(),
        tooLong.size(),
        parsed));
    EXPECT_EQ(parsed.roomId, 0U);
}

TEST(RudpStateSnapshotPayloadTests, RejectsUnsupportedVersionOrKind) {
    std::vector<uint8_t> bytes = validPayloadBytes();
    Net::RudpStateSnapshotPayload parsed;

    bytes[0] = 0x02;
    EXPECT_FALSE(Net::parseRudpStateSnapshotPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.roomId, 0U);

    bytes = validPayloadBytes();
    bytes[1] = 0x02;
    EXPECT_FALSE(Net::parseRudpStateSnapshotPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.roomId, 0U);

    Net::RudpStateSnapshotPayload payload = validPayload();
    std::vector<uint8_t> serialized{0xAA, 0xBB};
    payload.snapshotVersion = 2;
    EXPECT_FALSE(Net::serializeRudpStateSnapshotPayload(payload, serialized));
    EXPECT_TRUE(serialized.empty());

    payload = validPayload();
    payload.snapshotKind = 2;
    EXPECT_FALSE(Net::serializeRudpStateSnapshotPayload(payload, serialized));
    EXPECT_TRUE(serialized.empty());
}

TEST(RudpStateSnapshotPayloadTests, RejectsInvalidIdentityFields) {
    Net::RudpStateSnapshotPayload payload = validPayload();
    std::vector<uint8_t> serialized;

    payload.roomId = 0;
    EXPECT_FALSE(Net::serializeRudpStateSnapshotPayload(payload, serialized));
    EXPECT_TRUE(serialized.empty());

    payload = validPayload();
    payload.players[0].sessionId = 0;
    EXPECT_FALSE(Net::serializeRudpStateSnapshotPayload(payload, serialized));
    EXPECT_TRUE(serialized.empty());

    payload = validPayload();
    payload.players[1].sessionId = payload.players[0].sessionId;
    EXPECT_FALSE(Net::serializeRudpStateSnapshotPayload(payload, serialized));
    EXPECT_TRUE(serialized.empty());

    std::vector<uint8_t> bytes = validPayloadBytes();
    bytes[5] = 0x00;
    bytes[4] = 0x00;
    bytes[3] = 0x00;
    bytes[2] = 0x00;
    Net::RudpStateSnapshotPayload parsed;
    EXPECT_FALSE(Net::parseRudpStateSnapshotPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.roomId, 0U);

    bytes = validPayloadBytes();
    for (size_t i = 12; i < 20; ++i) {
        bytes[i] = 0x00;
    }
    EXPECT_FALSE(Net::parseRudpStateSnapshotPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.roomId, 0U);

    bytes = validPayloadBytes();
    for (size_t i = 0; i < 8; ++i) {
        bytes[28 + i] = bytes[12 + i];
    }
    EXPECT_FALSE(Net::parseRudpStateSnapshotPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.roomId, 0U);
}

TEST(RudpStateSnapshotPayloadTests, RejectsPlayerCountLengthMismatch) {
    std::vector<uint8_t> bytes = validPayloadBytes();
    Net::RudpStateSnapshotPayload parsed;

    bytes[11] = 0x03;
    EXPECT_FALSE(Net::parseRudpStateSnapshotPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.roomId, 0U);

    bytes = validPayloadBytes();
    bytes[11] = 0x01;
    EXPECT_FALSE(Net::parseRudpStateSnapshotPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.roomId, 0U);
}

TEST(RudpStateSnapshotPayloadTests, AcceptsMaxPlayerCountWithinMtu) {
    Net::RudpStateSnapshotPayload payload{
        Net::kRudpStateSnapshotVersion,
        Net::kRudpStateSnapshotKindRoomMovementPlayers,
        42,
        7,
        {}};
    for (size_t i = 0; i < Net::kMaxRudpStateSnapshotPlayersPerPayload; ++i) {
        payload.players.push_back(Net::RudpStateSnapshotPlayer{
            1001ULL + static_cast<uint64_t>(i),
            static_cast<int32_t>(i),
            -static_cast<int32_t>(i)});
    }

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(Net::serializeRudpStateSnapshotPayload(payload, bytes));
    EXPECT_LE(bytes.size(), Net::kMaxRudpPayloadSize);

    Net::RudpStateSnapshotPayload parsed;
    ASSERT_TRUE(Net::parseRudpStateSnapshotPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.players.size(), Net::kMaxRudpStateSnapshotPlayersPerPayload);
}

TEST(RudpStateSnapshotPayloadTests, RejectsOverMtuPlayerCount) {
    Net::RudpStateSnapshotPayload payload{
        Net::kRudpStateSnapshotVersion,
        Net::kRudpStateSnapshotKindRoomMovementPlayers,
        42,
        7,
        {}};
    for (size_t i = 0; i <= Net::kMaxRudpStateSnapshotPlayersPerPayload; ++i) {
        payload.players.push_back(Net::RudpStateSnapshotPlayer{
            1001ULL + static_cast<uint64_t>(i),
            0,
            0});
    }

    std::vector<uint8_t> bytes{0xAA, 0xBB};
    EXPECT_FALSE(Net::serializeRudpStateSnapshotPayload(payload, bytes));
    EXPECT_TRUE(bytes.empty());

    bytes.assign(
        Net::kRudpStateSnapshotFixedPayloadSize +
            ((Net::kMaxRudpStateSnapshotPlayersPerPayload + 1) *
             Net::kRudpStateSnapshotPlayerEntrySize),
        0);
    bytes[0] = Net::kRudpStateSnapshotVersion;
    bytes[1] = Net::kRudpStateSnapshotKindRoomMovementPlayers;
    bytes[5] = 42;
    bytes[11] = static_cast<uint8_t>(
        Net::kMaxRudpStateSnapshotPlayersPerPayload + 1);

    Net::RudpStateSnapshotPayload parsed;
    EXPECT_FALSE(Net::parseRudpStateSnapshotPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.roomId, 0U);
}
