#include "Net/RudpPacket.hpp"

#include <algorithm>

namespace {
constexpr size_t kVersionOffset = 2;
constexpr size_t kFlagsOffset = 3;
constexpr size_t kHeaderLenOffset = 4;
constexpr size_t kChannelIdOffset = 5;
constexpr size_t kPacketTypeOffset = 6;
constexpr size_t kSequenceOffset = 8;
constexpr size_t kAckOffset = 12;
constexpr size_t kAckBitsOffset = 16;
constexpr size_t kPayloadLenOffset = 20;
constexpr size_t kChecksumOffset = 22;
constexpr size_t kReservedOffset = 26;
constexpr uint32_t kCrc32Polynomial = 0xEDB88320U;
constexpr uint32_t kCrc32Initial = 0xFFFFFFFFU;
constexpr uint32_t kCrc32FinalXor = 0xFFFFFFFFU;
constexpr uint8_t kAllowedFlags = Net::kRudpFlagReliable | Net::kRudpFlagAckOnly;

void writeU16BE(uint16_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[1] = static_cast<uint8_t>(value & 0xFF);
}

void writeU32BE(uint32_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(value & 0xFF);
}

uint16_t readU16BE(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) |
                                 static_cast<uint16_t>(data[1]));
}

uint32_t readU32BE(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

uint32_t checksumForPacketWithZeroChecksum(const uint8_t* data, size_t size) {
    std::vector<uint8_t> copy(data, data + size);
    std::fill(copy.begin() + kChecksumOffset, copy.begin() + kChecksumOffset + 4, 0);
    return Net::computeRudpCrc32(copy.data(), copy.size());
}

uint8_t expectedChannelForPacketType(uint16_t packetType) {
    switch (static_cast<Net::RudpPacketType>(packetType)) {
        case Net::RudpPacketType::kHello:
        case Net::RudpPacketType::kMetaResponse:
        case Net::RudpPacketType::kError:
            return static_cast<uint8_t>(Net::RudpChannelId::kControl);
        case Net::RudpPacketType::kInputCommand:
            return static_cast<uint8_t>(Net::RudpChannelId::kInput);
        case Net::RudpPacketType::kStateSnapshot:
            return static_cast<uint8_t>(Net::RudpChannelId::kSnapshot);
        case Net::RudpPacketType::kBattleStart:
        case Net::RudpPacketType::kBattleStartRoster:
        case Net::RudpPacketType::kGameEvent:
            return static_cast<uint8_t>(Net::RudpChannelId::kEvent);
    }
    return 0;
}
}  // namespace

namespace Net {
uint32_t computeRudpCrc32(const uint8_t* data, size_t size) {
    if (data == nullptr && size > 0) {
        return 0;
    }

    uint32_t crc = kCrc32Initial;
    for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint32_t>(data[i]);
        for (int bit = 0; bit < 8; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ kCrc32Polynomial;
            } else {
                crc >>= 1U;
            }
        }
    }
    return crc ^ kCrc32FinalXor;
}

bool serializeRudpPacket(
    const RudpPacketHeader& header,
    const std::vector<uint8_t>& payload,
    std::vector<uint8_t>& outPacket) {
    if (payload.size() > kMaxRudpPayloadSize) {
        return false;
    }

    outPacket.assign(kRudpHeaderSize + payload.size(), 0);
    outPacket[0] = kRudpMagic0;
    outPacket[1] = kRudpMagic1;
    outPacket[kVersionOffset] = kRudpVersion;
    outPacket[kFlagsOffset] = header.flags;
    outPacket[kHeaderLenOffset] = static_cast<uint8_t>(kRudpHeaderSize);
    outPacket[kChannelIdOffset] = header.channelId;
    writeU16BE(header.packetType, outPacket.data() + kPacketTypeOffset);
    writeU32BE(header.sequence, outPacket.data() + kSequenceOffset);
    writeU32BE(header.ack, outPacket.data() + kAckOffset);
    writeU32BE(header.ackBits, outPacket.data() + kAckBitsOffset);
    writeU16BE(static_cast<uint16_t>(payload.size()), outPacket.data() + kPayloadLenOffset);
    writeU16BE(0, outPacket.data() + kReservedOffset);

    std::copy(payload.begin(), payload.end(), outPacket.begin() + kRudpHeaderSize);

    const uint32_t checksum = computeRudpCrc32(outPacket.data(), outPacket.size());
    writeU32BE(checksum, outPacket.data() + kChecksumOffset);
    return true;
}

bool validateRudpPacketSemantics(const RudpPacketHeader& header) {
    if ((header.flags & ~kAllowedFlags) != 0) {
        return false;
    }
    if ((header.flags & kRudpFlagAckOnly) != 0 && header.payloadLen != 0) {
        return false;
    }

    const uint8_t expectedChannel = expectedChannelForPacketType(header.packetType);
    if (expectedChannel == 0) {
        return false;
    }
    return header.channelId == expectedChannel;
}

bool parseRudpPacket(
    const uint8_t* data,
    size_t size,
    RudpPacketHeader& outHeader,
    std::vector<uint8_t>& outPayload) {
    if (data == nullptr || size < kRudpHeaderSize || size > kMaxRudpPacketSize) {
        return false;
    }

    if (data[0] != kRudpMagic0 || data[1] != kRudpMagic1) {
        return false;
    }
    if (data[kVersionOffset] != kRudpVersion) {
        return false;
    }
    if (data[kHeaderLenOffset] != kRudpHeaderSize) {
        return false;
    }
    if (readU16BE(data + kReservedOffset) != 0) {
        return false;
    }

    const uint16_t payloadLen = readU16BE(data + kPayloadLenOffset);
    if (payloadLen != size - kRudpHeaderSize) {
        return false;
    }

    const uint32_t expectedChecksum = readU32BE(data + kChecksumOffset);
    const uint32_t actualChecksum = checksumForPacketWithZeroChecksum(data, size);
    if (actualChecksum != expectedChecksum) {
        return false;
    }

    RudpPacketHeader parsedHeader;
    parsedHeader.flags = data[kFlagsOffset];
    parsedHeader.channelId = data[kChannelIdOffset];
    parsedHeader.packetType = readU16BE(data + kPacketTypeOffset);
    parsedHeader.sequence = readU32BE(data + kSequenceOffset);
    parsedHeader.ack = readU32BE(data + kAckOffset);
    parsedHeader.ackBits = readU32BE(data + kAckBitsOffset);
    parsedHeader.payloadLen = payloadLen;
    parsedHeader.checksum = expectedChecksum;
    if (!validateRudpPacketSemantics(parsedHeader)) {
        return false;
    }

    outHeader = parsedHeader;
    outPayload.assign(data + kRudpHeaderSize, data + size);
    return true;
}
}  // namespace Net
