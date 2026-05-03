#include "Game/Room.hpp"

#include <algorithm>
#include <limits>

namespace Game {
Room::Room(uint32_t roomId, uint16_t maxPlayers, uint16_t maxInventoryWeight)
    : roomId_(roomId), maxPlayers_(maxPlayers), maxInventoryWeight_(maxInventoryWeight) {}

uint32_t Room::roomId() const {
    return roomId_;
}

uint16_t Room::playerCount() const {
    return static_cast<uint16_t>(playerSessionIds_.size());
}

uint16_t Room::maxPlayers() const {
    return maxPlayers_;
}

uint16_t Room::readyPlayerCount() const {
    return static_cast<uint16_t>(readySessionIds_.size());
}

bool Room::contains(uint64_t sessionId) const {
    return std::find(playerSessionIds_.begin(), playerSessionIds_.end(), sessionId) !=
           playerSessionIds_.end();
}

bool Room::isReady(uint64_t sessionId) const {
    return std::find(readySessionIds_.begin(), readySessionIds_.end(), sessionId) !=
           readySessionIds_.end();
}

bool Room::isFull() const {
    return playerSessionIds_.size() >= maxPlayers_;
}

bool Room::empty() const {
    return playerSessionIds_.empty();
}

bool Room::battleStarted() const {
    return battleStarted_;
}

bool Room::hasAliveMonster() const {
    return monster_.alive;
}

bool Room::addPlayer(uint64_t sessionId) {
    if (contains(sessionId) || isFull()) {
        return false;
    }

    playerSessionIds_.push_back(sessionId);
    inventories_.push_back(InventorySnapshot{sessionId, 0, maxInventoryWeight_, {}});
    resetBattleState();
    return true;
}

bool Room::removePlayer(uint64_t sessionId) {
    auto it = std::find(playerSessionIds_.begin(), playerSessionIds_.end(), sessionId);
    if (it == playerSessionIds_.end()) {
        return false;
    }

    playerSessionIds_.erase(it);
    inventories_.erase(
        std::remove_if(
            inventories_.begin(),
            inventories_.end(),
            [sessionId](const InventorySnapshot& inventory) {
                return inventory.sessionId == sessionId;
            }),
        inventories_.end());
    resetBattleState();
    return true;
}

bool Room::markReady(uint64_t sessionId) {
    if (!contains(sessionId) || isReady(sessionId)) {
        return false;
    }

    readySessionIds_.push_back(sessionId);
    return true;
}

bool Room::tryStartBattle() {
    if (battleStarted_ || !isFull() || readySessionIds_.size() != playerSessionIds_.size()) {
        return false;
    }

    battleStarted_ = true;
    resetGameState();
    return true;
}

bool Room::spawnMonster(uint32_t monsterId, uint32_t monsterTypeId, uint16_t maxHp) {
    if (!battleStarted_ || hasAliveMonster() || !drops_.empty()) {
        return false;
    }

    monster_ = Monster{monsterId, monsterTypeId, maxHp, true};
    return true;
}

bool Room::defeatMonster(uint32_t monsterId, uint32_t dropId, uint32_t itemId, uint16_t quantity) {
    if (!hasAliveMonster() || monster_.monsterId != monsterId) {
        return false;
    }

    if (quantity == 0) {
        return false;
    }

    const auto duplicateDropIt = std::find_if(
        drops_.begin(),
        drops_.end(),
        [dropId](const Drop& drop) { return drop.dropId == dropId; });
    if (duplicateDropIt != drops_.end()) {
        return false;
    }

    monster_.alive = false;
    drops_.push_back(Drop{dropId, itemId, quantity});
    return true;
}

LootClaimResult Room::claimLoot(uint64_t sessionId, uint32_t dropId) {
    LootClaimResult result{};
    if (!contains(sessionId)) {
        return result;
    }

    auto dropIt = std::find_if(
        drops_.begin(),
        drops_.end(),
        [dropId](const Drop& drop) { return drop.dropId == dropId; });
    if (dropIt == drops_.end()) {
        return result;
    }
    // 반환 형식이 포인터인 이유는 수정하기 위해서!
    InventorySnapshot* inventory = findMutableInventory(sessionId);
    if (inventory == nullptr) {
        return result;
    }

    result.found = true;
    result.drop = *dropIt;
    result.inventory = *inventory;

    if (dropIt->claimed) {
        result.rejected = true;
        result.rejectReason = LootRejectReason::kAlreadyClaimed;
        result.winnerSessionId = dropIt->ownerSessionId;
        return result;
    }

    const uint32_t addedWeight =
        static_cast<uint32_t>(dropIt->quantity) * static_cast<uint32_t>(dropIt->unitWeight);
    if (addedWeight > static_cast<uint32_t>(inventory->maxWeight - inventory->currentWeight)) {
        result.rejected = true;
        result.rejectReason = LootRejectReason::kOverweight;
        return result;
    }

    auto entryIt = std::find_if(
        inventory->entries.begin(),
        inventory->entries.end(),
        [dropIt](const InventoryEntry& entry) { return entry.itemId == dropIt->itemId; });
    if (entryIt != inventory->entries.end()) {
        const uint32_t updatedQuantity =
            static_cast<uint32_t>(entryIt->quantity) + static_cast<uint32_t>(dropIt->quantity);
        if (updatedQuantity > std::numeric_limits<uint16_t>::max()) {
            result.rejected = true;
            result.rejectReason = LootRejectReason::kOverweight;
            return result;
        }
        entryIt->quantity = static_cast<uint16_t>(updatedQuantity);
    } else {
        inventory->entries.push_back(InventoryEntry{dropIt->itemId, dropIt->quantity});
    }

    inventory->currentWeight = static_cast<uint16_t>(
        static_cast<uint32_t>(inventory->currentWeight) + addedWeight);
    dropIt->claimed = true;
    dropIt->ownerSessionId = sessionId;

    result.claimed = true;
    result.winnerSessionId = sessionId;
    result.drop = *dropIt;
    result.inventory = *inventory;
    return result;
}

const std::vector<uint64_t>& Room::playerSessionIds() const {
    return playerSessionIds_;
}

const Monster& Room::monster() const {
    return monster_;
}

const std::vector<Drop>& Room::drops() const {
    return drops_;
}

const InventorySnapshot* Room::findInventory(uint64_t sessionId) const {
    auto it = std::find_if(
        inventories_.begin(),
        inventories_.end(),
        [sessionId](const InventorySnapshot& inventory) {
            return inventory.sessionId == sessionId;
        });
    if (it == inventories_.end()) {
        return nullptr;
    }

    return &(*it);
}

InventorySnapshot* Room::findMutableInventory(uint64_t sessionId) {
    auto it = std::find_if(
        inventories_.begin(),
        inventories_.end(),
        [sessionId](const InventorySnapshot& inventory) {
            return inventory.sessionId == sessionId;
        });
    if (it == inventories_.end()) {
        return nullptr;
    }

    return &(*it);
}

void Room::resetBattleState() {
    readySessionIds_.clear();
    battleStarted_ = false;
    resetGameState();
}

void Room::resetGameState() {
    monster_ = Monster{};
    drops_.clear();
    for (InventorySnapshot& inventory : inventories_) {
        inventory.currentWeight = 0;
        inventory.entries.clear();
    }
}
}  // namespace Game
