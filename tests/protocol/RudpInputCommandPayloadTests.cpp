#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "Net/RudpInputCommandPayload.hpp"

namespace {
void writeU32BE(uint32_t value, std::vector<uint8_t>& bytes) {
    bytes.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    bytes.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<uint8_t>(value & 0xFF));
}

std::vector<uint8_t> inputCommandPayload(
    uint32_t playerId,
    uint32_t cmdSeq,
    uint8_t op,
    const std::vector<uint8_t>& args) {
    std::vector<uint8_t> bytes;
    writeU32BE(playerId, bytes);
    writeU32BE(cmdSeq, bytes);
    bytes.push_back(op);
    bytes.push_back(static_cast<uint8_t>(args.size()));
    bytes.insert(bytes.end(), args.begin(), args.end());
    return bytes;
}

std::vector<uint8_t> u32Arg(uint32_t value) {
    std::vector<uint8_t> bytes;
    writeU32BE(value, bytes);
    return bytes;
}
}  // namespace

TEST(RudpInputCommandPayloadTests, ParsesReadyPayload) {
    const std::vector<uint8_t> bytes = inputCommandPayload(
        0x01020304U,
        0x10203040U,
        static_cast<uint8_t>(Net::RudpInputCommandOp::kReady),
        {});

    Net::RudpInputCommandPayload parsed;
    ASSERT_TRUE(Net::parseRudpInputCommandPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.playerId, 0x01020304U);
    EXPECT_EQ(parsed.cmdSeq, 0x10203040U);
    EXPECT_EQ(parsed.op, Net::RudpInputCommandOp::kReady);
    EXPECT_EQ(parsed.argValue, 0U);
}

TEST(RudpInputCommandPayloadTests, ParsesMonsterDeathPayload) {
    const std::vector<uint8_t> bytes = inputCommandPayload(
        7,
        8,
        static_cast<uint8_t>(Net::RudpInputCommandOp::kMonsterDeath),
        u32Arg(0x0A0B0C0DU));

    Net::RudpInputCommandPayload parsed;
    ASSERT_TRUE(Net::parseRudpInputCommandPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.playerId, 7U);
    EXPECT_EQ(parsed.cmdSeq, 8U);
    EXPECT_EQ(parsed.op, Net::RudpInputCommandOp::kMonsterDeath);
    EXPECT_EQ(parsed.argValue, 0x0A0B0C0DU);
}

TEST(RudpInputCommandPayloadTests, ParsesClickLootPayload) {
    const std::vector<uint8_t> bytes = inputCommandPayload(
        9,
        10,
        static_cast<uint8_t>(Net::RudpInputCommandOp::kClickLoot),
        u32Arg(0x01020304U));

    Net::RudpInputCommandPayload parsed;
    ASSERT_TRUE(Net::parseRudpInputCommandPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.playerId, 9U);
    EXPECT_EQ(parsed.cmdSeq, 10U);
    EXPECT_EQ(parsed.op, Net::RudpInputCommandOp::kClickLoot);
    EXPECT_EQ(parsed.argValue, 0x01020304U);
}

TEST(RudpInputCommandPayloadTests, RejectsPayloadBelowPrefixSize) {
    std::vector<uint8_t> bytes(Net::kRudpInputCommandPrefixSize - 1, 0);
    Net::RudpInputCommandPayload parsed{1, 2, Net::RudpInputCommandOp::kClickLoot, 3};

    EXPECT_FALSE(Net::parseRudpInputCommandPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.playerId, 0U);
    EXPECT_EQ(parsed.cmdSeq, 0U);
    EXPECT_EQ(parsed.op, Net::RudpInputCommandOp::kReady);
    EXPECT_EQ(parsed.argValue, 0U);
}

TEST(RudpInputCommandPayloadTests, RejectsArgLengthMismatch) {
    std::vector<uint8_t> bytes = inputCommandPayload(
        1,
        2,
        static_cast<uint8_t>(Net::RudpInputCommandOp::kClickLoot),
        u32Arg(3));
    bytes.pop_back();
    Net::RudpInputCommandPayload parsed;

    EXPECT_FALSE(Net::parseRudpInputCommandPayload(bytes.data(), bytes.size(), parsed));
}

TEST(RudpInputCommandPayloadTests, RejectsUnknownOp) {
    const std::vector<uint8_t> bytes = inputCommandPayload(1, 2, 0x7F, {});
    Net::RudpInputCommandPayload parsed;

    EXPECT_FALSE(Net::parseRudpInputCommandPayload(bytes.data(), bytes.size(), parsed));
}

TEST(RudpInputCommandPayloadTests, RejectsReadyWithArgs) {
    const std::vector<uint8_t> bytes = inputCommandPayload(
        1,
        2,
        static_cast<uint8_t>(Net::RudpInputCommandOp::kReady),
        u32Arg(3));
    Net::RudpInputCommandPayload parsed;

    EXPECT_FALSE(Net::parseRudpInputCommandPayload(bytes.data(), bytes.size(), parsed));
}

TEST(RudpInputCommandPayloadTests, RejectsActionPayloadWithWrongArgLength) {
    const std::vector<uint8_t> monsterDeath = inputCommandPayload(
        1,
        2,
        static_cast<uint8_t>(Net::RudpInputCommandOp::kMonsterDeath),
        {0x00, 0x01});
    const std::vector<uint8_t> clickLoot = inputCommandPayload(
        1,
        2,
        static_cast<uint8_t>(Net::RudpInputCommandOp::kClickLoot),
        {0x00, 0x01});
    Net::RudpInputCommandPayload parsed;

    EXPECT_FALSE(Net::parseRudpInputCommandPayload(
        monsterDeath.data(),
        monsterDeath.size(),
        parsed));
    EXPECT_FALSE(Net::parseRudpInputCommandPayload(
        clickLoot.data(),
        clickLoot.size(),
        parsed));
}

TEST(RudpInputCommandPayloadTests, RejectsNullInput) {
    Net::RudpInputCommandPayload parsed{1, 2, Net::RudpInputCommandOp::kClickLoot, 3};

    EXPECT_FALSE(Net::parseRudpInputCommandPayload(
        nullptr,
        Net::kRudpInputCommandPrefixSize,
        parsed));
    EXPECT_EQ(parsed.playerId, 0U);
    EXPECT_EQ(parsed.cmdSeq, 0U);
    EXPECT_EQ(parsed.op, Net::RudpInputCommandOp::kReady);
    EXPECT_EQ(parsed.argValue, 0U);
}
