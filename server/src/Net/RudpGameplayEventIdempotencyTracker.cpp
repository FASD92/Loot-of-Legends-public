#include "Net/RudpGameplayEventIdempotencyTracker.hpp"

#include <functional>

namespace {
void hashCombine(size_t& seed, size_t value) {
    seed ^= value + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
}
}  // namespace

namespace Net {
RudpGameplayEventKey RudpGameplayEventKey::battleStart(
    uint32_t roomId,
    uint64_t playerASessionId,
    uint64_t playerBSessionId) {
    return RudpGameplayEventKey{
        RudpGameplayEventKind::kBattleStart,
        roomId,
        playerASessionId,
        playerBSessionId};
}

RudpGameplayEventKey RudpGameplayEventKey::monsterDeath(
    uint32_t roomId,
    uint32_t monsterId) {
    return RudpGameplayEventKey{
        RudpGameplayEventKind::kMonsterDeath,
        roomId,
        monsterId,
        0};
}

RudpGameplayEventKey RudpGameplayEventKey::lootResolved(
    uint32_t roomId,
    uint32_t dropId) {
    return RudpGameplayEventKey{
        RudpGameplayEventKind::kLootResolved,
        roomId,
        dropId,
        0};
}

bool RudpGameplayEventKey::operator==(const RudpGameplayEventKey& other) const {
    return kind == other.kind &&
           roomId == other.roomId &&
           primaryId == other.primaryId &&
           secondaryId == other.secondaryId;
}

RudpGameplayEventIdempotencyResult RudpGameplayEventIdempotencyTracker::record(
    const RudpGameplayEventKey& key) {
    if (!isValidKey(key)) {
        return RudpGameplayEventIdempotencyResult::kInvalidKey;
    }

    const auto [_, inserted] = acceptedKeys_.insert(key);
    if (!inserted) {
        return RudpGameplayEventIdempotencyResult::kDuplicate;
    }

    return RudpGameplayEventIdempotencyResult::kAcceptedFirst;
}

bool RudpGameplayEventIdempotencyTracker::contains(
    const RudpGameplayEventKey& key) const {
    return acceptedKeys_.find(key) != acceptedKeys_.end();
}

size_t RudpGameplayEventIdempotencyTracker::size() const {
    return acceptedKeys_.size();
}

void RudpGameplayEventIdempotencyTracker::clear() {
    acceptedKeys_.clear();
}

size_t RudpGameplayEventIdempotencyTracker::KeyHash::operator()(
    const RudpGameplayEventKey& key) const {
    size_t seed = std::hash<int>{}(static_cast<int>(key.kind));
    hashCombine(seed, std::hash<uint32_t>{}(key.roomId));
    hashCombine(seed, std::hash<uint64_t>{}(key.primaryId));
    hashCombine(seed, std::hash<uint64_t>{}(key.secondaryId));
    return seed;
}

bool RudpGameplayEventIdempotencyTracker::isValidKey(
    const RudpGameplayEventKey& key) {
    if (key.roomId == 0 || key.primaryId == 0) {
        return false;
    }

    switch (key.kind) {
        case RudpGameplayEventKind::kBattleStart:
            return key.secondaryId != 0 && key.primaryId != key.secondaryId;
        case RudpGameplayEventKind::kMonsterDeath:
        case RudpGameplayEventKind::kLootResolved:
            return key.secondaryId == 0;
    }

    return false;
}
}  // namespace Net
