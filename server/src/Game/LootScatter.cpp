#include "Game/LootScatter.hpp"

#include <cstdint>

namespace Game {
namespace {
constexpr uint32_t kFallbackSeed = 0x9E3779B9U;

uint32_t nextRandom(uint32_t& state) {
    if (state == 0U) {
        state = kFallbackSeed;
    }
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}

uint32_t nextRange(uint32_t& state, uint32_t span) {
    return span == 0U ? 0U : nextRandom(state) % span;
}

int32_t clampArena(int64_t value) {
    if (value < Room::kArenaMinPosition) {
        return Room::kArenaMinPosition;
    }
    if (value > Room::kArenaMaxPosition) {
        return Room::kArenaMaxPosition;
    }
    return static_cast<int32_t>(value);
}

int32_t nextOffset(uint32_t& state) {
    constexpr int32_t kMaxOffset = Room::kLootRange * 2;
    constexpr uint32_t kOffsetSpan = static_cast<uint32_t>((kMaxOffset * 2) + 1);
    return static_cast<int32_t>(nextRange(state, kOffsetSpan)) - kMaxOffset;
}
}  // namespace

std::vector<ScatterDropPlacement> buildLootScatter(
    uint32_t scatterSeed,
    uint16_t eligiblePlayerCount,
    WorldPosition origin,
    uint32_t firstSequence) {
    if (eligiblePlayerCount < Room::kMinPlayersToStart) {
        return {};
    }

    uint32_t state = scatterSeed ^
                     (static_cast<uint32_t>(eligiblePlayerCount) * 0x85EBCA6BU) ^
                     (static_cast<uint32_t>(origin.x) * 0xC2B2AE35U) ^
                     static_cast<uint32_t>(origin.y) ^
                     firstSequence;
    const uint16_t dropCount =
        static_cast<uint16_t>(1U + nextRange(state, eligiblePlayerCount - 1U));

    std::vector<ScatterDropPlacement> placements;
    placements.reserve(dropCount);
    for (uint16_t index = 0; index < dropCount; ++index) {
        const int64_t x = static_cast<int64_t>(origin.x) + nextOffset(state);
        const int64_t y = static_cast<int64_t>(origin.y) + nextOffset(state);
        placements.push_back(
            ScatterDropPlacement{
                static_cast<uint32_t>(firstSequence + index),
                WorldPosition{clampArena(x), clampArena(y)}});
    }
    return placements;
}
}  // namespace Game
