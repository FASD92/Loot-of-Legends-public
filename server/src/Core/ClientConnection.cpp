#include "Core/ClientConnection.hpp"

#include <utility>

namespace Core {
ClientConnection::ClientConnection(
    int clientFd,
    uint64_t sessionId,
    std::string remoteKey,
    Util::TimePoint now)
    : clientFd_(clientFd),
      sessionId_(sessionId),
      remoteKey_(std::move(remoteKey)),
      lastHeard_(now) {}

int ClientConnection::clientFd() const {
    return clientFd_;
}

uint64_t ClientConnection::sessionId() const {
    return sessionId_;
}

const std::string& ClientConnection::remoteKey() const {
    return remoteKey_;
}

Util::TimePoint ClientConnection::lastHeard() const {
    return lastHeard_;
}

void ClientConnection::updateLastHeard(Util::TimePoint now) {
    lastHeard_ = now;
}

Net::TcpPacketReader& ClientConnection::packetReader() {
    return packetReader_;
}
}  // namespace Core
