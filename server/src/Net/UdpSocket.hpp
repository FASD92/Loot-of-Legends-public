#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <sys/socket.h>

namespace Net {
struct UdpEndpoint {    // remoteAddr이 담기는 구조체. UDP는 연결이 없어서 매 패킷마다 '보낸 사람 주소'가 따라온다. 하지만 IPv4일지 IPv6일지 모르니 주소 구조가 여러 종류...
    sockaddr_storage addr{};    // IP주소+포트+주소 패밀리(AF)를 담을 거임. 근데 IPv4가 올지 IPv6가 올지 모르니 IPv4든 IPv6이든 다 담을 수 있는 큰 박스인 sockaddr_storage 타입을 사용
    socklen_t addrLen{0};   // addr에 실제로 채워진 길이(바이트 수).
};

class UdpSocket {
public:
    UdpSocket();
    ~UdpSocket();

    bool open(uint16_t port);
    void close();

    ssize_t receiveFrom(uint8_t* buffer, size_t bufferSize, UdpEndpoint& endpoint);
    bool sendTo(const uint8_t* data, size_t size, const UdpEndpoint& endpoint);

    int fd() const;
    uint16_t boundPort() const;

private:
    int fd_;
};

std::string endpointToString(const UdpEndpoint& endpoint);
}  // namespace Net
