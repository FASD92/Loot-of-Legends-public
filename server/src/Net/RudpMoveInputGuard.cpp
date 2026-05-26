#include "Net/RudpMoveInputGuard.hpp"

#include <algorithm>
#include <chrono>

namespace {
constexpr uint64_t kTokenScale = 1000;
constexpr int64_t kMillisPerSecond = 1000;
}  // namespace

namespace Net {
RudpMoveInputGuard::RudpMoveInputGuard()
    : RudpMoveInputGuard(kDefaultRatePerSecond, kDefaultBurstCapacity) {}

RudpMoveInputGuard::RudpMoveInputGuard(
    uint32_t ratePerSecond,
    uint32_t burstCapacity)
    : ratePerSecond_(ratePerSecond == 0 ? kDefaultRatePerSecond : ratePerSecond),
      burstCapacity_(burstCapacity == 0 ? kDefaultBurstCapacity : burstCapacity),
      burstCapacityScaled_(static_cast<uint64_t>(burstCapacity_) * kTokenScale) {}

RudpMoveInputGuardResult RudpMoveInputGuard::record(
    uint64_t sessionId,
    const RudpInputCommandMoveArgs& move,
    Util::TimePoint now) {
    if (sessionId == 0) {
        return RudpMoveInputGuardResult::kInvalidSession;
    }
    if (move.inputFlags != 0) {
        return RudpMoveInputGuardResult::kInvalidReservedFlags;
    }

    auto [it, inserted] = buckets_.emplace(
        sessionId,
        Bucket{burstCapacityScaled_, now});
    if (!inserted) {
        refill(it->second, now);
    }

    Bucket& bucket = it->second;
    if (bucket.tokensScaled < kTokenScale) {
        return RudpMoveInputGuardResult::kRateLimited;
    }

    bucket.tokensScaled -= kTokenScale;
    return RudpMoveInputGuardResult::kAccepted;
}

bool RudpMoveInputGuard::removeSession(uint64_t sessionId) {
    return buckets_.erase(sessionId) > 0;
}

size_t RudpMoveInputGuard::size() const {
    return buckets_.size();
}

void RudpMoveInputGuard::refill(Bucket& bucket, Util::TimePoint now) const {
    if (now <= bucket.lastRefillAt) {
        return;
    }

    const int64_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - bucket.lastRefillAt).count();
    if (elapsedMs <= 0) {
        return;
    }

    const uint64_t refillScaled =
        static_cast<uint64_t>(elapsedMs) *
        static_cast<uint64_t>(ratePerSecond_) *
        kTokenScale /
        static_cast<uint64_t>(kMillisPerSecond);
    bucket.tokensScaled = std::min(
        burstCapacityScaled_,
        bucket.tokensScaled + refillScaled);
    bucket.lastRefillAt = now;
}
}  // namespace Net
