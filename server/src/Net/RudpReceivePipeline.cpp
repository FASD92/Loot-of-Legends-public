#include "Net/RudpReceivePipeline.hpp"

#include <utility>

namespace {
Net::RudpReceivePipelineResult mapPeerResult(
    Net::RudpPeerReceiveResult result) {
    switch (result) {
        case Net::RudpPeerReceiveResult::kDeliver:
            return Net::RudpReceivePipelineResult::kDeliver;
        case Net::RudpPeerReceiveResult::kAckOnly:
            return Net::RudpReceivePipelineResult::kAckOnly;
        case Net::RudpPeerReceiveResult::kDuplicate:
            return Net::RudpReceivePipelineResult::kDuplicate;
        case Net::RudpPeerReceiveResult::kTooOld:
            return Net::RudpReceivePipelineResult::kTooOld;
    }
    return Net::RudpReceivePipelineResult::kMalformed;
}
}  // namespace

namespace Net {
RudpReceivePipelineResult processRudpPacket(
    const RudpReceivedDatagram& datagram,
    RudpPeerRegistry& registry,
    Util::TimePoint now,
    RudpPacketDelivery& outDelivery) {
    outDelivery = RudpPacketDelivery{};

    RudpPeer* peer = registry.findOrCreate(datagram.endpoint, now);
    if (peer == nullptr) {
        return RudpReceivePipelineResult::kInvalidEndpoint;
    }

    const RudpReceivePipelineResult result = mapPeerResult(peer->receive(datagram));
    if (result != RudpReceivePipelineResult::kDeliver) {
        return result;
    }

    outDelivery.endpoint = datagram.endpoint;
    outDelivery.header = datagram.header;
    outDelivery.payload = datagram.payload;
    outDelivery.peer = peer;
    return RudpReceivePipelineResult::kDeliver;
}

RudpReceivePipelineResult receiveRudpPacket(
    UdpSocket& socket,
    RudpPeerRegistry& registry,
    Util::TimePoint now,
    RudpPacketDelivery& outDelivery) {
    outDelivery = RudpPacketDelivery{};

    RudpReceivedDatagram datagram;
    const RudpReceiveResult receiveResult =
        receiveRudpDatagram(socket, datagram);
    if (receiveResult == RudpReceiveResult::kWouldBlock) {
        return RudpReceivePipelineResult::kWouldBlock;
    }
    if (receiveResult == RudpReceiveResult::kSocketError) {
        return RudpReceivePipelineResult::kSocketError;
    }
    if (receiveResult == RudpReceiveResult::kMalformed) {
        return RudpReceivePipelineResult::kMalformed;
    }

    return processRudpPacket(datagram, registry, now, outDelivery);
}
}  // namespace Net
