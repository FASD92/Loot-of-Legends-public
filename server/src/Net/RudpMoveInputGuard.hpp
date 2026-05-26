#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "Net/RudpInputCommandPayload.hpp"
#include "Util/Time.hpp"

namespace Net {
enum class RudpMoveInputGuardResult {
    kAccepted,
    kInvalidSession,
    kInvalidReservedFlags,
    kRateLimited,
};

class RudpMoveInputGuard {
public:
    static constexpr uint32_t kDefaultRatePerSecond = 30;
    static constexpr uint32_t kDefaultBurstCapacity = 10;

    RudpMoveInputGuard();
    RudpMoveInputGuard(uint32_t ratePerSecond, uint32_t burstCapacity);

    RudpMoveInputGuardResult record(
        uint64_t sessionId,
        const RudpInputCommandMoveArgs& move,
        Util::TimePoint now);

    bool removeSession(uint64_t sessionId);
    size_t size() const;

private:
    struct Bucket {
        uint64_t tokensScaled{0};
        Util::TimePoint lastRefillAt{};
    };

    void refill(Bucket& bucket, Util::TimePoint now) const;

    uint32_t ratePerSecond_;
    uint32_t burstCapacity_;
    uint64_t burstCapacityScaled_;
    std::unordered_map<uint64_t, Bucket> buckets_;
};
}  // namespace Net
