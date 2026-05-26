#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "Core/ClientConnection.hpp"

namespace {
Core::ClientConnection makeConnection() {
    return Core::ClientConnection(10, 42, "127.0.0.1:50000", Util::TimePoint{});
}
}  // namespace

TEST(ClientConnectionTests, OutboundBufferPreservesOrderAndFlushOffset) {
    Core::ClientConnection connection = makeConnection();
    const std::array<uint8_t, 3> first{0x01, 0x02, 0x03};
    const std::array<uint8_t, 2> second{0x04, 0x05};

    ASSERT_TRUE(connection.enqueueOutbound(first.data(), first.size(), 8));
    ASSERT_EQ(connection.pendingOutboundSize(), 3U);
    EXPECT_EQ(connection.pendingOutboundData()[0], 0x01);

    connection.consumeOutboundBytes(2);
    ASSERT_EQ(connection.pendingOutboundSize(), 1U);
    EXPECT_EQ(connection.pendingOutboundData()[0], 0x03);

    ASSERT_TRUE(connection.enqueueOutbound(second.data(), second.size(), 8));
    ASSERT_EQ(connection.pendingOutboundSize(), 3U);
    EXPECT_EQ(connection.pendingOutboundData()[0], 0x03);
    EXPECT_EQ(connection.pendingOutboundData()[1], 0x04);
    EXPECT_EQ(connection.pendingOutboundData()[2], 0x05);

    connection.consumeOutboundBytes(3);
    EXPECT_FALSE(connection.hasPendingOutbound());
    EXPECT_EQ(connection.pendingOutboundData(), nullptr);
}

TEST(ClientConnectionTests, OutboundBufferRejectsCapOverflowWithoutDroppingExistingBytes) {
    Core::ClientConnection connection = makeConnection();
    const std::array<uint8_t, 3> first{0x10, 0x11, 0x12};
    const std::array<uint8_t, 2> second{0x13, 0x14};

    ASSERT_TRUE(connection.enqueueOutbound(first.data(), first.size(), 3));
    EXPECT_FALSE(connection.enqueueOutbound(second.data(), second.size(), 3));

    ASSERT_EQ(connection.pendingOutboundSize(), 3U);
    EXPECT_EQ(connection.pendingOutboundData()[0], 0x10);
    EXPECT_EQ(connection.pendingOutboundData()[1], 0x11);
    EXPECT_EQ(connection.pendingOutboundData()[2], 0x12);
}
