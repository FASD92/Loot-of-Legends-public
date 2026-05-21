#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "Net/RudpGameEventPayload.hpp"

namespace {
std::vector<uint8_t> validMonsterDeathBytes() {
    return std::vector<uint8_t>{
        0x00, 0x01,
        0x00, 0x08,
        0x01, 0x02, 0x03, 0x04,
        0x10, 0x20, 0x30, 0x40};
}

std::vector<uint8_t> validLootResolvedBytes() {
    return std::vector<uint8_t>{
        0x00, 0x02,
        0x00, 0x16,
        0x01, 0x02, 0x03, 0x04,
        0x10, 0x20, 0x30, 0x40,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x0A, 0x0B, 0x0C, 0x0D,
        0x01, 0xF4};
}
}  // namespace

TEST(RudpGameEventPayloadTests, SerializesMonsterDeathInBigEndianFrameOrder) {
    const Net::RudpMonsterDeathGameEventPayload payload{
        0x01020304U,
        0x10203040U};

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(Net::serializeRudpMonsterDeathGameEventPayload(payload, bytes));

    EXPECT_EQ(bytes, validMonsterDeathBytes());
}

TEST(RudpGameEventPayloadTests, ParsesMonsterDeathPayload) {
    const std::vector<uint8_t> bytes = validMonsterDeathBytes();

    Net::RudpMonsterDeathGameEventPayload parsed;
    ASSERT_TRUE(Net::parseRudpMonsterDeathGameEventPayload(
        bytes.data(),
        bytes.size(),
        parsed));

    EXPECT_EQ(parsed.roomId, 0x01020304U);
    EXPECT_EQ(parsed.monsterId, 0x10203040U);
}

TEST(RudpGameEventPayloadTests, RoundTripsSerializedMonsterDeathPayload) {
    const Net::RudpMonsterDeathGameEventPayload payload{42, 7};

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(Net::serializeRudpMonsterDeathGameEventPayload(payload, bytes));

    Net::RudpMonsterDeathGameEventPayload parsed;
    ASSERT_TRUE(Net::parseRudpMonsterDeathGameEventPayload(
        bytes.data(),
        bytes.size(),
        parsed));
    EXPECT_EQ(parsed.roomId, payload.roomId);
    EXPECT_EQ(parsed.monsterId, payload.monsterId);
}

TEST(RudpGameEventPayloadTests, RejectsInvalidMonsterDeathPayloadSize) {
    std::vector<uint8_t> tooShort = validMonsterDeathBytes();
    tooShort.pop_back();
    std::vector<uint8_t> tooLong = validMonsterDeathBytes();
    tooLong.push_back(0x00);
    Net::RudpMonsterDeathGameEventPayload parsed{42, 7};

    EXPECT_FALSE(Net::parseRudpMonsterDeathGameEventPayload(
        tooShort.data(),
        tooShort.size(),
        parsed));
    EXPECT_EQ(parsed.roomId, 0U);
    EXPECT_EQ(parsed.monsterId, 0U);

    parsed = Net::RudpMonsterDeathGameEventPayload{42, 7};
    EXPECT_FALSE(Net::parseRudpMonsterDeathGameEventPayload(
        tooLong.data(),
        tooLong.size(),
        parsed));
    EXPECT_EQ(parsed.roomId, 0U);
    EXPECT_EQ(parsed.monsterId, 0U);
}

TEST(RudpGameEventPayloadTests, RejectsNullMonsterDeathInput) {
    Net::RudpMonsterDeathGameEventPayload parsed{42, 7};

    EXPECT_FALSE(Net::parseRudpMonsterDeathGameEventPayload(
        nullptr,
        Net::kRudpMonsterDeathGameEventPayloadSize,
        parsed));
    EXPECT_EQ(parsed.roomId, 0U);
    EXPECT_EQ(parsed.monsterId, 0U);
}

TEST(RudpGameEventPayloadTests, RejectsUnsupportedGameEventType) {
    std::vector<uint8_t> bytes = validMonsterDeathBytes();
    bytes[1] = 0x7F;
    Net::RudpMonsterDeathGameEventPayload parsed{42, 7};

    EXPECT_FALSE(Net::parseRudpMonsterDeathGameEventPayload(
        bytes.data(),
        bytes.size(),
        parsed));
    EXPECT_EQ(parsed.roomId, 0U);
    EXPECT_EQ(parsed.monsterId, 0U);
}

TEST(RudpGameEventPayloadTests, RejectsMonsterDeathBodyLengthMismatch) {
    std::vector<uint8_t> bytes = validMonsterDeathBytes();
    bytes[3] = 0x07;
    Net::RudpMonsterDeathGameEventPayload parsed{42, 7};

    EXPECT_FALSE(Net::parseRudpMonsterDeathGameEventPayload(
        bytes.data(),
        bytes.size(),
        parsed));
    EXPECT_EQ(parsed.roomId, 0U);
    EXPECT_EQ(parsed.monsterId, 0U);
}

TEST(RudpGameEventPayloadTests, RejectsInvalidMonsterDeathIdentityFieldsOnParse) {
    const std::vector<std::vector<uint8_t>> invalidPayloads = {
        {0x00, 0x01,
         0x00, 0x08,
         0x00, 0x00, 0x00, 0x00,
         0x10, 0x20, 0x30, 0x40},
        {0x00, 0x01,
         0x00, 0x08,
         0x01, 0x02, 0x03, 0x04,
         0x00, 0x00, 0x00, 0x00},
    };

    for (const std::vector<uint8_t>& bytes : invalidPayloads) {
        Net::RudpMonsterDeathGameEventPayload parsed{42, 7};
        EXPECT_FALSE(Net::parseRudpMonsterDeathGameEventPayload(
            bytes.data(),
            bytes.size(),
            parsed));
        EXPECT_EQ(parsed.roomId, 0U);
        EXPECT_EQ(parsed.monsterId, 0U);
    }
}

TEST(RudpGameEventPayloadTests, RejectsInvalidMonsterDeathIdentityFieldsOnSerialize) {
    const std::vector<Net::RudpMonsterDeathGameEventPayload> invalidPayloads = {
        Net::RudpMonsterDeathGameEventPayload{0, 7},
        Net::RudpMonsterDeathGameEventPayload{42, 0},
    };

    for (const Net::RudpMonsterDeathGameEventPayload& payload : invalidPayloads) {
        std::vector<uint8_t> bytes{0xAA, 0xBB};
        EXPECT_FALSE(Net::serializeRudpMonsterDeathGameEventPayload(payload, bytes));
        EXPECT_TRUE(bytes.empty());
    }
}

TEST(RudpGameEventPayloadTests, SerializesLootResolvedInBigEndianFrameOrder) {
    const Net::RudpLootResolvedGameEventPayload payload{
        0x01020304U,
        0x10203040U,
        0x0102030405060708ULL,
        0x0A0B0C0DU,
        500};

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(Net::serializeRudpLootResolvedGameEventPayload(payload, bytes));

    EXPECT_EQ(bytes, validLootResolvedBytes());
}

TEST(RudpGameEventPayloadTests, ParsesLootResolvedPayload) {
    const std::vector<uint8_t> bytes = validLootResolvedBytes();

    Net::RudpLootResolvedGameEventPayload parsed;
    ASSERT_TRUE(Net::parseRudpLootResolvedGameEventPayload(
        bytes.data(),
        bytes.size(),
        parsed));

    EXPECT_EQ(parsed.roomId, 0x01020304U);
    EXPECT_EQ(parsed.dropId, 0x10203040U);
    EXPECT_EQ(parsed.winnerSessionId, 0x0102030405060708ULL);
    EXPECT_EQ(parsed.itemId, 0x0A0B0C0DU);
    EXPECT_EQ(parsed.quantity, 500);
}

TEST(RudpGameEventPayloadTests, RoundTripsSerializedLootResolvedPayload) {
    const Net::RudpLootResolvedGameEventPayload payload{42, 77, 1001, 3001, 2};

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(Net::serializeRudpLootResolvedGameEventPayload(payload, bytes));

    Net::RudpLootResolvedGameEventPayload parsed;
    ASSERT_TRUE(Net::parseRudpLootResolvedGameEventPayload(
        bytes.data(),
        bytes.size(),
        parsed));
    EXPECT_EQ(parsed.roomId, payload.roomId);
    EXPECT_EQ(parsed.dropId, payload.dropId);
    EXPECT_EQ(parsed.winnerSessionId, payload.winnerSessionId);
    EXPECT_EQ(parsed.itemId, payload.itemId);
    EXPECT_EQ(parsed.quantity, payload.quantity);
}

TEST(RudpGameEventPayloadTests, PreservesLootResolvedNonIdentityZeroFields) {
    const Net::RudpLootResolvedGameEventPayload payload{42, 77, 0, 0, 0};

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(Net::serializeRudpLootResolvedGameEventPayload(payload, bytes));

    Net::RudpLootResolvedGameEventPayload parsed;
    ASSERT_TRUE(Net::parseRudpLootResolvedGameEventPayload(
        bytes.data(),
        bytes.size(),
        parsed));
    EXPECT_EQ(parsed.roomId, 42U);
    EXPECT_EQ(parsed.dropId, 77U);
    EXPECT_EQ(parsed.winnerSessionId, 0U);
    EXPECT_EQ(parsed.itemId, 0U);
    EXPECT_EQ(parsed.quantity, 0U);
}

TEST(RudpGameEventPayloadTests, RejectsInvalidLootResolvedPayloadSize) {
    std::vector<uint8_t> tooShort = validLootResolvedBytes();
    tooShort.pop_back();
    std::vector<uint8_t> tooLong = validLootResolvedBytes();
    tooLong.push_back(0x00);
    Net::RudpLootResolvedGameEventPayload parsed{42, 77, 1001, 3001, 2};

    EXPECT_FALSE(Net::parseRudpLootResolvedGameEventPayload(
        tooShort.data(),
        tooShort.size(),
        parsed));
    EXPECT_EQ(parsed.roomId, 0U);
    EXPECT_EQ(parsed.dropId, 0U);
    EXPECT_EQ(parsed.winnerSessionId, 0U);
    EXPECT_EQ(parsed.itemId, 0U);
    EXPECT_EQ(parsed.quantity, 0U);

    parsed = Net::RudpLootResolvedGameEventPayload{42, 77, 1001, 3001, 2};
    EXPECT_FALSE(Net::parseRudpLootResolvedGameEventPayload(
        tooLong.data(),
        tooLong.size(),
        parsed));
    EXPECT_EQ(parsed.roomId, 0U);
    EXPECT_EQ(parsed.dropId, 0U);
    EXPECT_EQ(parsed.winnerSessionId, 0U);
    EXPECT_EQ(parsed.itemId, 0U);
    EXPECT_EQ(parsed.quantity, 0U);
}

TEST(RudpGameEventPayloadTests, RejectsNullLootResolvedInput) {
    Net::RudpLootResolvedGameEventPayload parsed{42, 77, 1001, 3001, 2};

    EXPECT_FALSE(Net::parseRudpLootResolvedGameEventPayload(
        nullptr,
        Net::kRudpLootResolvedGameEventPayloadSize,
        parsed));
    EXPECT_EQ(parsed.roomId, 0U);
    EXPECT_EQ(parsed.dropId, 0U);
    EXPECT_EQ(parsed.winnerSessionId, 0U);
    EXPECT_EQ(parsed.itemId, 0U);
    EXPECT_EQ(parsed.quantity, 0U);
}

TEST(RudpGameEventPayloadTests, RejectsUnsupportedLootResolvedGameEventType) {
    std::vector<uint8_t> bytes = validLootResolvedBytes();
    bytes[1] = 0x01;
    Net::RudpLootResolvedGameEventPayload parsed{42, 77, 1001, 3001, 2};

    EXPECT_FALSE(Net::parseRudpLootResolvedGameEventPayload(
        bytes.data(),
        bytes.size(),
        parsed));
    EXPECT_EQ(parsed.roomId, 0U);
    EXPECT_EQ(parsed.dropId, 0U);
    EXPECT_EQ(parsed.winnerSessionId, 0U);
    EXPECT_EQ(parsed.itemId, 0U);
    EXPECT_EQ(parsed.quantity, 0U);
}

TEST(RudpGameEventPayloadTests, RejectsLootResolvedBodyLengthMismatch) {
    std::vector<uint8_t> bytes = validLootResolvedBytes();
    bytes[3] = 0x15;
    Net::RudpLootResolvedGameEventPayload parsed{42, 77, 1001, 3001, 2};

    EXPECT_FALSE(Net::parseRudpLootResolvedGameEventPayload(
        bytes.data(),
        bytes.size(),
        parsed));
    EXPECT_EQ(parsed.roomId, 0U);
    EXPECT_EQ(parsed.dropId, 0U);
    EXPECT_EQ(parsed.winnerSessionId, 0U);
    EXPECT_EQ(parsed.itemId, 0U);
    EXPECT_EQ(parsed.quantity, 0U);
}

TEST(RudpGameEventPayloadTests, RejectsInvalidLootResolvedIdentityFieldsOnParse) {
    const std::vector<std::vector<uint8_t>> invalidPayloads = {
        {0x00, 0x02,
         0x00, 0x16,
         0x00, 0x00, 0x00, 0x00,
         0x10, 0x20, 0x30, 0x40,
         0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
         0x0A, 0x0B, 0x0C, 0x0D,
         0x01, 0xF4},
        {0x00, 0x02,
         0x00, 0x16,
         0x01, 0x02, 0x03, 0x04,
         0x00, 0x00, 0x00, 0x00,
         0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
         0x0A, 0x0B, 0x0C, 0x0D,
         0x01, 0xF4},
    };

    for (const std::vector<uint8_t>& bytes : invalidPayloads) {
        Net::RudpLootResolvedGameEventPayload parsed{42, 77, 1001, 3001, 2};
        EXPECT_FALSE(Net::parseRudpLootResolvedGameEventPayload(
            bytes.data(),
            bytes.size(),
            parsed));
        EXPECT_EQ(parsed.roomId, 0U);
        EXPECT_EQ(parsed.dropId, 0U);
        EXPECT_EQ(parsed.winnerSessionId, 0U);
        EXPECT_EQ(parsed.itemId, 0U);
        EXPECT_EQ(parsed.quantity, 0U);
    }
}

TEST(RudpGameEventPayloadTests, RejectsInvalidLootResolvedIdentityFieldsOnSerialize) {
    const std::vector<Net::RudpLootResolvedGameEventPayload> invalidPayloads = {
        Net::RudpLootResolvedGameEventPayload{0, 77, 1001, 3001, 2},
        Net::RudpLootResolvedGameEventPayload{42, 0, 1001, 3001, 2},
    };

    for (const Net::RudpLootResolvedGameEventPayload& payload : invalidPayloads) {
        std::vector<uint8_t> bytes{0xAA, 0xBB};
        EXPECT_FALSE(Net::serializeRudpLootResolvedGameEventPayload(payload, bytes));
        EXPECT_TRUE(bytes.empty());
    }
}
