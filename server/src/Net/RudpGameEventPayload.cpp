#include "Net/RudpGameEventPayload.hpp"

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

bool isValidPayload(const Net::RudpMonsterDeathGameEventPayload& payload) {
    return payload.roomId != 0 && payload.monsterId != 0;
}

bool isValidPayload(const Net::RudpLootResolvedGameEventPayload& payload) {
    return payload.roomId != 0 && payload.dropId != 0;
}
}  // namespace

namespace Net {
bool serializeRudpMonsterDeathGameEventPayload(
    const RudpMonsterDeathGameEventPayload& payload,
    std::vector<uint8_t>& outPayload) {
    outPayload.clear();
    if (!isValidPayload(payload)) {
        return false;
    }

    outPayload.assign(kRudpMonsterDeathGameEventPayloadSize, 0);
    writeU16BE(
        static_cast<uint16_t>(RudpGameEventType::kMonsterDeath),
        outPayload.data());
    writeU16BE(
        static_cast<uint16_t>(kRudpMonsterDeathGameEventBodySize),
        outPayload.data() + 2);
    writeU32BE(payload.roomId, outPayload.data() + 4);
    writeU32BE(payload.monsterId, outPayload.data() + 8);
    return true;
}

bool parseRudpMonsterDeathGameEventPayload(
    const uint8_t* data,
    size_t size,
    RudpMonsterDeathGameEventPayload& outPayload) {
    outPayload = RudpMonsterDeathGameEventPayload{};
    if (data == nullptr || size != kRudpMonsterDeathGameEventPayloadSize) {
        return false;
    }

    if (readU16BE(data) != static_cast<uint16_t>(RudpGameEventType::kMonsterDeath)) {
        return false;
    }
    if (readU16BE(data + 2) != kRudpMonsterDeathGameEventBodySize) {
        return false;
    }

    const RudpMonsterDeathGameEventPayload parsed{
        readU32BE(data + 4),
        readU32BE(data + 8),
    };
    if (!isValidPayload(parsed)) {
        return false;
    }

    outPayload = parsed;
    return true;
}

bool serializeRudpLootResolvedGameEventPayload(
    const RudpLootResolvedGameEventPayload& payload,
    std::vector<uint8_t>& outPayload) {
    outPayload.clear();
    if (!isValidPayload(payload)) {
        return false;
    }

    outPayload.assign(kRudpLootResolvedGameEventPayloadSize, 0);
    writeU16BE(
        static_cast<uint16_t>(RudpGameEventType::kLootResolved),
        outPayload.data());
    writeU16BE(
        static_cast<uint16_t>(kRudpLootResolvedGameEventBodySize),
        outPayload.data() + 2);
    writeU32BE(payload.roomId, outPayload.data() + 4);
    writeU32BE(payload.dropId, outPayload.data() + 8);
    writeU64BE(payload.winnerSessionId, outPayload.data() + 12);
    writeU32BE(payload.itemId, outPayload.data() + 20);
    writeU16BE(payload.quantity, outPayload.data() + 24);
    return true;
}

bool parseRudpLootResolvedGameEventPayload(
    const uint8_t* data,
    size_t size,
    RudpLootResolvedGameEventPayload& outPayload) {
    outPayload = RudpLootResolvedGameEventPayload{};
    if (data == nullptr || size != kRudpLootResolvedGameEventPayloadSize) {
        return false;
    }

    if (readU16BE(data) != static_cast<uint16_t>(RudpGameEventType::kLootResolved)) {
        return false;
    }
    if (readU16BE(data + 2) != kRudpLootResolvedGameEventBodySize) {
        return false;
    }

    const RudpLootResolvedGameEventPayload parsed{
        readU32BE(data + 4),
        readU32BE(data + 8),
        readU64BE(data + 12),
        readU32BE(data + 20),
        readU16BE(data + 24),
    };
    if (!isValidPayload(parsed)) {
        return false;
    }

    outPayload = parsed;
    return true;
}
}  // namespace Net
