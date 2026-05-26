#include "Net/RudpInputCommandPayload.hpp"

namespace {
uint16_t readU16BE(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) |
        static_cast<uint16_t>(data[1]));
}

int16_t readI16BE(const uint8_t* data) {
    const uint16_t value = readU16BE(data);
    if (value <= 0x7FFFU) {
        return static_cast<int16_t>(value);
    }
    return static_cast<int16_t>(static_cast<int32_t>(value) - 0x10000);
}

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
    case static_cast<uint8_t>(RudpInputCommandOp::kMove):
        if (argLen != 6) {
            return false;
        }
        parsed.op = RudpInputCommandOp::kMove;
        parsed.argValue = 0;
        parsed.move.dirX = readI16BE(data + kRudpInputCommandPrefixSize);
        parsed.move.dirY = readI16BE(data + kRudpInputCommandPrefixSize + 2);
        parsed.move.inputFlags = readU16BE(data + kRudpInputCommandPrefixSize + 4);
        break;
    default:
        return false;
    }

    outPayload = parsed;
    return true;
}
}  // namespace Net
