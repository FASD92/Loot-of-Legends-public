#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Net/RudpPacket.hpp"

namespace Net {
constexpr uint8_t kRudpStateSnapshotVersion = 1;
constexpr uint8_t kRudpStateSnapshotKindRoomMovementPlayers = 1;
constexpr size_t kRudpStateSnapshotFixedPayloadSize = 12;
constexpr size_t kRudpStateSnapshotPlayerEntrySize = 16;
constexpr size_t kMaxRudpStateSnapshotPlayersPerPayload =
    (kMaxRudpPayloadSize - kRudpStateSnapshotFixedPayloadSize) /
    kRudpStateSnapshotPlayerEntrySize;

struct RudpStateSnapshotPlayer {
    uint64_t sessionId{0};
    int32_t posX{0};
    int32_t posY{0};
};

struct RudpStateSnapshotPayload {
    uint8_t snapshotVersion{kRudpStateSnapshotVersion};
    uint8_t snapshotKind{kRudpStateSnapshotKindRoomMovementPlayers};
    uint32_t roomId{0};
    uint32_t serverTick{0};
    std::vector<RudpStateSnapshotPlayer> players{};
};

bool serializeRudpStateSnapshotPayload(
    const RudpStateSnapshotPayload& payload,
    std::vector<uint8_t>& outPayload);

bool parseRudpStateSnapshotPayload(
    const uint8_t* data,
    size_t size,
    RudpStateSnapshotPayload& outPayload);
}  // namespace Net
