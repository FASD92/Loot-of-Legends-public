#pragma once

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <unordered_map>

namespace Game {
struct ItemValueEntry {
    uint32_t itemId{0};
    int64_t unitValue{0};
};

class ItemValueCatalog {
public:
    ItemValueCatalog() = default;

    static ItemValueCatalog release0();
    static ItemValueCatalog fromEntries(std::initializer_list<ItemValueEntry> entries);

    std::optional<int64_t> findUnitValue(uint32_t itemId) const;

private:
    explicit ItemValueCatalog(std::unordered_map<uint32_t, int64_t> unitValues);

    std::unordered_map<uint32_t, int64_t> unitValues_;
};
}  // namespace Game
