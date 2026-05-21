#include "Net/RudpInputCommandSequenceTracker.hpp"

#include <cstdint>

namespace {
constexpr uint32_t kAmbiguousHalfRange = 0x80000000U;

bool isSequenceNewer(uint32_t candidate, uint32_t baseline) {
    return static_cast<int32_t>(candidate - baseline) > 0;
}
}  // namespace

namespace Net {
RudpInputCommandSequenceResult RudpInputCommandSequenceTracker::record(
    uint64_t sessionId,
    uint32_t cmdSeq) {
    if (sessionId == 0) {
        return RudpInputCommandSequenceResult::kInvalidSession;
    }

    auto [it, inserted] = lastAcceptedBySession_.emplace(sessionId, cmdSeq);
    if (inserted) {
        return RudpInputCommandSequenceResult::kAcceptedFirst;
    }

    const uint32_t lastAccepted = it->second;
    if (cmdSeq == lastAccepted) {
        return RudpInputCommandSequenceResult::kDuplicate;
    }

    const uint32_t distance = cmdSeq - lastAccepted;
    if (distance == kAmbiguousHalfRange) {
        return RudpInputCommandSequenceResult::kAmbiguous;
    }

    if (isSequenceNewer(cmdSeq, lastAccepted)) {
        it->second = cmdSeq;
        return RudpInputCommandSequenceResult::kAcceptedNewer;
    }

    return RudpInputCommandSequenceResult::kStale;
}

bool RudpInputCommandSequenceTracker::removeSession(uint64_t sessionId) {
    return lastAcceptedBySession_.erase(sessionId) > 0;
}

std::optional<uint32_t> RudpInputCommandSequenceTracker::lastAcceptedCmdSeq(
    uint64_t sessionId) const {
    const auto it = lastAcceptedBySession_.find(sessionId);
    if (it == lastAcceptedBySession_.end()) {
        return std::nullopt;
    }
    return it->second;
}

size_t RudpInputCommandSequenceTracker::size() const {
    return lastAcceptedBySession_.size();
}
}  // namespace Net
