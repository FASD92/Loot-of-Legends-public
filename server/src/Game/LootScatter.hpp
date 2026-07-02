#pragma once

#include <cstdint>
#include <vector>

#include "Game/Room.hpp"

namespace Game {
struct ScatterDropPlacement {
    uint32_t sequence{0};
    WorldPosition position{};
};

std::vector<ScatterDropPlacement> buildLootScatter(
    uint32_t scatterSeed,
    uint16_t eligiblePlayerCount,
    WorldPosition origin,
    uint32_t firstSequence);
}  // namespace Game
