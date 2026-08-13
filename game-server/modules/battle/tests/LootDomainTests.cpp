#include <lol/battle/LootApi.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

namespace {

using lol::battle::DropId;
using lol::battle::generateDrops;
using lol::battle::ItemId;
using lol::battle::RelicCatalog;
using lol::battle::RelicDrop;
using lol::battle::RelicRuleset;
using lol::shared::BattleInstanceId;
using lol::shared::RoomId;

constexpr std::int32_t kArenaBound = 10000;

RoomId room(std::uint64_t value) { return RoomId{value}; }
BattleInstanceId battle(std::uint64_t value) { return BattleInstanceId{value}; }

bool claimAdmissionRulesetMatchesFrozenContract() {
  return RelicRuleset::claimRatePerSecond == 8 && RelicRuleset::claimBurst == 4;
}

bool catalogIsFrozenImmutableV1Snapshot() {
  const auto catalog = RelicCatalog::v1Snapshot();
  if (RelicCatalog::version != 1 || catalog.size() != 2) {
    return false;
  }
  const auto items = catalog.items();
  if (items.size() != 2 || items[0].itemId != ItemId{1} ||
      items[0].unitValue != 100 || items[1].itemId != ItemId{2} ||
      items[1].unitValue != 300) {
    return false;
  }
  const auto common = catalog.unitValueOf(ItemId{1});
  const auto rare = catalog.unitValueOf(ItemId{2});
  return common.has_value() && *common == 100 && rare.has_value() &&
         *rare == 300 && !catalog.unitValueOf(ItemId{99}).has_value();
}

bool generatedDropCountsAndQuantities(const std::vector<RelicDrop> &drops,
                                      std::uint64_t rareCount,
                                      std::uint64_t commonCount) {
  std::uint64_t rare = 0;
  std::uint64_t common = 0;
  for (const auto &drop : drops) {
    if (drop.itemId == ItemId{2}) {
      ++rare;
    } else if (drop.itemId == ItemId{1}) {
      ++common;
    } else {
      return false;
    }
    if (drop.quantity != 1) {
      return false;
    }
  }
  return rare == rareCount && common == commonCount;
}

bool compositionForTwoCaptured() {
  const auto drops =
      generateDrops(room(7), battle(1), 1, 2, RelicCatalog::v1Snapshot());
  if (!drops.has_value() || drops->size() != 2) {
    return false;
  }
  // Rare first, followed by N-1 Common.
  if ((*drops)[0].itemId != ItemId{2} || (*drops)[1].itemId != ItemId{1}) {
    return false;
  }
  return generatedDropCountsAndQuantities(*drops, 1, 1);
}

bool compositionForTenCaptured() {
  const auto drops =
      generateDrops(room(7), battle(1), 1, 10, RelicCatalog::v1Snapshot());
  if (!drops.has_value() || drops->size() != 10 ||
      (*drops)[0].itemId != ItemId{2}) {
    return false;
  }
  for (std::size_t index = 1; index < drops->size(); ++index) {
    if ((*drops)[index].itemId != ItemId{1}) {
      return false;
    }
  }
  return generatedDropCountsAndQuantities(*drops, 1, 9);
}

bool sameCompleteInputYieldsEqualOutput() {
  const auto first =
      generateDrops(room(7), battle(1), 1, 6, RelicCatalog::v1Snapshot());
  const auto second =
      generateDrops(room(7), battle(1), 1, 6, RelicCatalog::v1Snapshot());
  return first.has_value() && first == second;
}

bool dropMatchesGolden(const RelicDrop &drop, std::uint64_t dropId,
                       std::int32_t x, std::int32_t y) {
  return drop.dropId == DropId{dropId} && drop.position.xMillimeter == x &&
         drop.position.yMillimeter == y;
}

bool goldenVectorMatchesFrozenLiteral() {
  const auto drops =
      generateDrops(room(7), battle(1), 1, 10, RelicCatalog::v1Snapshot());
  if (!drops.has_value() || drops->size() != 10) {
    return false;
  }
  constexpr std::int32_t golden[10][2] = {
      {-3360, 8722}, {-414, 819},   {2575, -9442}, {-9848, 52},   {-5343, 4533},
      {493, -3150},  {-2894, 9481}, {805, 5914},   {2666, -4400}, {-4216, 7418},
  };
  for (std::size_t index = 0; index < drops->size(); ++index) {
    if (!dropMatchesGolden((*drops)[index], index + 1, golden[index][0],
                           golden[index][1])) {
      return false;
    }
  }
  return true;
}

bool positionsStayInsideArenaBounds() {
  for (const std::uint32_t count : {2u, 10u}) {
    const auto drops =
        generateDrops(room(7), battle(1), 1, count, RelicCatalog::v1Snapshot());
    if (!drops.has_value()) {
      return false;
    }
    for (const auto &drop : *drops) {
      if (drop.position.xMillimeter < -kArenaBound ||
          drop.position.xMillimeter > kArenaBound ||
          drop.position.yMillimeter < -kArenaBound ||
          drop.position.yMillimeter > kArenaBound) {
        return false;
      }
    }
  }
  return true;
}

bool dropIdsAreNonzeroUniqueStrictlyMonotonic() {
  for (const std::uint32_t count : {2u, 10u}) {
    const auto drops =
        generateDrops(room(7), battle(1), 1, count, RelicCatalog::v1Snapshot());
    if (!drops.has_value()) {
      return false;
    }
    for (std::size_t index = 0; index < drops->size(); ++index) {
      const auto expected = index + 1;
      if ((*drops)[index].dropId != DropId{expected}) {
        return false;
      }
    }
  }
  return true;
}

bool catalogResolvesEveryGeneratedItem() {
  const auto catalog = RelicCatalog::v1Snapshot();
  const auto drops = generateDrops(room(7), battle(1), 1, 10, catalog);
  if (!drops.has_value()) {
    return false;
  }
  for (const auto &drop : *drops) {
    if (!catalog.unitValueOf(drop.itemId).has_value()) {
      return false;
    }
  }
  return true;
}

bool dropIdsAreNonzeroUniqueStrictlyMonotonicFor(
    const std::vector<RelicDrop> &drops) {
  for (std::size_t index = 0; index < drops.size(); ++index) {
    if (drops[index].dropId != DropId{index + 1}) {
      return false;
    }
  }
  return true;
}

bool anotherBattleDoesNotReuseGeneratorState() {
  const auto battleOneFirst =
      generateDrops(room(7), battle(1), 1, 5, RelicCatalog::v1Snapshot());
  const auto battleTwo =
      generateDrops(room(7), battle(2), 1, 5, RelicCatalog::v1Snapshot());
  const auto battleOneAgain =
      generateDrops(room(7), battle(1), 1, 5, RelicCatalog::v1Snapshot());
  if (!battleOneFirst.has_value() || !battleTwo.has_value() ||
      !battleOneAgain.has_value()) {
    return false;
  }
  // A different Battle (same RoomId, different BattleInstanceId) starts a
  // fresh DropId sequence at 1 and keeps it strictly increasing;
  // regenerating the first Battle yields the exact same vector (no shared or
  // mutated generator state was consumed by Battle 2).
  if (*battleTwo == *battleOneFirst || *battleOneFirst != *battleOneAgain) {
    return false;
  }
  return dropIdsAreNonzeroUniqueStrictlyMonotonicFor(*battleOneFirst) &&
         dropIdsAreNonzeroUniqueStrictlyMonotonicFor(*battleTwo);
}

bool rejectsCapturedCountOutsideRange() {
  for (const std::uint32_t count : {0u, 1u, 11u, 1000000u}) {
    if (generateDrops(room(7), battle(1), 1, count, RelicCatalog::v1Snapshot())
            .has_value()) {
      return false;
    }
  }
  return generateDrops(room(7), battle(1), 1,
                       RelicRuleset::minCapturedParticipants,
                       RelicCatalog::v1Snapshot())
             .has_value() &&
         generateDrops(room(7), battle(1), 1,
                       RelicRuleset::maxCapturedParticipants,
                       RelicCatalog::v1Snapshot())
             .has_value();
}

} // namespace

int main() {
  if (!claimAdmissionRulesetMatchesFrozenContract() ||
      !catalogIsFrozenImmutableV1Snapshot() || !compositionForTwoCaptured() ||
      !compositionForTenCaptured() || !sameCompleteInputYieldsEqualOutput() ||
      !goldenVectorMatchesFrozenLiteral() ||
      !positionsStayInsideArenaBounds() ||
      !dropIdsAreNonzeroUniqueStrictlyMonotonic() ||
      !catalogResolvesEveryGeneratedItem() ||
      !anotherBattleDoesNotReuseGeneratorState() ||
      !rejectsCapturedCountOutsideRange()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
