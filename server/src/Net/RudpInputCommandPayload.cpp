#include "Net/RudpInputCommandPayload.hpp"

namespace {
uint32_t readU32BE(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
        (static_cast<uint32_t>(data[1]) << 16) |
        (static_cast<uint32_t>(data[2]) << 8) |
        static_cast<uint32_t>(data[3]);
}
}  // namespace

namespace Net {
bool parseRudpInputCommandPayload(
    const uint8_t* data,
    size_t size,
    RudpInputCommandPayload& outPayload) {
    outPayload = RudpInputCommandPayload{};
    if (data == nullptr || size < kRudpInputCommandPrefixSize) {
        return false;
    }

    const uint8_t rawOp = data[8];
    const uint8_t argLen = data[9];
    if (size != kRudpInputCommandPrefixSize + static_cast<size_t>(argLen)) {
        return false;
    }

    RudpInputCommandPayload parsed;
    parsed.playerId = readU32BE(data);
    parsed.cmdSeq = readU32BE(data + 4);

    switch (rawOp) {
    case static_cast<uint8_t>(RudpInputCommandOp::kReady):
        if (argLen != 0) {
            return false;
        }
        parsed.op = RudpInputCommandOp::kReady;
        parsed.argValue = 0;
        break;
    case static_cast<uint8_t>(RudpInputCommandOp::kMonsterDeath):
        if (argLen != 4) {
            return false;
        }
        parsed.op = RudpInputCommandOp::kMonsterDeath;
        parsed.argValue = readU32BE(data + kRudpInputCommandPrefixSize);
        break;
    case static_cast<uint8_t>(RudpInputCommandOp::kClickLoot):
        if (argLen != 4) {
            return false;
        }
        parsed.op = RudpInputCommandOp::kClickLoot;
        parsed.argValue = readU32BE(data + kRudpInputCommandPrefixSize);
        break;
    default:
        return false;
    }

    outPayload = parsed;
    return true;
}
}  // namespace Net
