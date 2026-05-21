#pragma once

#include <cstdint>
#include <vector>

#include "Net/RudpPeerRegistry.hpp"
#include "Net/UdpSocket.hpp"
#include "Util/Time.hpp"

namespace Net {
struct RudpExpiredRetransmission {
    UdpEndpoint endpoint;
    uint32_t sequence{0};
};

struct RudpDueRetransmission {
    UdpEndpoint endpoint;
    uint32_t sequence{0};
    const std::vector<uint8_t>* packetBytes{nullptr};
};

struct RudpRetransmissionScanResult {
    std::vector<RudpExpiredRetransmission> expired;
    std::vector<RudpDueRetransmission> due;
};

RudpRetransmissionScanResult scanRudpRetransmissions(
    const RudpPeerRegistry& registry,
    Util::TimePoint now);
}  // namespace Net
