#pragma once

#include <cstddef>
#include <cstdint>

namespace Net {
constexpr size_t kRudpInputCommandPrefixSize = 10;

enum class RudpInputCommandOp : uint8_t {
    kReady = 0x01,
    kMonsterDeath = 0x02,
    kClickLoot = 0x03,
    kMove = 0x04,
};

struct RudpInputCommandMoveArgs {
    int16_t dirX{0};
    int16_t dirY{0};
    uint16_t inputFlags{0};
};

struct RudpInputCommandPayload {
    uint32_t playerId{0};
    uint32_t cmdSeq{0};
    RudpInputCommandOp op{RudpInputCommandOp::kReady};
    uint32_t argValue{0};
    RudpInputCommandMoveArgs move{};
};

bool parseRudpInputCommandPayload(
    const uint8_t* data,
    size_t size,
    RudpInputCommandPayload& outPayload);
}  // namespace Net
