#pragma once

#include <cstdint>

namespace Net {
enum class RudpAckWindowRecordResult {
    kAcceptedNew,
    kAcceptedOutOfOrder,
    kDuplicate,
    kTooOld,
};

class RudpAckWindow {
public:
    RudpAckWindow() = default;

    RudpAckWindowRecordResult recordReceived(uint32_t sequence);

    bool hasAck() const;
    uint32_t ack() const;
    uint32_t ackBits() const;

private:
    bool hasAck_{false};
    uint32_t ack_{0};
    uint32_t ackBits_{0};
};
}  // namespace Net
