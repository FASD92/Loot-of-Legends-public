#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "Net/RudpBattleStartPayload.hpp"

namespace {
std::vector<uint8_t> validPayloadBytes() {
    return std::vector<uint8_t>{
        0x01, 0x02, 0x03, 0x04,
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
        0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
}
}  // namespace

TEST(RudpBattleStartPayloadTests, SerializesValidPayloadInBigEndianOrder) {
    const Net::RudpBattleStartPayload payload{
        0x01020304U,
        0x0102030405060708ULL,
        0x1020304050607080ULL};

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(Net::serializeRudpBattleStartPayload(payload, bytes));

    EXPECT_EQ(bytes, validPayloadBytes());
}

TEST(RudpBattleStartPayloadTests, ParsesValidPayload) {
    const std::vector<uint8_t> bytes = validPayloadBytes();

    Net::RudpBattleStartPayload parsed;
    ASSERT_TRUE(Net::parseRudpBattleStartPayload(bytes.data(), bytes.size(), parsed));

    EXPECT_EQ(parsed.roomId, 0x01020304U);
    EXPECT_EQ(parsed.playerASessionId, 0x0102030405060708ULL);
    EXPECT_EQ(parsed.playerBSessionId, 0x1020304050607080ULL);
}

TEST(RudpBattleStartPayloadTests, RoundTripsSerializedPayload) {
    const Net::RudpBattleStartPayload payload{42, 1001, 1002};

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(Net::serializeRudpBattleStartPayload(payload, bytes));

    Net::RudpBattleStartPayload parsed;
    ASSERT_TRUE(Net::parseRudpBattleStartPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.roomId, payload.roomId);
    EXPECT_EQ(parsed.playerASessionId, payload.playerASessionId);
    EXPECT_EQ(parsed.playerBSessionId, payload.playerBSessionId);
}

TEST(RudpBattleStartPayloadTests, RejectsInvalidPayloadSize) {
    std::vector<uint8_t> tooShort = validPayloadBytes();
    tooShort.pop_back();
    std::vector<uint8_t> tooLong = validPayloadBytes();
    tooLong.push_back(0x00);
    Net::RudpBattleStartPayload parsed{42, 1001, 1002};

    EXPECT_FALSE(Net::parseRudpBattleStartPayload(tooShort.data(), tooShort.size(), parsed));
    EXPECT_EQ(parsed.roomId, 0U);
    EXPECT_EQ(parsed.playerASessionId, 0U);
    EXPECT_EQ(parsed.playerBSessionId, 0U);

    parsed = Net::RudpBattleStartPayload{42, 1001, 1002};
    EXPECT_FALSE(Net::parseRudpBattleStartPayload(tooLong.data(), tooLong.size(), parsed));
    EXPECT_EQ(parsed.roomId, 0U);
    EXPECT_EQ(parsed.playerASessionId, 0U);
    EXPECT_EQ(parsed.playerBSessionId, 0U);
}

TEST(RudpBattleStartPayloadTests, RejectsNullInput) {
    Net::RudpBattleStartPayload parsed{42, 1001, 1002};

    EXPECT_FALSE(Net::parseRudpBattleStartPayload(
        nullptr,
        Net::kRudpBattleStartPayloadSize,
        parsed));
    EXPECT_EQ(parsed.roomId, 0U);
    EXPECT_EQ(parsed.playerASessionId, 0U);
    EXPECT_EQ(parsed.playerBSessionId, 0U);
}

TEST(RudpBattleStartPayloadTests, RejectsInvalidIdentityFieldsOnParse) {
    const std::vector<std::vector<uint8_t>> invalidPayloads = {
        {0x00, 0x00, 0x00, 0x00,
         0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
         0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80},
        {0x01, 0x02, 0x03, 0x04,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
         0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80},
        {0x01, 0x02, 0x03, 0x04,
         0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
         0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
        {0x01, 0x02, 0x03, 0x04,
         0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
         0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08},
    };

    for (const std::vector<uint8_t>& bytes : invalidPayloads) {
        Net::RudpBattleStartPayload parsed{42, 1001, 1002};
        EXPECT_FALSE(Net::parseRudpBattleStartPayload(bytes.data(), bytes.size(), parsed));
        EXPECT_EQ(parsed.roomId, 0U);
        EXPECT_EQ(parsed.playerASessionId, 0U);
        EXPECT_EQ(parsed.playerBSessionId, 0U);
    }
}

TEST(RudpBattleStartPayloadTests, RejectsInvalidIdentityFieldsOnSerialize) {
    const std::vector<Net::RudpBattleStartPayload> invalidPayloads = {
        Net::RudpBattleStartPayload{0, 1001, 1002},
        Net::RudpBattleStartPayload{42, 0, 1002},
        Net::RudpBattleStartPayload{42, 1001, 0},
        Net::RudpBattleStartPayload{42, 1001, 1001},
    };

    for (const Net::RudpBattleStartPayload& payload : invalidPayloads) {
        std::vector<uint8_t> bytes{0xAA, 0xBB};
        EXPECT_FALSE(Net::serializeRudpBattleStartPayload(payload, bytes));
        EXPECT_TRUE(bytes.empty());
    }
}
