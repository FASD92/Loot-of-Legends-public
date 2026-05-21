#include "Net/RudpSocketDrain.hpp"

#include <utility>

namespace Net {
RudpSocketDrainSummary drainRudpSocket(
    UdpSocket& socket,
    RudpPeerRegistry& registry,
    Util::TimePoint now,
    size_t maxPackets) {
    RudpSocketDrainSummary summary;
    if (maxPackets == 0) {
        return summary;
    }

    for (size_t packetIndex = 0; packetIndex < maxPackets; ++packetIndex) {
        RudpPacketDelivery delivery;
        const RudpReceivePipelineResult result =
            receiveRudpPacket(socket, registry, now, delivery);
        ++summary.attempted;

        switch (result) {
            case RudpReceivePipelineResult::kWouldBlock:
                summary.stoppedByWouldBlock = true;
                return summary;
            case RudpReceivePipelineResult::kSocketError:
                ++summary.socketErrors;
                summary.stoppedBySocketError = true;
                return summary;
            case RudpReceivePipelineResult::kMalformed:
                ++summary.malformed;
                break;
            case RudpReceivePipelineResult::kInvalidEndpoint:
                ++summary.invalidEndpoint;
                break;
            case RudpReceivePipelineResult::kAckOnly:
                ++summary.ackOnly;
                break;
            case RudpReceivePipelineResult::kDuplicate:
                ++summary.duplicate;
                break;
            case RudpReceivePipelineResult::kTooOld:
                ++summary.tooOld;
                break;
            case RudpReceivePipelineResult::kDeliver:
                ++summary.delivered;
                summary.deliveries.push_back(std::move(delivery));
                break;
        }
    }

    summary.stoppedByMaxPackets = true;
    return summary;
}
}  // namespace Net
