#pragma once

#include <chrono>
#include <cstdint>
#include <vector>

namespace Net {
struct NetworkFdToken {
    int fd{-1};
    uint64_t generation{0};
};

constexpr bool operator==(const NetworkFdToken& lhs, const NetworkFdToken& rhs) {
    return lhs.fd == rhs.fd && lhs.generation == rhs.generation;
}

constexpr bool operator!=(const NetworkFdToken& lhs, const NetworkFdToken& rhs) {
    return !(lhs == rhs);
}

constexpr bool isValidNetworkFdToken(const NetworkFdToken& token) {
    return token.fd >= 0 && token.generation != 0;
}

enum class NetworkEventRole : uint8_t {
    kTcpListener,
    kTcpClient,
    kUdpSocket,
    kTimer,
    kWakeup,
};

enum class NetworkEventMask : uint8_t {
    kNone = 0,
    kReadable = 1u << 0,
    kWritable = 1u << 1,
    kError = 1u << 2,
    kHangup = 1u << 3,
};

constexpr uint8_t networkEventMaskBits(NetworkEventMask mask) {
    return static_cast<uint8_t>(mask);
}

constexpr NetworkEventMask operator|(NetworkEventMask lhs, NetworkEventMask rhs) {
    return static_cast<NetworkEventMask>(
        networkEventMaskBits(lhs) | networkEventMaskBits(rhs));
}

constexpr NetworkEventMask operator&(NetworkEventMask lhs, NetworkEventMask rhs) {
    return static_cast<NetworkEventMask>(
        networkEventMaskBits(lhs) & networkEventMaskBits(rhs));
}

inline NetworkEventMask& operator|=(NetworkEventMask& lhs, NetworkEventMask rhs) {
    lhs = lhs | rhs;
    return lhs;
}

constexpr bool hasAnyNetworkEventMask(NetworkEventMask mask, NetworkEventMask candidate) {
    return networkEventMaskBits(mask & candidate) != 0;
}

constexpr bool isValidNetworkEventMask(NetworkEventMask mask) {
    constexpr uint8_t kAllowedMask =
        networkEventMaskBits(NetworkEventMask::kReadable) |
        networkEventMaskBits(NetworkEventMask::kWritable) |
        networkEventMaskBits(NetworkEventMask::kError) |
        networkEventMaskBits(NetworkEventMask::kHangup);
    return networkEventMaskBits(mask) != 0 &&
        (networkEventMaskBits(mask) & ~kAllowedMask) == 0;
}

struct NetworkEvent {
    NetworkFdToken token{};
    NetworkEventRole role{NetworkEventRole::kTcpClient};
    NetworkEventMask readyMask{NetworkEventMask::kNone};
};

enum class NetworkEventLoopStatus {
    kOk,
    kAlreadyRegistered,
    kUnknownFd,
    kStaleToken,
    kInvalidToken,
    kInvalidMask,
    kClosed,
    kBackendError,
};

enum class NetworkEventLoopWaitStatus {
    kReady,
    kTimeout,
    kClosed,
    kBackendError,
};

class NetworkEventLoop {
public:
    virtual ~NetworkEventLoop() = default;

    virtual NetworkEventLoopStatus registerFd(
        NetworkFdToken token,
        NetworkEventRole role,
        NetworkEventMask interestMask) = 0;
    virtual NetworkEventLoopStatus modifyFd(
        NetworkFdToken token,
        NetworkEventMask interestMask) = 0;
    virtual NetworkEventLoopStatus unregisterFd(NetworkFdToken token) = 0;
    virtual NetworkEventLoopWaitStatus wait(
        std::chrono::milliseconds timeout,
        std::vector<NetworkEvent>& outEvents) = 0;
    virtual void close() = 0;
};
}  // namespace Net
