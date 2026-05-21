#pragma once

#include <vector>

#include "Net/RudpDatagramReceiver.hpp"
#include "Net/RudpPeer.hpp"
#include "Net/RudpPeerRegistry.hpp"
#include "Net/UdpSocket.hpp"
#include "Util/Time.hpp"

namespace Net {
enum class RudpReceivePipelineResult {
    kWouldBlock,
    kSocketError,
    kMalformed,
    kInvalidEndpoint,
    kAckOnly,
    kDuplicate,
    kTooOld,
    kDeliver,
};

struct RudpPacketDelivery {
    UdpEndpoint endpoint;
    RudpPacketHeader header;
    std::vector<uint8_t> payload;
    RudpPeer* peer{nullptr};
};

RudpReceivePipelineResult processRudpPacket(
    const RudpReceivedDatagram& datagram,
    RudpPeerRegistry& registry,
    Util::TimePoint now,
    RudpPacketDelivery& outDelivery);

RudpReceivePipelineResult receiveRudpPacket(
    UdpSocket& socket,
    RudpPeerRegistry& registry,
    Util::TimePoint now,
    RudpPacketDelivery& outDelivery);
}  // namespace Net
