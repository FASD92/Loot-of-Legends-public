#include "Net/TcpListener.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <unistd.h>

namespace Net {
namespace {
bool setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void setNoSigPipe(int fd) {
#ifdef SO_NOSIGPIPE
    int enabled = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
    (void)fd;
#endif
}
}  // namespace

TcpListener::TcpListener() : fd_(-1) {}

TcpListener::~TcpListener() {
    close();
}

bool TcpListener::open(uint16_t port) {
    close();

    fd_ = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd_ < 0) {
        return false;
    }

    int reuse = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    int v6only = 0;
    setsockopt(fd_, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only));

    if (!setNonBlocking(fd_)) {
        close();
        return false;
    }

    setNoSigPipe(fd_);

    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    addr.sin6_port = htons(port);

    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close();
        return false;
    }

    if (::listen(fd_, 128) < 0) {
        close();
        return false;
    }

    return true;
}

void TcpListener::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

AcceptStatus TcpListener::acceptClient(int& clientFd, TcpEndpoint& endpoint) {
    clientFd = -1;
    if (fd_ < 0) {
        return AcceptStatus::kError;
    }

    endpoint.addrLen = sizeof(endpoint.addr);
    int acceptedFd = ::accept(
        fd_,
        reinterpret_cast<sockaddr*>(&endpoint.addr),
        &endpoint.addrLen);

    if (acceptedFd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return AcceptStatus::kWouldBlock;
        }
        return AcceptStatus::kError;
    }

    if (!setNonBlocking(acceptedFd)) {
        ::close(acceptedFd);
        return AcceptStatus::kError;
    }

    setNoSigPipe(acceptedFd);

    clientFd = acceptedFd;
    return AcceptStatus::kAccepted;
}

ReceiveStatus TcpListener::receiveFromClient(
    int clientFd,
    uint8_t* buffer,
    size_t bufferSize,
    size_t& outReceived) {
    outReceived = 0;
    if (clientFd < 0 || buffer == nullptr || bufferSize == 0) {
        return ReceiveStatus::kError;
    }

    ssize_t received = ::recv(clientFd, buffer, bufferSize, 0);
    if (received == 0) {
        return ReceiveStatus::kClosed;
    }

    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            return ReceiveStatus::kWouldBlock;
        }
        return ReceiveStatus::kError;
    }

    outReceived = static_cast<size_t>(received);
    return ReceiveStatus::kReceived;
}

SendResult TcpListener::sendSomeToClient(int clientFd, const uint8_t* data, size_t size) {
    if (clientFd < 0 || data == nullptr || size == 0) {
        return SendResult{SendStatus::kError, 0};
    }

    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif

    const ssize_t sent = ::send(clientFd, data, size, flags);
    if (sent > 0) {
        return SendResult{SendStatus::kSent, static_cast<size_t>(sent)};
    }
    if (sent == 0) {
        return SendResult{SendStatus::kWouldBlock, 0};
    }

    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return SendResult{SendStatus::kWouldBlock, 0};
    }
    if (errno == EPIPE || errno == ECONNRESET) {
        return SendResult{SendStatus::kClosed, 0};
    }
    return SendResult{SendStatus::kError, 0};
}

bool TcpListener::sendToClient(int clientFd, const uint8_t* data, size_t size) {
    const SendResult result = sendSomeToClient(clientFd, data, size);
    return result.status == SendStatus::kSent && result.bytesSent == size;
}

void TcpListener::closeClient(int clientFd) {
    if (clientFd >= 0) {
        ::close(clientFd);
    }
}

int TcpListener::fd() const {
    return fd_;
}

uint16_t TcpListener::boundPort() const {
    if (fd_ < 0) {
        return 0;
    }

    sockaddr_in6 addr{};
    socklen_t addrLen = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &addrLen) < 0) {
        return 0;
    }

    return ntohs(addr.sin6_port);
}

std::string endpointToString(const TcpEndpoint& endpoint) {
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
