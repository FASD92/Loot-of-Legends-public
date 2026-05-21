#include "Net/RudpPeerRegistry.hpp"

#include <netinet/in.h>

#include <string>

namespace {
bool isKnownEndpoint(const Net::UdpEndpoint& endpoint) {
    if (endpoint.addr.ss_family == AF_INET) {
        return endpoint.addrLen >= sizeof(sockaddr_in);
    }
    if (endpoint.addr.ss_family == AF_INET6) {
        return endpoint.addrLen >= sizeof(sockaddr_in6);
    }
    return false;
}

std::string endpointKey(const Net::UdpEndpoint& endpoint) {
    if (!isKnownEndpoint(endpoint)) {
        return "";
    }

    const std::string key = Net::endpointToString(endpoint);
    if (key.empty() || key == "unknown") {
        return "";
    }
    return key;
}
}  // namespace

namespace Net {
RudpPeerRegistry::RudpPeerRegistry(std::chrono::milliseconds timeout)
    : timeout_(timeout) {}

RudpPeer* RudpPeerRegistry::findOrCreate(
    const UdpEndpoint& endpoint,
    Util::TimePoint now) {
    const std::string key = endpointKey(endpoint);
    if (key.empty()) {
        return nullptr;
    }

    auto it = peers_.find(key);
    if (it != peers_.end()) {
        it->second.lastHeard = now;
        return &it->second.peer;
    }

    auto [insertedIt, inserted] =
        peers_.emplace(key, PeerEntry{endpoint, RudpPeer{}, now});
    (void)inserted;
    return &insertedIt->second.peer;
}

RudpPeer* RudpPeerRegistry::find(const UdpEndpoint& endpoint) {
    const std::string key = endpointKey(endpoint);
    if (key.empty()) {
        return nullptr;
    }

    auto it = peers_.find(key);
    if (it == peers_.end()) {
        return nullptr;
    }
    return &it->second.peer;
}

const RudpPeer* RudpPeerRegistry::find(const UdpEndpoint& endpoint) const {
    const std::string key = endpointKey(endpoint);
    if (key.empty()) {
        return nullptr;
    }

    auto it = peers_.find(key);
    if (it == peers_.end()) {
        return nullptr;
    }
    return &it->second.peer;
}

bool RudpPeerRegistry::remove(const UdpEndpoint& endpoint) {
    const std::string key = endpointKey(endpoint);
    if (key.empty()) {
        return false;
    }
    return peers_.erase(key) > 0;
}

bool RudpPeerRegistry::touch(
    const UdpEndpoint& endpoint,
    Util::TimePoint now) {
    const std::string key = endpointKey(endpoint);
    if (key.empty()) {
        return false;
    }

    auto it = peers_.find(key);
    if (it == peers_.end()) {
        return false;
    }

    it->second.lastHeard = now;
    return true;
}

void RudpPeerRegistry::tick(Util::TimePoint now) {
    for (auto it = peers_.begin(); it != peers_.end();) {
        if (now - it->second.lastHeard > timeout_) {
            it = peers_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t RudpPeerRegistry::size() const {
    return peers_.size();
}
}  // namespace Net
