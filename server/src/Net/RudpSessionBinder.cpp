#include "Net/RudpSessionBinder.hpp"

#include <netinet/in.h>

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
RudpSessionBindResult RudpSessionBinder::bind(
    const UdpEndpoint& endpoint,
    uint64_t sessionId) {
    const std::string key = endpointKey(endpoint);
    if (key.empty() || sessionId == 0) {
        return RudpSessionBindResult::kInvalidEndpoint;
    }

    auto endpointIt = endpointToSessionId_.find(key);
    if (endpointIt != endpointToSessionId_.end()) {
        if (endpointIt->second == sessionId) {
            sessionIdToEndpoint_[sessionId] = key;
            return RudpSessionBindResult::kRefreshed;
        }
        return RudpSessionBindResult::kEndpointConflict;
    }

    auto sessionIt = sessionIdToEndpoint_.find(sessionId);
    if (sessionIt != sessionIdToEndpoint_.end() && sessionIt->second != key) {
        return RudpSessionBindResult::kSessionConflict;
    }

    endpointToSessionId_[key] = sessionId;
    sessionIdToEndpoint_[sessionId] = key;
    return RudpSessionBindResult::kBoundNew;
}

std::optional<uint64_t> RudpSessionBinder::findSessionId(
    const UdpEndpoint& endpoint) const {
    const std::string key = endpointKey(endpoint);
    if (key.empty()) {
        return std::nullopt;
    }

    auto it = endpointToSessionId_.find(key);
    if (it == endpointToSessionId_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool RudpSessionBinder::removeByEndpoint(const UdpEndpoint& endpoint) {
    const std::string key = endpointKey(endpoint);
    if (key.empty()) {
        return false;
    }

    auto endpointIt = endpointToSessionId_.find(key);
    if (endpointIt == endpointToSessionId_.end()) {
        return false;
    }

    sessionIdToEndpoint_.erase(endpointIt->second);
    endpointToSessionId_.erase(endpointIt);
    return true;
}

bool RudpSessionBinder::removeBySessionId(uint64_t sessionId) {
    auto sessionIt = sessionIdToEndpoint_.find(sessionId);
    if (sessionIt == sessionIdToEndpoint_.end()) {
        return false;
    }

    endpointToSessionId_.erase(sessionIt->second);
    sessionIdToEndpoint_.erase(sessionIt);
    return true;
}

size_t RudpSessionBinder::size() const {
    return endpointToSessionId_.size();
}
}  // namespace Net
