#include "Net/RudpBattleStartPayload.hpp"

namespace {
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

bool isValidPayload(const Net::RudpBattleStartPayload& payload) {
    return payload.roomId != 0 &&
        payload.playerASessionId != 0 &&
        payload.playerBSessionId != 0 &&
        payload.playerASessionId != payload.playerBSessionId;
}
}  // namespace

namespace Net {
bool serializeRudpBattleStartPayload(
    const RudpBattleStartPayload& payload,
    std::vector<uint8_t>& outPayload) {
    outPayload.clear();
    if (!isValidPayload(payload)) {
        return false;
    }

    outPayload.assign(kRudpBattleStartPayloadSize, 0);
    writeU32BE(payload.roomId, outPayload.data());
    writeU64BE(payload.playerASessionId, outPayload.data() + 4);
    writeU64BE(payload.playerBSessionId, outPayload.data() + 12);
    return true;
}

bool parseRudpBattleStartPayload(
    const uint8_t* data,
    size_t size,
    RudpBattleStartPayload& outPayload) {
    outPayload = RudpBattleStartPayload{};
    if (data == nullptr || size != kRudpBattleStartPayloadSize) {
        return false;
    }

    const RudpBattleStartPayload parsed{
        readU32BE(data),
        readU64BE(data + 4),
        readU64BE(data + 12),
    };
    if (!isValidPayload(parsed)) {
        return false;
    }

    outPayload = parsed;
    return true;
}
}  // namespace Net
