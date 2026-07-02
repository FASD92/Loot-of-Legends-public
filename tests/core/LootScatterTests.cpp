#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

#include "Game/LootScatter.hpp"
#include "Game/Room.hpp"

namespace {
bool positionIsInsideArena(const Game::WorldPosition& position) {
    return position.x >= Game::Room::kArenaMinPosition &&
           position.x <= Game::Room::kArenaMaxPosition &&
           position.y >= Game::Room::kArenaMinPosition &&
           position.y <= Game::Room::kArenaMaxPosition;
}

int64_t absDelta(int32_t lhs, int32_t rhs) {
    const int64_t delta = static_cast<int64_t>(lhs) - static_cast<int64_t>(rhs);
    return delta < 0 ? -delta : delta;
}
}  // namespace

TEST(LootScatterTests, BuildLootScatterReturnsOneToEligibleMinusOneDrops) {
    const std::vector<Game::ScatterDropPlacement> placements =
        Game::buildLootScatter(12345, 4, Game::WorldPosition{0, 0}, 77);

    ASSERT_GE(placements.size(), 1u);
    ASSERT_LE(placements.size(), 3u);
    for (std::size_t index = 0; index < placements.size(); ++index) {
        EXPECT_EQ(placements[index].sequence, 77u + static_cast<uint32_t>(index));
        EXPECT_TRUE(positionIsInsideArena(placements[index].position));
    }
}

TEST(LootScatterTests, BuildLootScatterIsDeterministicForSameInputs) {
    const std::vector<Game::ScatterDropPlacement> first =
        Game::buildLootScatter(54321, 10, Game::WorldPosition{3000, -2000}, 12);
    const std::vector<Game::ScatterDropPlacement> second =
        Game::buildLootScatter(54321, 10, Game::WorldPosition{3000, -2000}, 12);

    ASSERT_EQ(first.size(), second.size());
    for (std::size_t index = 0; index < first.size(); ++index) {
        EXPECT_EQ(first[index].sequence, second[index].sequence);
        EXPECT_EQ(first[index].position.x, second[index].position.x);
        EXPECT_EQ(first[index].position.y, second[index].position.y);
    }
}

TEST(LootScatterTests, BuildLootScatterStaysNearMonsterDeathOrigin) {
    constexpr Game::WorldPosition kMonsterDeathOrigin{3000, -2000};
    constexpr int64_t kExpectedMaxOffset = Game::Room::kLootRange * 2;

    const std::vector<Game::ScatterDropPlacement> placements =
        Game::buildLootScatter(54321, 10, kMonsterDeathOrigin, 12);

    ASSERT_FALSE(placements.empty());
    for (const Game::ScatterDropPlacement& placement : placements) {
        EXPECT_LE(absDelta(placement.position.x, kMonsterDeathOrigin.x), kExpectedMaxOffset);
        EXPECT_LE(absDelta(placement.position.y, kMonsterDeathOrigin.y), kExpectedMaxOffset);
        EXPECT_TRUE(positionIsInsideArena(placement.position));
    }
}

TEST(LootScatterTests, BuildLootScatterRejectsTooFewEligiblePlayers) {
    EXPECT_TRUE(
        Game::buildLootScatter(1, 0, Game::WorldPosition{0, 0}, 1).empty());
    EXPECT_TRUE(
        Game::buildLootScatter(1, 1, Game::WorldPosition{0, 0}, 1).empty());
}

TEST(LootScatterTests, BuildLootScatterClampsEdgeOriginToArenaBounds) {
    const std::vector<Game::ScatterDropPlacement> placements =
        Game::buildLootScatter(
            999,
            10,
            Game::WorldPosition{Game::Room::kArenaMaxPosition, Game::Room::kArenaMaxPosition},
            200);

    ASSERT_FALSE(placements.empty());
    for (const Game::ScatterDropPlacement& placement : placements) {
        EXPECT_TRUE(positionIsInsideArena(placement.position));
    }
}
