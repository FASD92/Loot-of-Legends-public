#include "Net/RudpHelloPayload.hpp"

namespace {
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

void writeU64BE(uint64_t value, uint8_t* out) {
    for (int i = 7; i >= 0; --i) {
        out[7 - i] = static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    }
}

uint16_t readU16BE(const uint8_t* data) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(data[0]) << 8) |
        static_cast<uint16_t>(data[1]));
}

uint32_t readU32BE(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
        (static_cast<uint32_t>(data[1]) << 16) |
        (static_cast<uint32_t>(data[2]) << 8) |
        static_cast<uint32_t>(data[3]);
}

uint64_t readU64BE(const uint8_t* data) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<uint64_t>(data[i]);
    }
    return value;
}
}  // namespace

namespace Net {
bool serializeRudpHelloPayload(
    const RudpHelloPayload& payload,
    std::vector<uint8_t>& outPayload) {
    outPayload.assign(kRudpHelloPayloadSize, 0);
    writeU16BE(payload.clientVersion, outPayload.data());
    writeU32BE(payload.clientId, outPayload.data() + 2);
    writeU64BE(payload.sessionId, outPayload.data() + 6);
    return true;
}

bool parseRudpHelloPayload(
    const uint8_t* data,
    size_t size,
    RudpHelloPayload& outPayload) {
    outPayload = RudpHelloPayload{};
    if (data == nullptr || size != kRudpHelloPayloadSize) {
        return false;
    }

    outPayload.clientVersion = readU16BE(data);
    outPayload.clientId = readU32BE(data + 2);
    outPayload.sessionId = readU64BE(data + 6);
    return true;
}
}  // namespace Net
