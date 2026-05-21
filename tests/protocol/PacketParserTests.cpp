#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Net/RudpPacket.hpp"

namespace {
Net::RudpPacketHeader sampleHeader() {
    Net::RudpPacketHeader header;
    header.flags = Net::kRudpFlagReliable;
    header.channelId = static_cast<uint8_t>(Net::RudpChannelId::kEvent);
    header.packetType = static_cast<uint16_t>(Net::RudpPacketType::kGameEvent);
    header.sequence = 0x01020304U;
    header.ack = 0x05060708U;
    header.ackBits = 0xA0B0C0D0U;
    return header;
}

std::vector<uint8_t> validPacket() {
    std::vector<uint8_t> packet;
    const std::vector<uint8_t> payload{0x10, 0x20, 0x30};
    EXPECT_TRUE(Net::serializeRudpPacket(sampleHeader(), payload, packet));
    return packet;
}

bool parsePacket(const std::vector<uint8_t>& packet) {
    Net::RudpPacketHeader header;
    std::vector<uint8_t> payload;
    return Net::parseRudpPacket(packet.data(), packet.size(), header, payload);
}
}  // namespace

TEST(PacketParserTests, ParseValidPacket) {
    const std::vector<uint8_t> packet = validPacket();
    Net::RudpPacketHeader header;
    std::vector<uint8_t> payload;

    ASSERT_TRUE(Net::parseRudpPacket(packet.data(), packet.size(), header, payload));

    EXPECT_EQ(header.flags, Net::kRudpFlagReliable);
    EXPECT_EQ(header.channelId, static_cast<uint8_t>(Net::RudpChannelId::kEvent));
    EXPECT_EQ(header.packetType, static_cast<uint16_t>(Net::RudpPacketType::kGameEvent));
    EXPECT_EQ(header.sequence, 0x01020304U);
    EXPECT_EQ(header.ack, 0x05060708U);
    EXPECT_EQ(header.ackBits, 0xA0B0C0D0U);
    EXPECT_EQ(header.payloadLen, 3U);
    EXPECT_NE(header.checksum, 0U);
    EXPECT_EQ(payload, (std::vector<uint8_t>{0x10, 0x20, 0x30}));
}

TEST(PacketParserTests, RejectPacketBelowHeaderSize) {
    const std::vector<uint8_t> packet(Net::kRudpHeaderSize - 1, 0);
    Net::RudpPacketHeader header;
    std::vector<uint8_t> payload;

    EXPECT_FALSE(Net::parseRudpPacket(packet.data(), packet.size(), header, payload));
}

TEST(PacketParserTests, RejectPacketAboveMaxSize) {
    const std::vector<uint8_t> packet(Net::kMaxRudpPacketSize + 1, 0);

    EXPECT_FALSE(parsePacket(packet));
}

TEST(PacketParserTests, RejectInvalidFixedHeaderFields) {
    std::vector<uint8_t> packet = validPacket();
    packet[0] = 0x00;
    EXPECT_FALSE(parsePacket(packet));

    packet = validPacket();
    packet[2] = 0x02;
    EXPECT_FALSE(parsePacket(packet));

    packet = validPacket();
    packet[4] = 0x1B;
    EXPECT_FALSE(parsePacket(packet));

    packet = validPacket();
    packet[26] = 0x00;
    packet[27] = 0x01;
    EXPECT_FALSE(parsePacket(packet));
}

TEST(PacketParserTests, RejectPayloadLengthMismatch) {
    std::vector<uint8_t> packet = validPacket();
    packet[20] = 0x00;
    packet[21] = 0x04;

    EXPECT_FALSE(parsePacket(packet));
}

TEST(PacketParserTests, RejectChecksumMismatch) {
    std::vector<uint8_t> packet = validPacket();
    packet[Net::kRudpHeaderSize] ^= 0xFF;

    EXPECT_FALSE(parsePacket(packet));
}
