#include "debug_cli/DebugClientTransport.hpp"

#include <cerrno>   //errno
#include <cstring>  // std::strerror
#include <fcntl.h>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h> // close
#include <vector>

#include "Net/TcpPacket.hpp"

namespace Client {
namespace {

bool makeNonBlocking(int fd, std::string& outError) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        outError = std::strerror(errno);
        return false;
    }

    if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        outError = std::strerror(errno);
        return false;
    }

    return true;
}

} // namespace

SocketDebugClientTransport::~SocketDebugClientTransport() {
    disconnect();
}

bool SocketDebugClientTransport::connectTo(const std::string& host, uint16_t port, std::string& outError) {
    disconnect();   // 이미 연결된 fd가 있으면 먼저 끊는다. connectTo는 '새 연결 시도'의 의미다.

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;    // IPv4/IPv6 모두 허용
    hints.ai_socktype = SOCK_STREAM;    // TCP socket을 원함

    const std::string portText = std::to_string(port);  //getaddrinfo는 port를 문자열로 받으므로 변환
    addrinfo* result = nullptr; // getaddrinfo 결과 링크드 리스트를 받을 포인터
    const int gaiResult = ::getaddrinfo(host.c_str(), portText.c_str(), &hints, &result);
    if (gaiResult != 0) {
        outError = ::gai_strerror(gaiResult);
        return false;
    }

    int connectedFd = -1;   // 성공적으로 connect된 fd를 저장할 변수
    std::string lastError = "no address resolved";
    for (addrinfo* entry = result; entry != nullptr; entry = entry->ai_next) {
        const int candidateFd = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (candidateFd < 0) {
            lastError = std::strerror(errno);
            continue;
        }

#ifdef SO_NOSIGPIPE
        const int noSigPipe = 1;
        (void)::setsockopt(candidateFd, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
#endif

        if (::connect(candidateFd, entry->ai_addr, entry->ai_addrlen) == 0) {
            connectedFd = candidateFd;
            break;
        }

        lastError = std::strerror(errno);
        ::close(candidateFd);
    }
    // getaddrinfo가 할당한 주소 목록 메모리 해제
    ::freeaddrinfo(result);
    // 모든 주소 후보에 connect 실패한 경우
    if (connectedFd < 0) {
        outError = lastError;
        return false;
    }
    if (!makeNonBlocking(connectedFd, outError)) {
        ::close(connectedFd);
        return false;
    }

    // 최종 연결 성공 fd를 멤버 변수에 저장
    fd_ = connectedFd;
    receiveBuffer_.clear();
    return true;
}

void SocketDebugClientTransport::disconnect() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    receiveBuffer_.clear();
}

bool SocketDebugClientTransport::isConnected() const {
    return fd_ >= 0;
}

bool SocketDebugClientTransport::sendPacket(const std::vector<uint8_t>& packet, std::string& outError) {
    if (fd_ < 0) {
        outError = "not connected";
        return false;
    }
    if (packet.empty()) {
        outError = "empty packet";
        return false;
    }

    size_t sent = 0;
    while (sent < packet.size()) {
        const ssize_t result = ::send(fd_, packet.data() + sent, packet.size() - sent, 0);
        if (result > 0) {
            sent += static_cast<size_t>(result);
            continue;
        }

        if (result < 0 && errno == EINTR) {
            continue;
        }
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            outError = "socket is not ready for write";
            return false;
        }

        outError = std::strerror(errno);
        return false;
    }

    return true;
}

bool SocketDebugClientTransport::pollPackets(
    std::vector<std::vector<uint8_t>>& outPackets,
    std::string& outError) {
    outPackets.clear();
    if (fd_ < 0) {
        outError = "not connected";
        return false;
    }

    for (;;) {
        uint8_t buffer[4096];
        const ssize_t result = ::recv(fd_, buffer, sizeof(buffer), 0);
        if (result > 0) {
            receiveBuffer_.insert(receiveBuffer_.end(), buffer, buffer + result);
            continue;
        }
        if (result == 0) {
            disconnect();
            outError = "remote closed connection";
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        outError = std::strerror(errno);
        return false;
    }

    for (;;) {
        if (receiveBuffer_.size() < Net::kTcpHeaderSize) {
            return true;
        }

        Net::TcpPacketHeader header;
        if (!Net::peekTcpPacketHeader(receiveBuffer_.data(), receiveBuffer_.size(), header)) {
            outError = "invalid packet header";
            return false;
        }
        if (header.size < Net::kTcpHeaderSize || header.size > Net::kMaxTcpPacketSize) {
            outError = "invalid packet size";
            return false;
        }
        if (receiveBuffer_.size() < header.size) {
            return true;
        }

        outPackets.emplace_back(receiveBuffer_.begin(), receiveBuffer_.begin() + header.size);
        receiveBuffer_.erase(receiveBuffer_.begin(), receiveBuffer_.begin() + header.size);
    }
}

} // namespace Client
