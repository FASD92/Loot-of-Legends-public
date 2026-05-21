#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Net/RudpPacket.hpp"

namespace {
Net::RudpPacketHeader sampleHeader() {
    Net::RudpPacketHeader header;
    header.flags = Net::kRudpFlagReliable;
    header.channelId = static_cast<uint8_t>(Net::RudpChannelId::kEvent);
    header.packetType = static_cast<uint16_t>(Net::RudpPacketType::kBattleStart);
    header.sequence = 0x01020304U;
    header.ack = 0x05060708U;
    header.ackBits = 0xA0B0C0D0U;
    return header;
}

uint16_t readU16BE(const std::vector<uint8_t>& data, size_t offset) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[offset]) << 8U) |
                                 static_cast<uint16_t>(data[offset + 1]));
}

uint32_t readU32BE(const std::vector<uint8_t>& data, size_t offset) {
    return (static_cast<uint32_t>(data[offset]) << 24U) |
           (static_cast<uint32_t>(data[offset + 1]) << 16U) |
           (static_cast<uint32_t>(data[offset + 2]) << 8U) |
           static_cast<uint32_t>(data[offset + 3]);
}
}  // namespace

TEST(PacketSerializerTests, SerializeHeaderFields) {
    const std::vector<uint8_t> payload{0xDE, 0xAD, 0xBE, 0xEF};
    std::vector<uint8_t> packet;

    ASSERT_TRUE(Net::serializeRudpPacket(sampleHeader(), payload, packet));

    ASSERT_EQ(packet.size(), Net::kRudpHeaderSize + payload.size());
    EXPECT_EQ(packet[0], Net::kRudpMagic0);
    EXPECT_EQ(packet[1], Net::kRudpMagic1);
    EXPECT_EQ(packet[2], Net::kRudpVersion);
    EXPECT_EQ(packet[3], Net::kRudpFlagReliable);
    EXPECT_EQ(packet[4], Net::kRudpHeaderSize);
    EXPECT_EQ(packet[5], static_cast<uint8_t>(Net::RudpChannelId::kEvent));
    EXPECT_EQ(readU16BE(packet, 6), static_cast<uint16_t>(Net::RudpPacketType::kBattleStart));
    EXPECT_EQ(readU32BE(packet, 8), 0x01020304U);
    EXPECT_EQ(readU32BE(packet, 12), 0x05060708U);
    EXPECT_EQ(readU32BE(packet, 16), 0xA0B0C0D0U);
    EXPECT_EQ(readU16BE(packet, 20), payload.size());
    EXPECT_NE(readU32BE(packet, 22), 0U);
    EXPECT_EQ(readU16BE(packet, 26), 0U);
    EXPECT_EQ(std::vector<uint8_t>(packet.begin() + Net::kRudpHeaderSize, packet.end()), payload);
}

TEST(PacketSerializerTests, SerializePayloadLengthZero) {
    std::vector<uint8_t> packet;

    ASSERT_TRUE(Net::serializeRudpPacket(sampleHeader(), {}, packet));

    EXPECT_EQ(packet.size(), Net::kRudpHeaderSize);
    EXPECT_EQ(readU16BE(packet, 20), 0U);
}

TEST(PacketSerializerTests, SerializeMaxPayloadSize) {
    const std::vector<uint8_t> payload(Net::kMaxRudpPayloadSize, 0x7A);
    std::vector<uint8_t> packet;

    ASSERT_TRUE(Net::serializeRudpPacket(sampleHeader(), payload, packet));

    EXPECT_EQ(packet.size(), Net::kMaxRudpPacketSize);
    EXPECT_EQ(readU16BE(packet, 20), Net::kMaxRudpPayloadSize);
}

TEST(PacketSerializerTests, RejectInvalidPayloadLength) {
    const std::vector<uint8_t> payload(Net::kMaxRudpPayloadSize + 1, 0x7A);
    std::vector<uint8_t> packet{0xAA};

    EXPECT_FALSE(Net::serializeRudpPacket(sampleHeader(), payload, packet));
}

TEST(PacketSerializerTests, ComputesCrc32CheckVector) {
    const std::string input = "123456789";

    EXPECT_EQ(
        Net::computeRudpCrc32(
            reinterpret_cast<const uint8_t*>(input.data()),
            input.size()),
        0xCBF43926U);
}
