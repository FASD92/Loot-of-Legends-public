#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "Net/RudpBattleStartRosterPayload.hpp"

TEST(RudpBattleStartRosterPayloadTests, SerializesAndParsesRosterPayload) {
    const Net::RudpBattleStartRosterPayload payload{
        0x01020304U,
        {0x0102030405060708ULL, 0x1020304050607080ULL, 0x1112131415161718ULL}};

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(Net::serializeRudpBattleStartRosterPayload(payload, bytes));
    EXPECT_EQ(bytes.size(), Net::rudpBattleStartRosterPayloadSize(3));

    Net::RudpBattleStartRosterPayload parsed;
    ASSERT_TRUE(Net::parseRudpBattleStartRosterPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.roomId, payload.roomId);
    EXPECT_EQ(parsed.playerSessionIds, payload.playerSessionIds);
}

TEST(RudpBattleStartRosterPayloadTests, RejectsInvalidRosterFieldsOnSerialize) {
    std::vector<uint8_t> bytes{0xAA};
    EXPECT_FALSE(Net::serializeRudpBattleStartRosterPayload(
        Net::RudpBattleStartRosterPayload{0, {1001, 1002}},
        bytes));
    EXPECT_TRUE(bytes.empty());

    EXPECT_FALSE(Net::serializeRudpBattleStartRosterPayload(
        Net::RudpBattleStartRosterPayload{42, {1001}},
        bytes));
    EXPECT_TRUE(bytes.empty());

    EXPECT_FALSE(Net::serializeRudpBattleStartRosterPayload(
        Net::RudpBattleStartRosterPayload{42, {1001, 0, 1003}},
        bytes));
    EXPECT_TRUE(bytes.empty());

    EXPECT_FALSE(Net::serializeRudpBattleStartRosterPayload(
        Net::RudpBattleStartRosterPayload{42, {1001, 1002, 1001}},
        bytes));
    EXPECT_TRUE(bytes.empty());
}

TEST(RudpBattleStartRosterPayloadTests, RejectsMalformedInputOnParse) {
    const Net::RudpBattleStartRosterPayload payload{42, {1001, 1002, 1003}};
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(Net::serializeRudpBattleStartRosterPayload(payload, bytes));

    Net::RudpBattleStartRosterPayload parsed{9, {7}};
    EXPECT_FALSE(Net::parseRudpBattleStartRosterPayload(nullptr, bytes.size(), parsed));
    EXPECT_EQ(parsed.roomId, 0u);
    EXPECT_TRUE(parsed.playerSessionIds.empty());

    parsed = Net::RudpBattleStartRosterPayload{9, {7}};
    std::vector<uint8_t> tooShort = bytes;
    tooShort.pop_back();
    EXPECT_FALSE(Net::parseRudpBattleStartRosterPayload(
        tooShort.data(),
        tooShort.size(),
        parsed));
    EXPECT_EQ(parsed.roomId, 0u);
    EXPECT_TRUE(parsed.playerSessionIds.empty());

    parsed = Net::RudpBattleStartRosterPayload{9, {7}};
    std::vector<uint8_t> tooLong = bytes;
    tooLong.push_back(0x00);
    EXPECT_FALSE(Net::parseRudpBattleStartRosterPayload(
        tooLong.data(),
        tooLong.size(),
        parsed));
    EXPECT_EQ(parsed.roomId, 0u);
    EXPECT_TRUE(parsed.playerSessionIds.empty());
}
