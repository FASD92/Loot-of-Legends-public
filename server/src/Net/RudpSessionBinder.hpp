#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>

#include "Net/UdpSocket.hpp"

namespace Net {
enum class RudpSessionBindResult {
    kBoundNew,
    kRefreshed,
    kEndpointConflict,
    kSessionConflict,
    kInvalidEndpoint,
};

class RudpSessionBinder {
public:
    RudpSessionBindResult bind(const UdpEndpoint& endpoint, uint64_t sessionId);
    std::optional<uint64_t> findSessionId(const UdpEndpoint& endpoint) const;
    bool removeByEndpoint(const UdpEndpoint& endpoint);
    bool removeBySessionId(uint64_t sessionId);
    size_t size() const;

private:
    std::unordered_map<std::string, uint64_t> endpointToSessionId_;
    std::unordered_map<uint64_t, std::string> sessionIdToEndpoint_;
};
}  // namespace Net
