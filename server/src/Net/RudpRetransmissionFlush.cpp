#include "Net/RudpRetransmissionFlush.hpp"

#include <string>
#include <unordered_set>

namespace Net {
RudpExpiredPeerDropSummary dropExpiredRudpPeers(
    RudpPeerRegistry& registry,
    const std::vector<RudpExpiredRetransmission>& expired) {
    RudpExpiredPeerDropSummary summary;
    summary.expiredSequences = expired.size();

    std::unordered_set<std::string> droppedEndpointKeys;
    for (const RudpExpiredRetransmission& item : expired) {
        const std::string endpointKey = endpointToString(item.endpoint);
        if (endpointKey.empty() || endpointKey == "unknown") {
            continue;
        }
        if (!droppedEndpointKeys.insert(endpointKey).second) {
            continue;
        }
        if (registry.remove(item.endpoint)) {
            ++summary.droppedPeers;
        }
    }

    return summary;
}

RudpRetransmissionFlushSummary flushRudpRetransmissions(
    UdpSocket& socket,
    RudpPeerRegistry& registry,
    Util::TimePoint now) {
    const RudpRetransmissionScanResult scan = scanRudpRetransmissions(registry, now);

    RudpRetransmissionFlushSummary summary;
    summary.expired = scan.expired.size();
    summary.due = scan.due.size();

    for (const RudpDueRetransmission& due : scan.due) {
        if (due.packetBytes == nullptr || due.packetBytes->empty()) {
            ++summary.sendErrors;
            continue;
        }

        if (!socket.sendTo(
                due.packetBytes->data(),
                due.packetBytes->size(),
                due.endpoint)) {
            ++summary.sendErrors;
            continue;
        }

        RudpPeer* peer = registry.find(due.endpoint);
        if (peer == nullptr ||
            !peer->reliableSendQueue().markRetransmitted(due.sequence, now)) {
            ++summary.sendErrors;
            continue;
        }

        ++summary.resent;
    }

    const RudpExpiredPeerDropSummary dropSummary =
        dropExpiredRudpPeers(registry, scan.expired);
    summary.droppedPeers = dropSummary.droppedPeers;

    return summary;
}
}  // namespace Net
