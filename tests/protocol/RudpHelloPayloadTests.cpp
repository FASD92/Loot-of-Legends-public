#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "Net/RudpHelloPayload.hpp"

TEST(RudpHelloPayloadTests, SerializeAndParseValidPayload) {
    const Net::RudpHelloPayload payload{7, 0x01020304U, 0x0102030405060708ULL};

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(Net::serializeRudpHelloPayload(payload, bytes));

    ASSERT_EQ(bytes.size(), Net::kRudpHelloPayloadSize);
    EXPECT_EQ(bytes[0], 0x00);
    EXPECT_EQ(bytes[1], 0x07);
    EXPECT_EQ(bytes[2], 0x01);
    EXPECT_EQ(bytes[3], 0x02);
    EXPECT_EQ(bytes[4], 0x03);
    EXPECT_EQ(bytes[5], 0x04);
    EXPECT_EQ(bytes[6], 0x01);
    EXPECT_EQ(bytes[13], 0x08);

    Net::RudpHelloPayload parsed;
    ASSERT_TRUE(Net::parseRudpHelloPayload(bytes.data(), bytes.size(), parsed));
    EXPECT_EQ(parsed.clientVersion, payload.clientVersion);
    EXPECT_EQ(parsed.clientId, payload.clientId);
    EXPECT_EQ(parsed.sessionId, payload.sessionId);
}

TEST(RudpHelloPayloadTests, RejectsInvalidPayloadSize) {
    std::vector<uint8_t> tooShort(Net::kRudpHelloPayloadSize - 1, 0);
    std::vector<uint8_t> tooLong(Net::kRudpHelloPayloadSize + 1, 0);
    Net::RudpHelloPayload parsed;

    EXPECT_FALSE(Net::parseRudpHelloPayload(tooShort.data(), tooShort.size(), parsed));
    EXPECT_FALSE(Net::parseRudpHelloPayload(tooLong.data(), tooLong.size(), parsed));
    EXPECT_FALSE(Net::parseRudpHelloPayload(nullptr, Net::kRudpHelloPayloadSize, parsed));
}
