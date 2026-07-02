#include "Net/RudpBattleStartRosterPayload.hpp"

#include <utility>

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
    for (size_t i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((value >> ((7 - i) * 8)) & 0xFF);
    }
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

uint64_t readU64BE(const uint8_t* data) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<uint64_t>(data[i]);
    }
    return value;
}

bool isValidPayload(const Net::RudpBattleStartRosterPayload& payload) {
    if (payload.roomId == 0 ||
        payload.playerSessionIds.size() < Net::kRudpBattleStartRosterMinPlayers ||
        payload.playerSessionIds.size() > Net::kRudpBattleStartRosterMaxPlayers) {
        return false;
    }

    for (size_t i = 0; i < payload.playerSessionIds.size(); ++i) {
        if (payload.playerSessionIds[i] == 0) {
            return false;
        }
        for (size_t j = i + 1; j < payload.playerSessionIds.size(); ++j) {
            if (payload.playerSessionIds[i] == payload.playerSessionIds[j]) {
                return false;
            }
        }
    }

    return true;
}
}  // namespace

namespace Net {
size_t rudpBattleStartRosterPayloadSize(size_t playerCount) {
    return kRudpBattleStartRosterFixedPayloadSize +
        (playerCount * kRudpBattleStartRosterEntrySize);
}

bool serializeRudpBattleStartRosterPayload(
    const RudpBattleStartRosterPayload& payload,
    std::vector<uint8_t>& outPayload) {
    outPayload.clear();
    if (!isValidPayload(payload)) {
        return false;
    }

    outPayload.assign(rudpBattleStartRosterPayloadSize(payload.playerSessionIds.size()), 0);
    writeU32BE(payload.roomId, outPayload.data());
    writeU16BE(
        static_cast<uint16_t>(payload.playerSessionIds.size()),
        outPayload.data() + 4);

    uint8_t* entries = outPayload.data() + kRudpBattleStartRosterFixedPayloadSize;
    for (size_t i = 0; i < payload.playerSessionIds.size(); ++i) {
        writeU64BE(payload.playerSessionIds[i], entries + (i * kRudpBattleStartRosterEntrySize));
    }
    return true;
}

bool parseRudpBattleStartRosterPayload(
    const uint8_t* data,
    size_t size,
    RudpBattleStartRosterPayload& outPayload) {
    outPayload = RudpBattleStartRosterPayload{};
    if (data == nullptr || size < kRudpBattleStartRosterFixedPayloadSize) {
        return false;
    }

    RudpBattleStartRosterPayload parsed;
    parsed.roomId = readU32BE(data);
    const uint16_t playerCount = readU16BE(data + 4);
    if (size != rudpBattleStartRosterPayloadSize(playerCount)) {
        return false;
    }

    parsed.playerSessionIds.reserve(playerCount);
    const uint8_t* entries = data + kRudpBattleStartRosterFixedPayloadSize;
    for (uint16_t i = 0; i < playerCount; ++i) {
        parsed.playerSessionIds.push_back(readU64BE(entries + (i * kRudpBattleStartRosterEntrySize)));
    }

    if (!isValidPayload(parsed)) {
        return false;
    }

    outPayload = std::move(parsed);
    return true;
}
}  // namespace Net
