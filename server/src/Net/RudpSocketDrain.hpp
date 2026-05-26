#pragma once

#include <cstddef>
#include <vector>

#include "Net/RudpReceivePipeline.hpp"
#include "Net/RudpPeerRegistry.hpp"
#include "Net/UdpSocket.hpp"
#include "Util/Time.hpp"

namespace Net {
struct RudpSocketDrainSummary {
    size_t attempted{0};
    size_t delivered{0};
    size_t malformed{0};
    size_t invalidEndpoint{0};
    size_t ackOnly{0};
    size_t duplicate{0};
    size_t tooOld{0};
    size_t socketErrors{0};
    bool stoppedByWouldBlock{false};
    bool stoppedByMaxPackets{false};
    bool stoppedBySocketError{false};
    std::vector<RudpPacketDelivery> ackOnlyDeliveries;
    std::vector<RudpPacketDelivery> deliveries;
};

RudpSocketDrainSummary drainRudpSocket(
    UdpSocket& socket,
    RudpPeerRegistry& registry,
    Util::TimePoint now,
    size_t maxPackets);
}  // namespace Net
