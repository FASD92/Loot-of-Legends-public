#include "Net/RudpAckWindow.hpp"

#include <cstdint>

namespace {
bool isSequenceNewer(uint32_t candidate, uint32_t baseline) {
    return static_cast<int32_t>(candidate - baseline) > 0;
}
}  // namespace

namespace Net {
RudpAckWindowRecordResult RudpAckWindow::recordReceived(uint32_t sequence) {
    if (!hasAck_) {
        hasAck_ = true;
        ack_ = sequence;
        ackBits_ = 0;
        return RudpAckWindowRecordResult::kAcceptedNew;
    }

    if (sequence == ack_) {
        return RudpAckWindowRecordResult::kDuplicate;
    }

    if (isSequenceNewer(sequence, ack_)) {
        const uint32_t delta = sequence - ack_;
        if (delta < 32) {
            ackBits_ = (ackBits_ << delta) | (1U << (delta - 1));
        } else if (delta == 32) {
            ackBits_ = 1U << 31U;
        } else {
            ackBits_ = 0;
        }
        ack_ = sequence;
        return RudpAckWindowRecordResult::kAcceptedNew;
    }

    const uint32_t distance = ack_ - sequence;
    if (distance == 0) {
        return RudpAckWindowRecordResult::kDuplicate;
    }
    if (distance > 32) {
        return RudpAckWindowRecordResult::kTooOld;
    }

    const uint32_t bit = 1U << (distance - 1);
    if ((ackBits_ & bit) != 0) {
        return RudpAckWindowRecordResult::kDuplicate;
    }

    ackBits_ |= bit;
    return RudpAckWindowRecordResult::kAcceptedOutOfOrder;
}

bool RudpAckWindow::hasAck() const {
    return hasAck_;
}

uint32_t RudpAckWindow::ack() const {
    return ack_;
}

uint32_t RudpAckWindow::ackBits() const {
    return ackBits_;
}
}  // namespace Net
