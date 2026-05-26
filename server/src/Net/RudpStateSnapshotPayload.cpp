#include "Net/RudpStateSnapshotPayload.hpp"

#include <limits>
#include <unordered_set>

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

void writeI32BE(int32_t value, uint8_t* out) {
    writeU32BE(static_cast<uint32_t>(value), out);
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

int32_t readI32BE(const uint8_t* data) {
    const uint32_t raw = readU32BE(data);
    if (raw <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max())) {
        return static_cast<int32_t>(raw);
    }

    const uint32_t magnitude = (~raw) + 1U;
    if (magnitude == 0x80000000U) {
        return std::numeric_limits<int32_t>::min();
    }
    return -static_cast<int32_t>(magnitude);
}

uint64_t readU64BE(const uint8_t* data) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<uint64_t>(data[i]);
    }
    return value;
}

bool hasValidHeaderFields(const Net::RudpStateSnapshotPayload& payload) {
    return payload.snapshotVersion == Net::kRudpStateSnapshotVersion &&
        payload.snapshotKind == Net::kRudpStateSnapshotKindRoomMovementPlayers &&
        payload.roomId != 0;
}

bool hasValidPlayerList(
    const std::vector<Net::RudpStateSnapshotPlayer>& players) {
    if (players.size() > Net::kMaxRudpStateSnapshotPlayersPerPayload) {
        return false;
    }

    std::unordered_set<uint64_t> seenSessionIds;
    seenSessionIds.reserve(players.size());
    for (const Net::RudpStateSnapshotPlayer& player : players) {
        if (player.sessionId == 0) {
            return false;
        }
        if (!seenSessionIds.insert(player.sessionId).second) {
            return false;
        }
    }
    return true;
}
}  // namespace

namespace Net {
bool serializeRudpStateSnapshotPayload(
    const RudpStateSnapshotPayload& payload,
    std::vector<uint8_t>& outPayload) {
    outPayload.clear();
    if (!hasValidHeaderFields(payload) || !hasValidPlayerList(payload.players)) {
        return false;
    }

    const size_t payloadSize = kRudpStateSnapshotFixedPayloadSize +
        (payload.players.size() * kRudpStateSnapshotPlayerEntrySize);
    if (payloadSize > kMaxRudpPayloadSize) {
        return false;
    }

    outPayload.assign(payloadSize, 0);
    outPayload[0] = payload.snapshotVersion;
    outPayload[1] = payload.snapshotKind;
    writeU32BE(payload.roomId, outPayload.data() + 2);
    writeU32BE(payload.serverTick, outPayload.data() + 6);
    writeU16BE(static_cast<uint16_t>(payload.players.size()), outPayload.data() + 10);

    size_t offset = kRudpStateSnapshotFixedPayloadSize;
    for (const RudpStateSnapshotPlayer& player : payload.players) {
        writeU64BE(player.sessionId, outPayload.data() + offset);
        writeI32BE(player.posX, outPayload.data() + offset + 8);
        writeI32BE(player.posY, outPayload.data() + offset + 12);
        offset += kRudpStateSnapshotPlayerEntrySize;
    }
    return true;
}

bool parseRudpStateSnapshotPayload(
    const uint8_t* data,
    size_t size,
    RudpStateSnapshotPayload& outPayload) {
    outPayload = RudpStateSnapshotPayload{};
    if (data == nullptr ||
        size < kRudpStateSnapshotFixedPayloadSize ||
        size > kMaxRudpPayloadSize) {
        return false;
    }

    const uint16_t playerCount = readU16BE(data + 10);
    if (playerCount > kMaxRudpStateSnapshotPlayersPerPayload) {
        return false;
    }

    const size_t expectedSize = kRudpStateSnapshotFixedPayloadSize +
        (static_cast<size_t>(playerCount) * kRudpStateSnapshotPlayerEntrySize);
    if (size != expectedSize) {
        return false;
    }

    RudpStateSnapshotPayload parsed;
    parsed.snapshotVersion = data[0];
    parsed.snapshotKind = data[1];
    parsed.roomId = readU32BE(data + 2);
    parsed.serverTick = readU32BE(data + 6);
    parsed.players.reserve(playerCount);

    size_t offset = kRudpStateSnapshotFixedPayloadSize;
    for (uint16_t i = 0; i < playerCount; ++i) {
        parsed.players.push_back(RudpStateSnapshotPlayer{
            readU64BE(data + offset),
            readI32BE(data + offset + 8),
            readI32BE(data + offset + 12),
        });
        offset += kRudpStateSnapshotPlayerEntrySize;
    }

    if (!hasValidHeaderFields(parsed) || !hasValidPlayerList(parsed.players)) {
        return false;
    }

    outPayload = parsed;
    return true;
}
}  // namespace Net
