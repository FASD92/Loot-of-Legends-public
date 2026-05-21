#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_set>

namespace Net {
enum class RudpGameplayEventKind {
    kBattleStart,
    kMonsterDeath,
    kLootResolved,
};

enum class RudpGameplayEventIdempotencyResult {
    kAcceptedFirst,
    kDuplicate,
    kInvalidKey,
};

struct RudpGameplayEventKey {
    RudpGameplayEventKind kind{RudpGameplayEventKind::kBattleStart};
    uint32_t roomId{0};
    uint64_t primaryId{0};
    uint64_t secondaryId{0};

    static RudpGameplayEventKey battleStart(
        uint32_t roomId,
        uint64_t playerASessionId,
        uint64_t playerBSessionId);
    static RudpGameplayEventKey monsterDeath(uint32_t roomId, uint32_t monsterId);
    static RudpGameplayEventKey lootResolved(uint32_t roomId, uint32_t dropId);

    bool operator==(const RudpGameplayEventKey& other) const;
};

class RudpGameplayEventIdempotencyTracker {
public:
    RudpGameplayEventIdempotencyResult record(const RudpGameplayEventKey& key);

    bool contains(const RudpGameplayEventKey& key) const;
    size_t size() const;
    void clear();

private:
    struct KeyHash {
        size_t operator()(const RudpGameplayEventKey& key) const;
    };

    static bool isValidKey(const RudpGameplayEventKey& key);

    std::unordered_set<RudpGameplayEventKey, KeyHash> acceptedKeys_;
};
}  // namespace Net
