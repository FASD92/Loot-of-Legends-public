#pragma once

#include "Net/RudpAckWindow.hpp"
#include "Net/RudpDatagramReceiver.hpp"
#include "Net/RudpReliableSendQueue.hpp"

namespace Net {
enum class RudpPeerReceiveResult {
    kDeliver,
    kAckOnly,
    kDuplicate,
    kTooOld,
};

class RudpPeer {
public:
    RudpPeer() = default;

    RudpPeerReceiveResult receive(const RudpReceivedDatagram& datagram);

    RudpAckWindow& receiveWindow();
    const RudpAckWindow& receiveWindow() const;

    RudpReliableSendQueue& reliableSendQueue();
    const RudpReliableSendQueue& reliableSendQueue() const;

private:
    RudpAckWindow receiveWindow_;
    RudpReliableSendQueue reliableSendQueue_;
};
}  // namespace Net
