#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace Net {
constexpr size_t kRudpBattleStartRosterCountFieldSize = 2;
constexpr size_t kRudpBattleStartRosterEntrySize = 8;
constexpr size_t kRudpBattleStartRosterFixedPayloadSize = 6;
constexpr uint16_t kRudpBattleStartRosterMinPlayers = 2;
constexpr uint16_t kRudpBattleStartRosterMaxPlayers = 10;

struct RudpBattleStartRosterPayload {
    uint32_t roomId{0};
    std::vector<uint64_t> playerSessionIds{};
};

size_t rudpBattleStartRosterPayloadSize(size_t playerCount);

bool serializeRudpBattleStartRosterPayload(
    const RudpBattleStartRosterPayload& payload,
    std::vector<uint8_t>& outPayload);

bool parseRudpBattleStartRosterPayload(
    const uint8_t* data,
    size_t size,
    RudpBattleStartRosterPayload& outPayload);
}  // namespace Net
