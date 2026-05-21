#pragma once

#include <cstddef>
#include <vector>

#include "Net/RudpPeerRegistry.hpp"
#include "Net/RudpRetransmissionScan.hpp"
#include "Net/UdpSocket.hpp"
#include "Util/Time.hpp"

namespace Net {
struct RudpExpiredPeerDropSummary {
    size_t expiredSequences{0};
    size_t droppedPeers{0};
};

struct RudpRetransmissionFlushSummary {
    size_t expired{0};
    size_t due{0};
    size_t resent{0};
    size_t sendErrors{0};
    size_t droppedPeers{0};
};

RudpExpiredPeerDropSummary dropExpiredRudpPeers(
    RudpPeerRegistry& registry,
    const std::vector<RudpExpiredRetransmission>& expired);

RudpRetransmissionFlushSummary flushRudpRetransmissions(
    UdpSocket& socket,
    RudpPeerRegistry& registry,
    Util::TimePoint now);
}  // namespace Net
