#pragma once

#include <cstdint>
#include <string>

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

    void updateLastHeard(Util::TimePoint now);
    Net::TcpPacketReader& packetReader();

private:
    int clientFd_;
    uint64_t sessionId_;
    std::string remoteKey_;
    Util::TimePoint lastHeard_;
    Net::TcpPacketReader packetReader_;
};
}  // namespace Core
