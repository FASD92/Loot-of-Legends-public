#include "Net/RudpPeer.hpp"

namespace Net {
RudpPeerReceiveResult RudpPeer::receive(
    const RudpReceivedDatagram& datagram) {
    const RudpPacketHeader& header = datagram.header;
    reliableSendQueue_.consumeAck(header.ack, header.ackBits);

    if ((header.flags & kRudpFlagAckOnly) != 0) {
        return RudpPeerReceiveResult::kAckOnly;
    }

    if ((header.flags & kRudpFlagReliable) == 0) {
        return RudpPeerReceiveResult::kDeliver;
    }

    const RudpAckWindowRecordResult recordResult =
        receiveWindow_.recordReceived(header.sequence);
    if (recordResult == RudpAckWindowRecordResult::kDuplicate) {
        return RudpPeerReceiveResult::kDuplicate;
    }
    if (recordResult == RudpAckWindowRecordResult::kTooOld) {
        return RudpPeerReceiveResult::kTooOld;
    }
    return RudpPeerReceiveResult::kDeliver;
}

RudpAckWindow& RudpPeer::receiveWindow() {
    return receiveWindow_;
}

const RudpAckWindow& RudpPeer::receiveWindow() const {
    return receiveWindow_;
}

RudpReliableSendQueue& RudpPeer::reliableSendQueue() {
    return reliableSendQueue_;
}

const RudpReliableSendQueue& RudpPeer::reliableSendQueue() const {
    return reliableSendQueue_;
}
}  // namespace Net
