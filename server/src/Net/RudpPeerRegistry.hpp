#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <unordered_map>

#include "Net/RudpPeer.hpp"
#include "Net/UdpSocket.hpp"
#include "Util/Time.hpp"

namespace Net {
class RudpPeerRegistry {
public:
    explicit RudpPeerRegistry(std::chrono::milliseconds timeout);

    RudpPeer* findOrCreate(const UdpEndpoint& endpoint, Util::TimePoint now);
    RudpPeer* find(const UdpEndpoint& endpoint);
    const RudpPeer* find(const UdpEndpoint& endpoint) const;
    bool remove(const UdpEndpoint& endpoint);
    bool touch(const UdpEndpoint& endpoint, Util::TimePoint now);
    void tick(Util::TimePoint now);
    size_t size() const;

    template <typename Visitor>
    void forEachPeer(Visitor visitor) {
        for (auto& entry : peers_) {
            visitor(entry.second.endpoint, entry.second.peer);
        }
    }

    template <typename Visitor>
    void forEachPeer(Visitor visitor) const {
        for (const auto& entry : peers_) {
            visitor(entry.second.endpoint, entry.second.peer);
        }
    }

private:
    struct PeerEntry {
        UdpEndpoint endpoint;
        RudpPeer peer;
        Util::TimePoint lastHeard;
    };

    std::chrono::milliseconds timeout_;
    std::unordered_map<std::string, PeerEntry> peers_;
};
}  // namespace Net
