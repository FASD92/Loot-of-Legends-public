#include "Net/UdpSocket.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <unistd.h>

#include <cstring>

namespace Net {
UdpSocket::UdpSocket() : fd_(-1) {}     // 멤버 초기화 리스트. 처음엔 소켓이 안 열려있으니 fd를 -1로 엶. 생성자 본문보다 먼저 실행됨.

UdpSocket::~UdpSocket() {
    close();
}

bool UdpSocket::open(uint16_t port) {
    close();

    fd_ = ::socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        return false;
    }

    int reuse = 1;      // 포트 재사용 옵션
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    int v6only = 0;     // IPv6 소켓이 IPv4도 같이 받도록 설정 = 듀얼 스택
    setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

    int flags = fcntl(fd_, F_GETFL, 0);     // 소켓을 논블로킹으로 설정
    if (flags < 0 || fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        close();
        return false;
    }

    // bind할 로컬 주소 설정
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;   // 어떤 로컬 ip로 들어오든 다 받음(0.0.0.0의 IPv6 버전)
    addr.sin6_port = htons(port);   // 네트워크는 BE가 관례라 통일하기 위해 변환

    // 소켓을 (모든 주소, 지정 포트)에 bind
    // bind(C API)는 인자를 sockaddr*로 받으니 sockaddr_in6를 해석해서 넘김.
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close();
        return false;
    }

    return true;    // 정상적으로 소켓 열고 bind까지 완료
}

void UdpSocket::close() {
    if (fd_ >= 0) {     // fd가 유효할 때만
        ::close(fd_);   // 닫고
        fd_ = -1;       // -1로 초기화
    }
}

ssize_t UdpSocket::receiveFrom(uint8_t* buffer, size_t bufferSize, UdpEndpoint& endpoint) {
    if (fd_ < 0 || buffer == nullptr || bufferSize == 0) {  // 소켓이 안 열렸거나, 버퍼가 이상하면
        return -1;  // -1 리턴
    }

    endpoint.addrLen = sizeof(endpoint.addr);   // 주소를 담을 버퍼의 크기를 커널에게 알려줌
    ssize_t received = ::recvfrom(      // UDP 데이터그램 하나를 받는다.
        fd_,
        buffer,
        bufferSize,
        0,
        reinterpret_cast<sockaddr*>(&endpoint.addr),    // 보낸 사람 주소를 endpoint.addr에 채우고
        &endpoint.addrLen);     // 실제 길이는 endpoint.addrLen에 채운다.

    if (received < 0) {     // 수신 실패
        if (errno == EAGAIN || errno == EWOULDBLOCK) {  // 논블로킹에서 '지금은 읽을 게 없음'이면 EAGAIN/EWOULDBLOCK이 뜬다.
            return 0;   // 그러면 0을 리턴
        }
        return -1;  // 그 외 에러는 -1을 리턴
    }

    // 상위 서버가 기대하는 규약: >0: 받은 바이트 수, ==0: 지금은 받을 게 없음(정상, break 조건), ==-1: 진짜 에러
    return received;    // 정상 수신이면 바이트 수 반환
}

bool UdpSocket::sendTo(const uint8_t* data, size_t size, const UdpEndpoint& endpoint) {
    if (fd_ < 0 || data == nullptr || size == 0) {  // 소켓이 없거나 보낼 데이터가 없으면
        return false;   // 실패
    }

    ssize_t sent = ::sendto(    // endpoint로 UDP 데이터그램 전송
        fd_,
        data,
        size,
        0,
        reinterpret_cast<const sockaddr*>(&endpoint.addr),
        endpoint.addrLen);  // endpoint에 들어있는 addr/addrLen을 그대로 사용

    return sent == static_cast<ssize_t>(size);  // 보낸 바이트 수가 요청한 size와 같으면 성공
}

int UdpSocket::fd() const {     // 내부 fd 반환. const라 상태 변경 x
    return fd_;
}

uint16_t UdpSocket::boundPort() const {
    if (fd_ < 0) {
        return 0;
    }

    sockaddr_storage addr{};
    socklen_t addrLen = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &addrLen) < 0) {
        return 0;
    }

    if (addr.ss_family == AF_INET) {
        const sockaddr_in* ipv4 = reinterpret_cast<const sockaddr_in*>(&addr);
        return ntohs(ipv4->sin_port);
    }

    if (addr.ss_family == AF_INET6) {
        const sockaddr_in6* ipv6 = reinterpret_cast<const sockaddr_in6*>(&addr);
        return ntohs(ipv6->sin6_port);
    }

    return 0;
}

    // 로깅용 문자열 변환
    // ip 문자열을 담을 버퍼(IPv6 최대 길이)와 포트 변수 준비
std::string endpointToString(const UdpEndpoint& endpoint) {
    char buffer[INET6_ADDRSTRLEN] = {};
    uint16_t port = 0;

    if (endpoint.addr.ss_family == AF_INET) {
        const sockaddr_in* addr = reinterpret_cast<const sockaddr_in*>(&endpoint.addr);
        inet_ntop(AF_INET, &addr->sin_addr, buffer, sizeof(buffer));
        port = ntohs(addr->sin_port);
        return std::string(buffer) + ":" + std::to_string(port);
    }

    if (endpoint.addr.ss_family == AF_INET6) {
        const sockaddr_in6* addr = reinterpret_cast<const sockaddr_in6*>(&endpoint.addr);
        inet_ntop(AF_INET6, &addr->sin6_addr, buffer, sizeof(buffer));
        port = ntohs(addr->sin6_port);
        return std::string("[") + buffer + "]:" + std::to_string(port);
    }

    return "unknown";
}
}  // namespace Net
