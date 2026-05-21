#include "Net/RudpMetaResponseIdempotencyTracker.hpp"

namespace {
bool isTerminalStatus(Net::RudpMetaResponseStatus status) {
    switch (status) {
        case Net::RudpMetaResponseStatus::kApplied:
        case Net::RudpMetaResponseStatus::kDuplicate:
        case Net::RudpMetaResponseStatus::kRejected:
            return true;
        case Net::RudpMetaResponseStatus::kRetryLater:
            return false;
    }

    return false;
}
}  // namespace

namespace Net {
RudpMetaResponseIdempotencyResult RudpMetaResponseIdempotencyTracker::record(
    const std::string& settlementId,
    RudpMetaResponseStatus status) {
    if (!isValidSettlementId(settlementId)) {
        return RudpMetaResponseIdempotencyResult::kInvalidSettlementId;
    }

    if (isTerminalStatus(status)) {
        const auto [_, inserted] = completedSettlementIds_.insert(settlementId);
        retryObservedSettlementIds_.erase(settlementId);
        if (!inserted) {
            return RudpMetaResponseIdempotencyResult::kCompletionDuplicate;
        }

        return RudpMetaResponseIdempotencyResult::kCompletedFirst;
    }

    if (completedSettlementIds_.find(settlementId) != completedSettlementIds_.end()) {
        return RudpMetaResponseIdempotencyResult::kRetryIgnoredAfterCompletion;
    }

    const auto [_, inserted] = retryObservedSettlementIds_.insert(settlementId);
    if (!inserted) {
        return RudpMetaResponseIdempotencyResult::kRetryDuplicate;
    }

    return RudpMetaResponseIdempotencyResult::kRetryObserved;
}

bool RudpMetaResponseIdempotencyTracker::isCompleted(
    const std::string& settlementId) const {
    return completedSettlementIds_.find(settlementId) != completedSettlementIds_.end();
}

bool RudpMetaResponseIdempotencyTracker::isRetryObserved(
    const std::string& settlementId) const {
    return retryObservedSettlementIds_.find(settlementId) != retryObservedSettlementIds_.end();
}

size_t RudpMetaResponseIdempotencyTracker::completionCount() const {
    return completedSettlementIds_.size();
}

size_t RudpMetaResponseIdempotencyTracker::retryCount() const {
    return retryObservedSettlementIds_.size();
}

void RudpMetaResponseIdempotencyTracker::clear() {
    completedSettlementIds_.clear();
    retryObservedSettlementIds_.clear();
}

bool RudpMetaResponseIdempotencyTracker::isValidSettlementId(
    const std::string& settlementId) {
    if (settlementId.empty() ||
        settlementId.size() > kRudpMetaResponseSettlementIdMaxLength) {
        return false;
    }

    for (const unsigned char ch : settlementId) {
        if (ch < 0x20 || ch > 0x7E) {
            return false;
        }
    }

    return true;
}
}  // namespace Net
