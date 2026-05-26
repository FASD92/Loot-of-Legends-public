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

void writeI16BE(int16_t value, std::vector<uint8_t>& bytes) {
    const auto encoded = static_cast<uint16_t>(value);
    bytes.push_back(static_cast<uint8_t>((encoded >> 8) & 0xFF));
    bytes.push_back(static_cast<uint8_t>(encoded & 0xFF));
}

void writeU16BE(uint16_t value, std::vector<uint8_t>& bytes) {
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

std::vector<uint8_t> moveArgs(int16_t dirX, int16_t dirY, uint16_t inputFlags) {
    std::vector<uint8_t> bytes;
    writeI16BE(dirX, bytes);
    writeI16BE(dirY, bytes);
    writeU16BE(inputFlags, bytes);
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

TEST(RudpInputCommandPayloadTests, ParsesMovePayload) {
    const std::vector<uint8_t> bytes = inputCommandPayload(
        11,
        12,
        static_cast<uint8_t>(Net::RudpInputCommandOp::kMove),
        moveArgs(1234, -2345, 0x00F0));

    Net::RudpInputCommandPayload parsed;
    ASSERT_TRUE(Net::parseRudpInputCommandPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.playerId, 11U);
    EXPECT_EQ(parsed.cmdSeq, 12U);
    EXPECT_EQ(parsed.op, Net::RudpInputCommandOp::kMove);
    EXPECT_EQ(parsed.argValue, 0U);
    EXPECT_EQ(parsed.move.dirX, 1234);
    EXPECT_EQ(parsed.move.dirY, -2345);
    EXPECT_EQ(parsed.move.inputFlags, 0x00F0U);
}

TEST(RudpInputCommandPayloadTests, ParsesMoveZeroVector) {
    const std::vector<uint8_t> bytes = inputCommandPayload(
        11,
        13,
        static_cast<uint8_t>(Net::RudpInputCommandOp::kMove),
        moveArgs(0, 0, 0));

    Net::RudpInputCommandPayload parsed;
    ASSERT_TRUE(Net::parseRudpInputCommandPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.op, Net::RudpInputCommandOp::kMove);
    EXPECT_EQ(parsed.move.dirX, 0);
    EXPECT_EQ(parsed.move.dirY, 0);
    EXPECT_EQ(parsed.move.inputFlags, 0U);
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
    const std::vector<uint8_t> move = inputCommandPayload(
        1,
        2,
        static_cast<uint8_t>(Net::RudpInputCommandOp::kMove),
        {0x00, 0x01, 0x02, 0x03});
    Net::RudpInputCommandPayload parsed;

    EXPECT_FALSE(Net::parseRudpInputCommandPayload(
        monsterDeath.data(),
        monsterDeath.size(),
        parsed));
    EXPECT_FALSE(Net::parseRudpInputCommandPayload(
        clickLoot.data(),
        clickLoot.size(),
        parsed));
    EXPECT_FALSE(Net::parseRudpInputCommandPayload(
        move.data(),
        move.size(),
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
