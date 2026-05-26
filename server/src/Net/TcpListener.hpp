#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <sys/socket.h>

namespace Net {
struct TcpEndpoint {
    sockaddr_storage addr{};
    socklen_t addrLen{0};
};

enum class AcceptStatus {
    kAccepted,
    kWouldBlock,
    kError,
};

enum class ReceiveStatus {
    kReceived,
    kWouldBlock,
    kClosed,
    kError,
};

enum class SendStatus {
    kSent,
    kWouldBlock,
    kClosed,
    kError,
};

struct SendResult {
    SendStatus status{SendStatus::kError};
    size_t bytesSent{0};
};

class TcpListener {
public:
    TcpListener();
    ~TcpListener();

    bool open(uint16_t port);
    void close();

    AcceptStatus acceptClient(int& clientFd, TcpEndpoint& endpoint);
    ReceiveStatus receiveFromClient(
        int clientFd,
        uint8_t* buffer,
        size_t bufferSize,
        size_t& outReceived);
    SendResult sendSomeToClient(int clientFd, const uint8_t* data, size_t size);
    bool sendToClient(int clientFd, const uint8_t* data, size_t size);
    void closeClient(int clientFd);

    int fd() const;
    uint16_t boundPort() const;

private:
    int fd_;
};

std::string endpointToString(const TcpEndpoint& endpoint);
}  // namespace Net
