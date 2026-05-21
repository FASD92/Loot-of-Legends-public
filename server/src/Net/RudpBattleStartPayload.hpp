#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Net {
constexpr size_t kRudpBattleStartPayloadSize = 20;

struct RudpBattleStartPayload {
    uint32_t roomId{0};
    uint64_t playerASessionId{0};
    uint64_t playerBSessionId{0};
};

bool serializeRudpBattleStartPayload(
    const RudpBattleStartPayload& payload,
    std::vector<uint8_t>& outPayload);

bool parseRudpBattleStartPayload(
    const uint8_t* data,
    size_t size,
    RudpBattleStartPayload& outPayload);
}  // namespace Net
