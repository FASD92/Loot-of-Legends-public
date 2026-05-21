#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Net {
constexpr size_t kRudpHeaderSize = 28;
constexpr size_t kMaxRudpPacketSize = 1200;
constexpr size_t kMaxRudpPayloadSize = kMaxRudpPacketSize - kRudpHeaderSize;
constexpr uint8_t kRudpVersion = 0x01;
constexpr uint8_t kRudpMagic0 = 0x4C;   // 'L'
constexpr uint8_t kRudpMagic1 = 0x4F;   // 'O'

constexpr uint8_t kRudpFlagReliable = 0x01;
constexpr uint8_t kRudpFlagAckOnly = 0x02;
constexpr uint8_t kRudpFlagFragmented = 0x04;
constexpr uint8_t kRudpFlagEncrypted = 0x08;

enum class RudpChannelId : uint8_t {
    kControl = 0x01,
    kInput = 0x02,
    kSnapshot = 0x03,
    kEvent = 0x04,
};

enum class RudpPacketType : uint16_t {
    kHello = 0x1001,
    kInputCommand = 0x1002,
    kBattleStart = 0x1003,
    kStateSnapshot = 0x1004,
    kGameEvent = 0x1005,
    kMetaResponse = 0x1006,
    kError = 0x2001,
};

struct RudpPacketHeader {
    uint8_t flags{0};
    uint8_t channelId{0};
    uint16_t packetType{0};
    uint32_t sequence{0};
    uint32_t ack{0};
    uint32_t ackBits{0};
    uint16_t payloadLen{0};
    uint32_t checksum{0};
};

uint32_t computeRudpCrc32(const uint8_t* data, size_t size);

bool serializeRudpPacket(
    const RudpPacketHeader& header,
    const std::vector<uint8_t>& payload,
    std::vector<uint8_t>& outPacket);

bool validateRudpPacketSemantics(const RudpPacketHeader& header);

bool parseRudpPacket(
    const uint8_t* data,
    size_t size,
    RudpPacketHeader& outHeader,
    std::vector<uint8_t>& outPayload);
}  // namespace Net
