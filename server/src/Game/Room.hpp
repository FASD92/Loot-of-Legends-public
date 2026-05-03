#pragma once

#include <cstdint>
#include <vector>

namespace Game {
struct Monster {
    uint32_t monsterId{0};
    uint32_t monsterTypeId{0};
    uint16_t maxHp{0};
    bool alive{false};
};

struct Drop {
    uint32_t dropId{0};
    uint32_t itemId{0};
    uint16_t quantity{0};
    uint16_t unitWeight{1};
    bool claimed{false};
    uint64_t ownerSessionId{0};
};

struct InventoryEntry {
    uint32_t itemId{0};
    uint16_t quantity{0};
};

struct InventorySnapshot {
    uint64_t sessionId{0};
    uint16_t currentWeight{0};
    uint16_t maxWeight{0};
    std::vector<InventoryEntry> entries{};
};

enum class LootRejectReason : uint16_t {
    kNone = 0,
    kAlreadyClaimed = 1,
    kOverweight = 2,
};

struct LootClaimResult {
    bool found{false};
    bool claimed{false};
    bool rejected{false};
    LootRejectReason rejectReason{LootRejectReason::kNone};
    uint64_t winnerSessionId{0};
    Drop drop{};
    InventorySnapshot inventory{};
};

class Room {
public:
    static constexpr uint16_t kDefaultMaxPlayers = 2;
    static constexpr uint16_t kDefaultMaxInventoryWeight = 10;

    explicit Room(
        uint32_t roomId,
        uint16_t maxPlayers = kDefaultMaxPlayers,
        uint16_t maxInventoryWeight = kDefaultMaxInventoryWeight);

    uint32_t roomId() const;
    uint16_t playerCount() const;
    uint16_t maxPlayers() const;
    uint16_t readyPlayerCount() const;
    bool contains(uint64_t sessionId) const;
    bool isReady(uint64_t sessionId) const;
    bool isFull() const;
    bool empty() const;
    bool battleStarted() const;
    bool hasAliveMonster() const;

    bool addPlayer(uint64_t sessionId);
    bool removePlayer(uint64_t sessionId);
    bool markReady(uint64_t sessionId);
    bool tryStartBattle();
    bool spawnMonster(uint32_t monsterId, uint32_t monsterTypeId, uint16_t maxHp);
    bool defeatMonster(uint32_t monsterId, uint32_t dropId, uint32_t itemId, uint16_t quantity);
    LootClaimResult claimLoot(uint64_t sessionId, uint32_t dropId);

    const std::vector<uint64_t>& playerSessionIds() const;
    const Monster& monster() const;
    const std::vector<Drop>& drops() const;
    const InventorySnapshot* findInventory(uint64_t sessionId) const;

private:
    InventorySnapshot* findMutableInventory(uint64_t sessionId);
    void resetBattleState();
    void resetGameState();

    uint32_t roomId_;
    uint16_t maxPlayers_;
    uint16_t maxInventoryWeight_;
    std::vector<uint64_t> playerSessionIds_;
    std::vector<uint64_t> readySessionIds_;
    std::vector<InventorySnapshot> inventories_;
    Monster monster_{};
    std::vector<Drop> drops_;
    bool battleStarted_{false};
};
}  // namespace Game
