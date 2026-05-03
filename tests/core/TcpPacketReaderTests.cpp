#include <gtest/gtest.h>

#include <array>
#include <vector>

#include "Net/TcpPacket.hpp"
#include "Net/TcpPacketReader.hpp"

namespace {
void writeU16BE(uint16_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[1] = static_cast<uint8_t>(value & 0xFF);
}
}  // namespace

TEST(TcpPacketReaderTests, ReassemblesPacketFromPartialReads) {
    Net::TcpPacketReader reader;
    std::array<uint8_t, Net::kWelcomePacketSize> packet{};
    ASSERT_TRUE(Net::serializeWelcomePacket(11, packet));

    ASSERT_TRUE(reader.appendBytes(packet.data(), 3));

    std::vector<uint8_t> framedPacket;
    EXPECT_EQ(reader.tryReadPacket(framedPacket), Net::TcpPacketReadResult::kNeedMoreData);

    ASSERT_TRUE(reader.appendBytes(packet.data() + 3, packet.size() - 3));
    ASSERT_EQ(reader.tryReadPacket(framedPacket), Net::TcpPacketReadResult::kPacketReady);
    EXPECT_EQ(framedPacket.size(), packet.size());

    Net::TcpPacketHeader header;
    uint64_t sessionId = 0;
    ASSERT_TRUE(Net::parseWelcomePacket(framedPacket.data(), framedPacket.size(), header, sessionId));
    EXPECT_EQ(sessionId, 11u);
    EXPECT_EQ(reader.bufferedSize(), 0u);
}

TEST(TcpPacketReaderTests, ExtractsMultiplePacketsInOrderFromSingleBuffer) {
    Net::TcpPacketReader reader;
    std::array<uint8_t, Net::kWelcomePacketSize> first{};
    std::array<uint8_t, Net::kWelcomePacketSize> second{};
    ASSERT_TRUE(Net::serializeWelcomePacket(101, first));
    ASSERT_TRUE(Net::serializeWelcomePacket(202, second));

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), first.begin(), first.end());
    combined.insert(combined.end(), second.begin(), second.end());
    ASSERT_TRUE(reader.appendBytes(combined.data(), combined.size()));

    std::vector<uint8_t> framedPacket;
    ASSERT_EQ(reader.tryReadPacket(framedPacket), Net::TcpPacketReadResult::kPacketReady);
    Net::TcpPacketHeader firstHeader;
    uint64_t firstSessionId = 0;
    ASSERT_TRUE(Net::parseWelcomePacket(framedPacket.data(), framedPacket.size(), firstHeader, firstSessionId));
    EXPECT_EQ(firstSessionId, 101u);

    ASSERT_EQ(reader.tryReadPacket(framedPacket), Net::TcpPacketReadResult::kPacketReady);
    Net::TcpPacketHeader secondHeader;
    uint64_t secondSessionId = 0;
    ASSERT_TRUE(Net::parseWelcomePacket(framedPacket.data(), framedPacket.size(), secondHeader, secondSessionId));
    EXPECT_EQ(secondSessionId, 202u);

    EXPECT_EQ(reader.bufferedSize(), 0u);
}

TEST(TcpPacketReaderTests, RejectsPacketWithInvalidSize) {
    Net::TcpPacketReader reader;
    std::array<uint8_t, Net::kTcpHeaderSize> invalidHeader{};
    writeU16BE(3, invalidHeader.data());
    writeU16BE(static_cast<uint16_t>(Net::TcpPacketType::kWelcome), invalidHeader.data() + 2);

    ASSERT_TRUE(reader.appendBytes(invalidHeader.data(), invalidHeader.size()));

    std::vector<uint8_t> framedPacket;
    EXPECT_EQ(reader.tryReadPacket(framedPacket), Net::TcpPacketReadResult::kInvalidPacket);
}
