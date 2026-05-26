#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "Net/TcpPacketReader.hpp"
#include "Util/Time.hpp"

namespace Core {
class ClientConnection {
public:
    ClientConnection(int clientFd, uint64_t sessionId, std::string remoteKey, Util::TimePoint now);

    int clientFd() const;
    uint64_t sessionId() const;
    const std::string& remoteKey() const;   // connection을 식별하는 문자열 key
    Util::TimePoint lastHeard() const;
    uint64_t fdGeneration() const;
    bool tcpWriteInterestEnabled() const;

    void updateLastHeard(Util::TimePoint now);
    void setFdGeneration(uint64_t generation);
    void setTcpWriteInterestEnabled(bool enabled);
    Net::TcpPacketReader& packetReader();
    bool enqueueOutbound(const uint8_t* data, size_t size, size_t maxPendingBytes);
    bool hasPendingOutbound() const;
    const uint8_t* pendingOutboundData() const;
    size_t pendingOutboundSize() const;
    void consumeOutboundBytes(size_t size);
    size_t pendingOutboundBytes() const;

private:
    void compactOutboundBuffer();

    int clientFd_;
    uint64_t sessionId_;
    uint64_t fdGeneration_;
    std::string remoteKey_;
    Util::TimePoint lastHeard_;
    Net::TcpPacketReader packetReader_;
    std::vector<uint8_t> outboundBuffer_;
    size_t outboundFlushOffset_;
    bool tcpWriteInterestEnabled_;
};
}  // namespace Core
