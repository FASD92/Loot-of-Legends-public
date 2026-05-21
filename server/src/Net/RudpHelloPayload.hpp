#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Net {
constexpr size_t kRudpHelloPayloadSize = 14;

struct RudpHelloPayload {
    uint16_t clientVersion{0};
    uint32_t clientId{0};
    uint64_t sessionId{0};
};

bool serializeRudpHelloPayload(
    const RudpHelloPayload& payload,
    std::vector<uint8_t>& outPayload);

bool parseRudpHelloPayload(
    const uint8_t* data,
    size_t size,
    RudpHelloPayload& outPayload);
}  // namespace Net
