#include "Game/ItemValueCatalog.hpp"

#include <utility>

namespace Game {
namespace {
constexpr uint32_t kRelease0DefaultItemId = 1001;
constexpr int64_t kRelease0DefaultUnitValue = 100;
}  // namespace

ItemValueCatalog::ItemValueCatalog(std::unordered_map<uint32_t, int64_t> unitValues)
    : unitValues_(std::move(unitValues)) {}

ItemValueCatalog ItemValueCatalog::release0() {
    return ItemValueCatalog::fromEntries({
        ItemValueEntry{kRelease0DefaultItemId, kRelease0DefaultUnitValue},
    });
}

ItemValueCatalog ItemValueCatalog::fromEntries(std::initializer_list<ItemValueEntry> entries) {
    std::unordered_map<uint32_t, int64_t> unitValues;
    for (const ItemValueEntry& entry : entries) {
        if (entry.itemId == 0) {
            continue;
        }
        unitValues[entry.itemId] = entry.unitValue;
    }
    return ItemValueCatalog(std::move(unitValues));
}

std::optional<int64_t> ItemValueCatalog::findUnitValue(uint32_t itemId) const {
    const auto it = unitValues_.find(itemId);
    if (it == unitValues_.end()) {
        return std::nullopt;
    }
    return it->second;
}
}  // namespace Game
