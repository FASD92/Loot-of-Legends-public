#include <lol/battle/LootApi.hpp>

#include <cstdint>
#include <vector>

namespace lol::battle {

RelicCatalog RelicCatalog::v1Snapshot() {
  return RelicCatalog{std::vector<RelicItem>{
      RelicItem{RelicRuleset::commonItemId, 100},
      RelicItem{RelicRuleset::rareItemId, 300},
  }};
}

RelicCatalog::RelicCatalog(std::vector<RelicItem> items) noexcept
    : items_(std::move(items)) {}

const std::vector<RelicItem> &RelicCatalog::items() const noexcept {
  return items_;
}

std::size_t RelicCatalog::size() const noexcept { return items_.size(); }

std::optional<std::uint64_t>
RelicCatalog::unitValueOf(ItemId itemId) const noexcept {
  for (const auto &item : items_) {
    if (item.itemId == itemId) {
      return item.unitValue;
    }
  }
  return std::nullopt;
}

} // namespace lol::battle
