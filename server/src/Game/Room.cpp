#include "Game/Room.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace Game {
Room::Room(
    uint32_t roomId,
    uint16_t maxPlayers,
    uint16_t maxInventoryWeight,
    std::string title)
    : roomId_(roomId),
      title_(std::move(title)),
      maxPlayers_(maxPlayers),
      maxInventoryWeight_(maxInventoryWeight) {}

uint32_t Room::roomId() const {
    return roomId_;
}

const std::string& Room::title() const {
    return title_;
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

bool Room::isHost(uint64_t sessionId) const {
    return !playerSessionIds_.empty() && playerSessionIds_.front() == sessionId;
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

uint64_t Room::battleInstanceId() const {
    return battleInstanceId_;
}

bool Room::hasActiveArenaLoadBarrier() const {
    return arenaLoadBarrierActive_;
}

bool Room::arenaGameplayStarted() const {
    return arenaGameplayStarted_;
}

bool Room::canStartBattle() const {
    return !battleStarted_ &&
           playerSessionIds_.size() >= kMinPlayersToStart &&
           readySessionIds_.size() == playerSessionIds_.size();
}

bool Room::hasAliveMonster() const {
    return monster_.alive;
}

uint64_t Room::hostSessionId() const {
    return playerSessionIds_.empty() ? 0 : playerSessionIds_.front();
}

bool Room::addPlayer(uint64_t sessionId) {
    if (battleStarted_ || contains(sessionId) || isFull()) {
        return false;
    }

    playerSessionIds_.push_back(sessionId);
    inventories_.push_back(InventorySnapshot{sessionId, 0, maxInventoryWeight_, {}});
    resetGameState();
    return true;
}

bool Room::removePlayer(uint64_t sessionId) {
    auto it = std::find(playerSessionIds_.begin(), playerSessionIds_.end(), sessionId);
    if (it == playerSessionIds_.end()) {
        return false;
    }

    const bool preserveGameplayState = arenaGameplayStarted_;
    playerSessionIds_.erase(it);
    readySessionIds_.erase(
        std::remove(readySessionIds_.begin(), readySessionIds_.end(), sessionId),
        readySessionIds_.end());
    if (preserveGameplayState) {
        movementSnapshots_.erase(
            std::remove_if(
                movementSnapshots_.begin(),
                movementSnapshots_.end(),
                [sessionId](const MovementSnapshot& movement) {
                    return movement.sessionId == sessionId;
                }),
            movementSnapshots_.end());
    } else {
        inventories_.erase(
            std::remove_if(
                inventories_.begin(),
                inventories_.end(),
                [sessionId](const InventorySnapshot& inventory) {
                    return inventory.sessionId == sessionId;
                }),
            inventories_.end());
        resetGameState();
    }
    return true;
}

bool Room::markReady(uint64_t sessionId) {
    if (!contains(sessionId) || battleStarted_) {
        return false;
    }

    if (isReady(sessionId)) {
        return true;
    }
    readySessionIds_.push_back(sessionId);
    return true;
}

bool Room::markUnready(uint64_t sessionId) {
    if (!contains(sessionId) || battleStarted_) {
        return false;
    }

    readySessionIds_.erase(
        std::remove(readySessionIds_.begin(), readySessionIds_.end(), sessionId),
        readySessionIds_.end());
    return true;
}

bool Room::tryStartBattle(uint64_t battleInstanceId) {
    if (battleInstanceId == 0 || !canStartBattle()) {
        return false;
    }

    battleStarted_ = true;
    battleInstanceId_ = battleInstanceId;
    arenaLoadBarrierActive_ = true;
    arenaGameplayStarted_ = false;
    arenaLoadCandidateSessionIds_ = playerSessionIds_;
    arenaLoadCompleteSessionIds_.clear();
    resetGameState();
    return true;
}

bool Room::hostStartBattle(uint64_t requestedHostSessionId, uint64_t battleInstanceId) {
    if (!isHost(requestedHostSessionId)) {
        return false;
    }

    return tryStartBattle(battleInstanceId);
}

bool Room::markArenaLoadComplete(
    uint64_t sessionId,
    uint64_t battleInstanceId,
    bool& outArenaGameplayJustStarted) {
    outArenaGameplayJustStarted = false;
    if (!battleStarted_ ||
        battleInstanceId == 0 ||
        battleInstanceId != battleInstanceId_ ||
        std::find(
            arenaLoadCandidateSessionIds_.begin(),
            arenaLoadCandidateSessionIds_.end(),
            sessionId) == arenaLoadCandidateSessionIds_.end()) {
        return false;
    }

    if (arenaGameplayStarted_) {
        return true;
    }

    if (!arenaLoadBarrierActive_) {
        return false;
    }

    if (std::find(
            arenaLoadCompleteSessionIds_.begin(),
            arenaLoadCompleteSessionIds_.end(),
            sessionId) != arenaLoadCompleteSessionIds_.end()) {
        return true;
    }

    arenaLoadCompleteSessionIds_.push_back(sessionId);
    if (arenaLoadCompleteSessionIds_.size() == arenaLoadCandidateSessionIds_.size()) {
        arenaLoadBarrierActive_ = false;
        arenaGameplayStarted_ = true;
        arenaGameplayParticipantSessionIds_ = arenaLoadCandidateSessionIds_;
        outArenaGameplayJustStarted = true;
    }
    return true;
}

bool Room::kickPlayer(uint64_t targetSessionId) {
    if (battleStarted_ || !contains(targetSessionId) || isHost(targetSessionId)) {
        return false;
    }

    return removePlayer(targetSessionId);
}

bool Room::spawnMonster(uint32_t monsterId, uint32_t monsterTypeId, uint16_t maxHp) {
    if (!battleStarted_ || hasAliveMonster() || !drops_.empty()) {
        return false;
    }

    monster_ = Monster{monsterId, monsterTypeId, maxHp, true, WorldPosition{0, 0}, maxHp};
    return true;
}

bool Room::defeatMonster(uint32_t monsterId, uint32_t dropId, uint32_t itemId, uint16_t quantity) {
    Drop drop{dropId, itemId, quantity};
    drop.position = monster_.position;
    return defeatMonster(monsterId, std::vector<Drop>{drop});
}

bool Room::defeatMonster(uint32_t monsterId, const std::vector<Drop>& scatteredDrops) {
    if (!hasAliveMonster() || monster_.monsterId != monsterId || scatteredDrops.empty()) {
        return false;
    }

    for (std::size_t i = 0; i < scatteredDrops.size(); ++i) {
        const Drop& candidate = scatteredDrops[i];
        if (candidate.quantity == 0) {
            return false;
        }

        const auto existingDropIt = std::find_if(
            drops_.begin(),
            drops_.end(),
            [&candidate](const Drop& drop) { return drop.dropId == candidate.dropId; });
        if (existingDropIt != drops_.end()) {
            return false;
        }

        for (std::size_t j = i + 1; j < scatteredDrops.size(); ++j) {
            if (candidate.dropId == scatteredDrops[j].dropId) {
                return false;
            }
        }
    }

    monster_.alive = false;
    drops_.insert(drops_.end(), scatteredDrops.begin(), scatteredDrops.end());
    return true;
}

AttackApplyResult Room::applyAttack(
    uint64_t sessionId,
    uint32_t targetHintMonsterId,
    const std::vector<Drop>& deathDrops) {
    const MovementPosition* player = findMovementPosition(sessionId);
    if (player == nullptr) {
        return AttackApplyResult{AttackApplyStatus::kNoPlayer, monster_};
    }

    if (!hasAliveMonster() ||
        (targetHintMonsterId != 0 && targetHintMonsterId != monster_.monsterId)) {
        return AttackApplyResult{AttackApplyStatus::kNoMonster, monster_};
    }

    const int64_t dx = static_cast<int64_t>(player->x) - monster_.position.x;
    const int64_t dy = static_cast<int64_t>(player->y) - monster_.position.y;
    const int64_t range = kAttackRange;
    if ((dx * dx) + (dy * dy) > range * range) {
        return AttackApplyResult{AttackApplyStatus::kOutOfRange, monster_};
    }

    monster_.currentHp =
        monster_.currentHp <= kAttackDamage
            ? 0
            : static_cast<uint16_t>(monster_.currentHp - kAttackDamage);
    if (monster_.currentHp == 0) {
        monster_.alive = false;
        drops_.insert(drops_.end(), deathDrops.begin(), deathDrops.end());
        return AttackApplyResult{AttackApplyStatus::kApplied, monster_, true, drops_};
    }

    return AttackApplyResult{AttackApplyStatus::kApplied, monster_, false, drops_};
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

void Room::markBattleDropsVisible(uint64_t generatedAtUnixMs) {
    if (drops_.empty()) {
        return;
    }

    battleDropsGenerated_ = true;
    for (Drop& drop : drops_) {
        if (drop.generatedAtUnixMs == 0) {
            drop.generatedAtUnixMs = generatedAtUnixMs;
        }
        if (drop.claimed) {
            drop.resolved = true;
        }
    }
}

bool Room::resolveExpiredBattleDrops(uint64_t nowUnixMs) {
    bool resolvedAny = false;
    if (!battleDropsGenerated_) {
        return false;
    }

    for (Drop& drop : drops_) {
        if (drop.resolved || drop.generatedAtUnixMs == 0) {
            continue;
        }
        if (nowUnixMs < drop.generatedAtUnixMs + kBattleDropResolutionTimeoutMs) {
            continue;
        }
        drop.resolved = true;
        resolvedAny = true;
    }

    return resolvedAny;
}

bool Room::hasGeneratedBattleDrops() const {
    return battleDropsGenerated_;
}

bool Room::allBattleDropsResolved() const {
    if (!battleDropsGenerated_ || drops_.empty()) {
        return false;
    }

    return std::all_of(
        drops_.begin(),
        drops_.end(),
        [](const Drop& drop) { return drop.resolved; });
}

LootClaimResult Room::claimNearestLoot(uint64_t sessionId) {
    LootClaimResult result{};
    if (!contains(sessionId)) {
        return result;
    }

    const MovementPosition* player = findMovementPosition(sessionId);
    if (player == nullptr) {
        return result;
    }

    const int64_t range = kLootRange;
    const int64_t rangeSquared = range * range;
    const Drop* nearestDrop = nullptr;
    int64_t nearestDistanceSquared = 0;
    for (const Drop& drop : drops_) {
        const int64_t dx = static_cast<int64_t>(player->x) - drop.position.x;
        const int64_t dy = static_cast<int64_t>(player->y) - drop.position.y;
        const int64_t distanceSquared = (dx * dx) + (dy * dy);
        if (distanceSquared > rangeSquared) {
            continue;
        }
        if (nearestDrop == nullptr ||
            distanceSquared < nearestDistanceSquared ||
            (distanceSquared == nearestDistanceSquared && drop.dropId < nearestDrop->dropId)) {
            nearestDrop = &drop;
            nearestDistanceSquared = distanceSquared;
        }
    }

    if (nearestDrop == nullptr) {
        return result;
    }

    return claimLoot(sessionId, nearestDrop->dropId);
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
    dropIt->resolved = true;

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

const std::vector<uint64_t>& Room::arenaGameplayParticipantSessionIds() const {
    return arenaGameplayParticipantSessionIds_;
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
            MovementSnapshot{
                playerSessionIds_[index],
                spawnPositionForSlot(index, playerSessionIds_.size())});
    }
}

void Room::resetBattleState() {
    readySessionIds_.clear();
    battleStarted_ = false;
    battleInstanceId_ = 0;
    arenaLoadBarrierActive_ = false;
    arenaGameplayStarted_ = false;
    arenaLoadCandidateSessionIds_.clear();
    arenaLoadCompleteSessionIds_.clear();
    arenaGameplayParticipantSessionIds_.clear();
    resetGameState();
}

void Room::resetGameState() {
    monster_ = Monster{};
    drops_.clear();
    battleDropsGenerated_ = false;
    for (InventorySnapshot& inventory : inventories_) {
        inventory.currentWeight = 0;
        inventory.entries.clear();
    }
    resetMovementPositions();
}

MovementPosition Room::spawnPositionForSlot(std::size_t slotIndex, std::size_t playerCount) {
    if (playerCount <= 2U) {
        const int32_t slotDistance =
            static_cast<int32_t>((slotIndex / 2U) + 1U) * kMovementScale;
        const int32_t signedX = (slotIndex % 2U == 0U) ? -slotDistance : slotDistance;
        return MovementPosition{clampMovementPosition(signedX), 0};
    }

    constexpr double kPi = 3.14159265358979323846;
    const double angle =
        (2.0 * kPi * static_cast<double>(slotIndex)) / static_cast<double>(playerCount);
    const int32_t x = static_cast<int32_t>(
        std::llround(std::cos(angle) * static_cast<double>(kPlayerSpawnRingRadius)));
    const int32_t y = static_cast<int32_t>(
        std::llround(std::sin(angle) * static_cast<double>(kPlayerSpawnRingRadius)));
    return MovementPosition{clampMovementPosition(x), clampMovementPosition(y)};
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
