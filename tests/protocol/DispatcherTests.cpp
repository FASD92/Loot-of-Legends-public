#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "Net/RudpPacket.hpp"

namespace {
std::vector<uint8_t> makePacket(
    uint16_t packetType,
    uint8_t channelId,
    uint8_t flags,
    const std::vector<uint8_t>& payload = {0x10}) {
    Net::RudpPacketHeader header;
    header.flags = flags;
    header.channelId = channelId;
    header.packetType = packetType;
    header.sequence = 1;
    header.ack = 0;
    header.ackBits = 0;

    std::vector<uint8_t> packet;
    EXPECT_TRUE(Net::serializeRudpPacket(header, payload, packet));
    return packet;
}

bool parsePacket(const std::vector<uint8_t>& packet) {
    Net::RudpPacketHeader header;
    std::vector<uint8_t> payload;
    return Net::parseRudpPacket(packet.data(), packet.size(), header, payload);
}
}  // namespace

TEST(DispatcherTests, DispatchKnownPacketType) {
    EXPECT_TRUE(parsePacket(makePacket(
        static_cast<uint16_t>(Net::RudpPacketType::kHello),
        static_cast<uint8_t>(Net::RudpChannelId::kControl),
        Net::kRudpFlagReliable)));
    EXPECT_TRUE(parsePacket(makePacket(
        static_cast<uint16_t>(Net::RudpPacketType::kInputCommand),
        static_cast<uint8_t>(Net::RudpChannelId::kInput),
        0)));
    EXPECT_TRUE(parsePacket(makePacket(
        static_cast<uint16_t>(Net::RudpPacketType::kStateSnapshot),
        static_cast<uint8_t>(Net::RudpChannelId::kSnapshot),
        0)));
    EXPECT_TRUE(parsePacket(makePacket(
        static_cast<uint16_t>(Net::RudpPacketType::kBattleStart),
        static_cast<uint8_t>(Net::RudpChannelId::kEvent),
        Net::kRudpFlagReliable)));
    EXPECT_TRUE(parsePacket(makePacket(
        static_cast<uint16_t>(Net::RudpPacketType::kGameEvent),
        static_cast<uint8_t>(Net::RudpChannelId::kEvent),
        Net::kRudpFlagReliable)));
    EXPECT_TRUE(parsePacket(makePacket(
        static_cast<uint16_t>(Net::RudpPacketType::kMetaResponse),
        static_cast<uint8_t>(Net::RudpChannelId::kControl),
        Net::kRudpFlagReliable)));
    EXPECT_TRUE(parsePacket(makePacket(
        static_cast<uint16_t>(Net::RudpPacketType::kError),
        static_cast<uint8_t>(Net::RudpChannelId::kControl),
        Net::kRudpFlagReliable)));
}

TEST(DispatcherTests, RejectUnknownPacketType) {
    EXPECT_FALSE(parsePacket(makePacket(
        0x9999,
        static_cast<uint8_t>(Net::RudpChannelId::kControl),
        Net::kRudpFlagReliable)));
}

TEST(DispatcherTests, EnforceTypeWhitelist) {
    EXPECT_FALSE(parsePacket(makePacket(
        static_cast<uint16_t>(Net::RudpPacketType::kHello),
        static_cast<uint8_t>(Net::RudpChannelId::kEvent),
        Net::kRudpFlagReliable)));
    EXPECT_FALSE(parsePacket(makePacket(
        static_cast<uint16_t>(Net::RudpPacketType::kInputCommand),
        static_cast<uint8_t>(Net::RudpChannelId::kControl),
        0)));
}

TEST(DispatcherTests, RejectReservedFutureAndUnknownFlags) {
    EXPECT_FALSE(parsePacket(makePacket(
        static_cast<uint16_t>(Net::RudpPacketType::kGameEvent),
        static_cast<uint8_t>(Net::RudpChannelId::kEvent),
        Net::kRudpFlagFragmented)));
    EXPECT_FALSE(parsePacket(makePacket(
        static_cast<uint16_t>(Net::RudpPacketType::kGameEvent),
        static_cast<uint8_t>(Net::RudpChannelId::kEvent),
        Net::kRudpFlagEncrypted)));
    EXPECT_FALSE(parsePacket(makePacket(
        static_cast<uint16_t>(Net::RudpPacketType::kGameEvent),
        static_cast<uint8_t>(Net::RudpChannelId::kEvent),
        0x80)));
}

TEST(DispatcherTests, AckOnlyRequiresEmptyPayload) {
    EXPECT_TRUE(parsePacket(makePacket(
        static_cast<uint16_t>(Net::RudpPacketType::kHello),
        static_cast<uint8_t>(Net::RudpChannelId::kControl),
        Net::kRudpFlagAckOnly,
        {})));
    EXPECT_FALSE(parsePacket(makePacket(
        static_cast<uint16_t>(Net::RudpPacketType::kHello),
        static_cast<uint8_t>(Net::RudpChannelId::kControl),
        Net::kRudpFlagAckOnly,
        {0x10})));
}
