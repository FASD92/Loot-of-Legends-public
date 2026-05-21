#pragma once

#include <vector>

#include "Net/RudpPacket.hpp"
#include "Net/UdpSocket.hpp"

namespace Net {
enum class RudpReceiveResult {
    kWouldBlock,
    kReceived,
    kMalformed,
    kSocketError,
};

struct RudpReceivedDatagram {
    UdpEndpoint endpoint;
    RudpPacketHeader header;
    std::vector<uint8_t> payload;
};

RudpReceiveResult receiveRudpDatagram(
    UdpSocket& socket,
    RudpReceivedDatagram& outDatagram);
}  // namespace Net
