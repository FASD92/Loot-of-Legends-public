#include "Core/ClientConnection.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace Core {
ClientConnection::ClientConnection(
    int clientFd,
    uint64_t sessionId,
    std::string remoteKey,
    Util::TimePoint now)
    : clientFd_(clientFd),
      sessionId_(sessionId),
      fdGeneration_(0),
      remoteKey_(std::move(remoteKey)),
      lastHeard_(now),
      outboundFlushOffset_(0),
      tcpWriteInterestEnabled_(false) {}

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

uint64_t ClientConnection::fdGeneration() const {
    return fdGeneration_;
}

bool ClientConnection::tcpWriteInterestEnabled() const {
    return tcpWriteInterestEnabled_;
}

void ClientConnection::updateLastHeard(Util::TimePoint now) {
    lastHeard_ = now;
}

void ClientConnection::setFdGeneration(uint64_t generation) {
    fdGeneration_ = generation;
}

void ClientConnection::setTcpWriteInterestEnabled(bool enabled) {
    tcpWriteInterestEnabled_ = enabled;
}

Net::TcpPacketReader& ClientConnection::packetReader() {
    return packetReader_;
}

bool ClientConnection::enqueueOutbound(
    const uint8_t* data,
    size_t size,
    size_t maxPendingBytes) {
    if (data == nullptr || size == 0) {
        return false;
    }
    compactOutboundBuffer();
    if (pendingOutboundBytes() + size > maxPendingBytes) {
        return false;
    }
    outboundBuffer_.insert(outboundBuffer_.end(), data, data + size);
    return true;
}

bool ClientConnection::hasPendingOutbound() const {
    return pendingOutboundSize() > 0;
}

const uint8_t* ClientConnection::pendingOutboundData() const {
    if (!hasPendingOutbound()) {
        return nullptr;
    }
    return outboundBuffer_.data() + outboundFlushOffset_;
}

size_t ClientConnection::pendingOutboundSize() const {
    return outboundBuffer_.size() - outboundFlushOffset_;
}

void ClientConnection::consumeOutboundBytes(size_t size) {
    const size_t consumed = std::min(size, pendingOutboundSize());
    outboundFlushOffset_ += consumed;
    if (outboundFlushOffset_ == outboundBuffer_.size()) {
        outboundBuffer_.clear();
        outboundFlushOffset_ = 0;
    }
}

size_t ClientConnection::pendingOutboundBytes() const {
    return pendingOutboundSize();
}

void ClientConnection::compactOutboundBuffer() {
    if (outboundFlushOffset_ == 0) {
        return;
    }
    outboundBuffer_.erase(
        outboundBuffer_.begin(),
        outboundBuffer_.begin() + static_cast<std::ptrdiff_t>(outboundFlushOffset_));
    outboundFlushOffset_ = 0;
}
}  // namespace Core
