#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Game {
struct WorldPosition {
    int32_t x{0};
    int32_t y{0};
};

struct Monster {
    uint32_t monsterId{0};
    uint32_t monsterTypeId{0};
    uint16_t maxHp{0};
    bool alive{false};
    WorldPosition position{};
    uint16_t currentHp{0};
};

struct Drop {
    uint32_t dropId{0};
    uint32_t itemId{0};
    uint16_t quantity{0};
    uint16_t unitWeight{1};
    bool claimed{false};
    uint64_t ownerSessionId{0};
    WorldPosition position{};
    uint64_t generatedAtUnixMs{0};
    bool resolved{false};
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

struct MovementPosition {
    int32_t x{0};
    int32_t y{0};
};

struct MovementSnapshot {
    uint64_t sessionId{0};
    MovementPosition position{};
};

enum class MovementApplyStatus : uint16_t {
    kApplied = 0,
    kNoPlayer = 1,
};

struct MovementApplyResult {
    MovementApplyStatus status{MovementApplyStatus::kNoPlayer};
    uint64_t sessionId{0};
    MovementPosition previousPosition{};
    MovementPosition currentPosition{};
};

enum class AttackApplyStatus : uint16_t {
    kApplied = 0,
    kNoPlayer = 1,
    kNoMonster = 2,
    kOutOfRange = 3,
};

struct AttackApplyResult {
    AttackApplyStatus status{AttackApplyStatus::kNoMonster};
    Monster monster{};
    bool monsterJustDefeated{false};
    std::vector<Drop> drops{};
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
    static constexpr uint16_t kDefaultMaxPlayers = 10;
    static constexpr uint16_t kMinPlayersToStart = 2;
    static constexpr uint16_t kDefaultMaxInventoryWeight = 10;
    static constexpr int32_t kMovementScale = 1000;
    static constexpr int32_t kMovementMinPosition = -50 * kMovementScale;
    static constexpr int32_t kMovementMaxPosition = 50 * kMovementScale;
    static constexpr int32_t kArenaMinPosition = kMovementMinPosition;
    static constexpr int32_t kArenaMaxPosition = kMovementMaxPosition;
    static constexpr int32_t kMovementSpeedPerSecond = kMovementScale;
    static constexpr int32_t kPlayerSpawnRingRadius = 8 * kMovementScale;
    static constexpr int32_t kAttackRange = 10 * kMovementScale;
    static constexpr int32_t kLootRange = 3 * kMovementScale;
    static constexpr uint16_t kAttackDamage = 25;
    static constexpr uint64_t kBattleDropResolutionTimeoutMs = 60000;

    explicit Room(
        uint32_t roomId,
        uint16_t maxPlayers = kDefaultMaxPlayers,
        uint16_t maxInventoryWeight = kDefaultMaxInventoryWeight,
        std::string title = {});

    uint32_t roomId() const;
    const std::string& title() const;
    uint16_t playerCount() const;
    uint16_t maxPlayers() const;
    uint16_t readyPlayerCount() const;
    bool contains(uint64_t sessionId) const;
    bool isReady(uint64_t sessionId) const;
    bool isHost(uint64_t sessionId) const;
    bool isFull() const;
    bool empty() const;
    bool battleStarted() const;
    uint64_t battleInstanceId() const;
    bool hasActiveArenaLoadBarrier() const;
    bool arenaGameplayStarted() const;
    bool canStartBattle() const;
    bool hasAliveMonster() const;
    uint64_t hostSessionId() const;

    bool addPlayer(uint64_t sessionId);
    bool removePlayer(uint64_t sessionId);
    bool markReady(uint64_t sessionId);
    bool markUnready(uint64_t sessionId);
    bool tryStartBattle(uint64_t battleInstanceId = 1);
    bool hostStartBattle(uint64_t hostSessionId, uint64_t battleInstanceId = 1);
    bool markArenaLoadComplete(
        uint64_t sessionId,
        uint64_t battleInstanceId,
        bool& outArenaGameplayJustStarted);
    bool kickPlayer(uint64_t targetSessionId);
    bool spawnMonster(uint32_t monsterId, uint32_t monsterTypeId, uint16_t maxHp);
    bool defeatMonster(uint32_t monsterId, uint32_t dropId, uint32_t itemId, uint16_t quantity);
    bool defeatMonster(uint32_t monsterId, const std::vector<Drop>& scatteredDrops);
    AttackApplyResult applyAttack(
        uint64_t sessionId,
        uint32_t targetHintMonsterId,
        const std::vector<Drop>& deathDrops);
    bool createSmokeDrop(uint32_t dropId, uint32_t itemId, uint16_t quantity);
    void markBattleDropsVisible(uint64_t generatedAtUnixMs);
    bool resolveExpiredBattleDrops(uint64_t nowUnixMs);
    bool hasGeneratedBattleDrops() const;
    bool allBattleDropsResolved() const;
    LootClaimResult claimNearestLoot(uint64_t sessionId);
    LootClaimResult claimLoot(uint64_t sessionId, uint32_t dropId);
    bool placePlayersAroundSmokeCenter();
    MovementApplyResult applyMovement(
        uint64_t sessionId,
        int16_t dirX,
        int16_t dirY,
        uint32_t elapsedMs);

    const std::vector<uint64_t>& playerSessionIds() const;
    const std::vector<uint64_t>& arenaGameplayParticipantSessionIds() const;
    const Monster& monster() const;
    const std::vector<Drop>& drops() const;
    const InventorySnapshot* findInventory(uint64_t sessionId) const;
    const MovementPosition* findMovementPosition(uint64_t sessionId) const;
    const std::vector<MovementSnapshot>& movementSnapshots() const;

private:
    InventorySnapshot* findMutableInventory(uint64_t sessionId);
    MovementSnapshot* findMutableMovementSnapshot(uint64_t sessionId);
    void resetMovementPositions();
    void resetBattleState();
    void resetGameState();
    static MovementPosition spawnPositionForSlot(std::size_t slotIndex, std::size_t playerCount);
    static int32_t clampMovementPosition(int64_t value);

    uint32_t roomId_;
    std::string title_;
    uint16_t maxPlayers_;
    uint16_t maxInventoryWeight_;
    std::vector<uint64_t> playerSessionIds_;
    std::vector<uint64_t> readySessionIds_;
    std::vector<InventorySnapshot> inventories_;
    std::vector<MovementSnapshot> movementSnapshots_;
    Monster monster_{};
    std::vector<Drop> drops_;
    bool battleDropsGenerated_{false};
    bool battleStarted_{false};
    uint64_t battleInstanceId_{0};
    bool arenaLoadBarrierActive_{false};
    bool arenaGameplayStarted_{false};
    std::vector<uint64_t> arenaLoadCandidateSessionIds_;
    std::vector<uint64_t> arenaLoadCompleteSessionIds_;
    std::vector<uint64_t> arenaGameplayParticipantSessionIds_;
};
}  // namespace Game
