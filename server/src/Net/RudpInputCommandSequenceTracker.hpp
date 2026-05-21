#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <unordered_map>

namespace Net {
enum class RudpInputCommandSequenceResult {
    kAcceptedFirst,
    kAcceptedNewer,
    kDuplicate,
    kStale,
    kAmbiguous,
    kInvalidSession,
};

class RudpInputCommandSequenceTracker {
public:
    RudpInputCommandSequenceResult record(uint64_t sessionId, uint32_t cmdSeq);

    bool removeSession(uint64_t sessionId);
    std::optional<uint32_t> lastAcceptedCmdSeq(uint64_t sessionId) const;
    size_t size() const;

private:
    std::unordered_map<uint64_t, uint32_t> lastAcceptedBySession_;
};
}  // namespace Net
