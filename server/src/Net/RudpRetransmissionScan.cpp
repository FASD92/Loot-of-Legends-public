#include "Net/RudpRetransmissionScan.hpp"

#include <cstdint>
#include <vector>

namespace Net {
RudpRetransmissionScanResult scanRudpRetransmissions(
    const RudpPeerRegistry& registry,
    Util::TimePoint now) {
    RudpRetransmissionScanResult result;

    registry.forEachPeer(
        [&result, now](const UdpEndpoint& endpoint, const RudpPeer& peer) {
            const RudpReliableSendQueue& queue = peer.reliableSendQueue();

            for (uint32_t sequence : queue.expiredSequences(now)) {
                result.expired.push_back(RudpExpiredRetransmission{endpoint, sequence});
            }

            for (uint32_t sequence : queue.dueForRetransmission(now)) {
                const std::vector<uint8_t>* packetBytes = queue.packetBytes(sequence);
                if (packetBytes != nullptr) {
                    result.due.push_back(
                        RudpDueRetransmission{endpoint, sequence, packetBytes});
                }
            }
        });

    return result;
}
}  // namespace Net
