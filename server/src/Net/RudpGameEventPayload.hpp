#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Net {
constexpr size_t kRudpGameEventFrameHeaderSize = 4;
constexpr size_t kRudpMonsterDeathGameEventBodySize = 8;
constexpr size_t kRudpMonsterDeathGameEventPayloadSize =
    kRudpGameEventFrameHeaderSize + kRudpMonsterDeathGameEventBodySize;
constexpr size_t kRudpLootResolvedGameEventBodySize = 22;
constexpr size_t kRudpLootResolvedGameEventPayloadSize =
    kRudpGameEventFrameHeaderSize + kRudpLootResolvedGameEventBodySize;

enum class RudpGameEventType : uint16_t {
    kMonsterDeath = 0x0001,
    kLootResolved = 0x0002,
};

struct RudpMonsterDeathGameEventPayload {
    uint32_t roomId{0};
    uint32_t monsterId{0};
};

struct RudpLootResolvedGameEventPayload {
    uint32_t roomId{0};
    uint32_t dropId{0};
    uint64_t winnerSessionId{0};
    uint32_t itemId{0};
    uint16_t quantity{0};
};

bool serializeRudpMonsterDeathGameEventPayload(
    const RudpMonsterDeathGameEventPayload& payload,
    std::vector<uint8_t>& outPayload);

bool parseRudpMonsterDeathGameEventPayload(
    const uint8_t* data,
    size_t size,
    RudpMonsterDeathGameEventPayload& outPayload);

bool serializeRudpLootResolvedGameEventPayload(
    const RudpLootResolvedGameEventPayload& payload,
    std::vector<uint8_t>& outPayload);

bool parseRudpLootResolvedGameEventPayload(
    const uint8_t* data,
    size_t size,
    RudpLootResolvedGameEventPayload& outPayload);
}  // namespace Net
