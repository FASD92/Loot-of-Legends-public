#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Client {

class IDebugClientTransport {
public:
    virtual ~IDebugClientTransport() = default;

    virtual bool connectTo(const std::string& host, uint16_t port, std::string& outError) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    virtual bool sendPacket(const std::vector<uint8_t>& packet, std::string& outError) = 0;
    virtual bool pollPackets(std::vector<std::vector<uint8_t>>& outPackets, std::string& outError) = 0;
};

using DebugClientTransportPtr = std::unique_ptr<IDebugClientTransport>;

class SocketDebugClientTransport final : public IDebugClientTransport {
public:
    SocketDebugClientTransport() = default;
    ~SocketDebugClientTransport() override;

    SocketDebugClientTransport(const SocketDebugClientTransport&) = delete;
    SocketDebugClientTransport& operator=(const SocketDebugClientTransport&) = delete;

    bool connectTo(const std::string& host, uint16_t port, std::string& outError) override;
    void disconnect() override;
    bool isConnected() const override;
    bool sendPacket(const std::vector<uint8_t>& packet, std::string& outError) override;
    bool pollPackets(std::vector<std::vector<uint8_t>>& outPackets, std::string& outError) override;

private:
    int fd_{-1};
    std::vector<uint8_t> receiveBuffer_;
};

} // namespace Client
