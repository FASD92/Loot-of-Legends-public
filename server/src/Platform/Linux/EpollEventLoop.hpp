#pragma once

#include <chrono>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "Net/NetworkEventLoop.hpp"

namespace Net {
class EpollEventLoop final : public NetworkEventLoop {
public:
    EpollEventLoop();
    ~EpollEventLoop() override;

    EpollEventLoop(const EpollEventLoop&) = delete;
    EpollEventLoop& operator=(const EpollEventLoop&) = delete;

    NetworkEventLoopStatus registerFd(
        NetworkFdToken token,
        NetworkEventRole role,
        NetworkEventMask interestMask) override;
    NetworkEventLoopStatus modifyFd(
        NetworkFdToken token,
        NetworkEventMask interestMask) override;
    NetworkEventLoopStatus unregisterFd(NetworkFdToken token) override;
    NetworkEventLoopWaitStatus wait(
        std::chrono::milliseconds timeout,
        std::vector<NetworkEvent>& outEvents) override;
    void close() override;

private:
    struct Registration {
        uint64_t registrationId{0};
        NetworkFdToken token{};
        NetworkEventRole role{NetworkEventRole::kTcpClient};
        NetworkEventMask interestMask{NetworkEventMask::kNone};
    };

    NetworkEventLoopStatus ensureOpen() const;
    uint64_t nextRegistrationId();

    int epollFd_;
    bool closed_;
    uint64_t nextRegistrationId_;
    std::unordered_map<int, Registration> registrationsByFd_;
    std::unordered_map<uint64_t, int> fdByRegistrationId_;
};
}  // namespace Net
