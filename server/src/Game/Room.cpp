#include "Game/Room.hpp"

#include <algorithm>
#include <cmath>
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

bool Room::createSmokeDrop(uint32_t dropId, uint32_t itemId, uint16_t quantity) {
    if (!battleStarted_ || quantity == 0 || !drops_.empty()) {
        return false;
    }

    const auto duplicateDropIt = std::find_if(
        drops_.begin(),
        drops_.end(),
        [dropId](const Drop& drop) { return drop.dropId == dropId; });
    if (duplicateDropIt != drops_.end()) {
        return false;
    }

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

bool Room::placePlayersAroundSmokeCenter() {
    if (playerSessionIds_.size() != 2 || movementSnapshots_.size() != playerSessionIds_.size()) {
        return false;
    }

    MovementSnapshot* first = findMutableMovementSnapshot(playerSessionIds_[0]);
    MovementSnapshot* second = findMutableMovementSnapshot(playerSessionIds_[1]);
    if (first == nullptr || second == nullptr) {
        return false;
    }

    first->position = MovementPosition{-kMovementScale, 0};
    second->position = MovementPosition{kMovementScale, 0};
    return true;
}

MovementApplyResult Room::applyMovement(
    uint64_t sessionId,
    int16_t dirX,
    int16_t dirY,
    uint32_t elapsedMs) {
    MovementSnapshot* movement = findMutableMovementSnapshot(sessionId);
    if (movement == nullptr) {
        return MovementApplyResult{MovementApplyStatus::kNoPlayer, sessionId};
    }

    const MovementPosition previous = movement->position;
    MovementPosition current = previous;
    if (elapsedMs != 0 && (dirX != 0 || dirY != 0)) {
        const double x = static_cast<double>(dirX);
        const double y = static_cast<double>(dirY);
        const double length = std::sqrt((x * x) + (y * y));
        const double distance =
            (static_cast<double>(kMovementSpeedPerSecond) * static_cast<double>(elapsedMs)) /
            1000.0;
        const int64_t deltaX = static_cast<int64_t>(std::llround((x / length) * distance));
        const int64_t deltaY = static_cast<int64_t>(std::llround((y / length) * distance));
        current.x = clampMovementPosition(static_cast<int64_t>(previous.x) + deltaX);
        current.y = clampMovementPosition(static_cast<int64_t>(previous.y) + deltaY);
        movement->position = current;
    }

    return MovementApplyResult{
        MovementApplyStatus::kApplied,
        sessionId,
        previous,
        current};
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

const MovementPosition* Room::findMovementPosition(uint64_t sessionId) const {
    auto it = std::find_if(
        movementSnapshots_.begin(),
        movementSnapshots_.end(),
        [sessionId](const MovementSnapshot& movement) {
            return movement.sessionId == sessionId;
        });
    if (it == movementSnapshots_.end()) {
        return nullptr;
    }

    return &it->position;
}

const std::vector<MovementSnapshot>& Room::movementSnapshots() const {
    return movementSnapshots_;
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

MovementSnapshot* Room::findMutableMovementSnapshot(uint64_t sessionId) {
    auto it = std::find_if(
        movementSnapshots_.begin(),
        movementSnapshots_.end(),
        [sessionId](const MovementSnapshot& movement) {
            return movement.sessionId == sessionId;
        });
    if (it == movementSnapshots_.end()) {
        return nullptr;
    }

    return &(*it);
}

void Room::resetMovementPositions() {
    movementSnapshots_.clear();
    movementSnapshots_.reserve(playerSessionIds_.size());
    for (std::size_t index = 0; index < playerSessionIds_.size(); ++index) {
        movementSnapshots_.push_back(
            MovementSnapshot{playerSessionIds_[index], spawnPositionForSlot(index)});
    }
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
    resetMovementPositions();
}

MovementPosition Room::spawnPositionForSlot(std::size_t slotIndex) {
    const int32_t slotDistance =
        static_cast<int32_t>((slotIndex / 2U) + 1U) * kMovementScale;
    const int32_t signedX = (slotIndex % 2U == 0U) ? -slotDistance : slotDistance;
    return MovementPosition{clampMovementPosition(signedX), 0};
}

int32_t Room::clampMovementPosition(int64_t value) {
    if (value < kMovementMinPosition) {
        return kMovementMinPosition;
    }
    if (value > kMovementMaxPosition) {
        return kMovementMaxPosition;
    }
    return static_cast<int32_t>(value);
}
}  // namespace Game
