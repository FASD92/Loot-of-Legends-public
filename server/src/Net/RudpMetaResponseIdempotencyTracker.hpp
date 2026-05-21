#pragma once

#include <cstddef>
#include <string>
#include <unordered_set>

namespace Net {
constexpr size_t kRudpMetaResponseSettlementIdMaxLength = 64;

enum class RudpMetaResponseStatus {
    kApplied,
    kDuplicate,
    kRejected,
    kRetryLater,
};

enum class RudpMetaResponseIdempotencyResult {
    kCompletedFirst,
    kCompletionDuplicate,
    kRetryObserved,
    kRetryDuplicate,
    kRetryIgnoredAfterCompletion,
    kInvalidSettlementId,
};

class RudpMetaResponseIdempotencyTracker {
public:
    RudpMetaResponseIdempotencyResult record(
        const std::string& settlementId,
        RudpMetaResponseStatus status);

    bool isCompleted(const std::string& settlementId) const;
    bool isRetryObserved(const std::string& settlementId) const;
    size_t completionCount() const;
    size_t retryCount() const;
    void clear();

private:
    static bool isValidSettlementId(const std::string& settlementId);

    std::unordered_set<std::string> completedSettlementIds_;
    std::unordered_set<std::string> retryObservedSettlementIds_;
};
}  // namespace Net
