#include "Net/RudpDatagramReceiver.hpp"

#include <array>
#include <cstddef>
#include <utility>
#include <vector>

namespace Net {
RudpReceiveResult receiveRudpDatagram(
    UdpSocket& socket,
    RudpReceivedDatagram& outDatagram) {
    std::array<uint8_t, kMaxRudpPacketSize> buffer{};
    UdpEndpoint endpoint;

    const ssize_t received =
        socket.receiveFrom(buffer.data(), buffer.size(), endpoint);
    if (received == 0) {
        return RudpReceiveResult::kWouldBlock;
    }
    if (received < 0) {
        return RudpReceiveResult::kSocketError;
    }

    RudpPacketHeader header;
    std::vector<uint8_t> payload;
    if (!parseRudpPacket(
            buffer.data(),
            static_cast<size_t>(received),
            header,
            payload)) {
        outDatagram = RudpReceivedDatagram{};
        return RudpReceiveResult::kMalformed;
    }

    outDatagram.endpoint = endpoint;
    outDatagram.header = header;
    outDatagram.payload = std::move(payload);
    return RudpReceiveResult::kReceived;
}
}  // namespace Net
