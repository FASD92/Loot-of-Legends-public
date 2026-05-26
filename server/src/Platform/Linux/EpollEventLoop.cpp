#include "Platform/Linux/EpollEventLoop.hpp"

#include <errno.h>
#include <limits.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <array>

namespace Net {
namespace {
constexpr int kMaxEpollEvents = 64;

uint32_t toEpollMask(NetworkEventMask mask) {
    uint32_t epollMask = 0;
    if (hasAnyNetworkEventMask(mask, NetworkEventMask::kReadable)) {
        epollMask |= EPOLLIN;
    }
    if (hasAnyNetworkEventMask(mask, NetworkEventMask::kWritable)) {
        epollMask |= EPOLLOUT;
    }
    if (hasAnyNetworkEventMask(mask, NetworkEventMask::kError)) {
        epollMask |= EPOLLERR;
    }
    if (hasAnyNetworkEventMask(mask, NetworkEventMask::kHangup)) {
        epollMask |= EPOLLHUP;
    }
    return epollMask;
}

NetworkEventMask toNetworkMask(uint32_t epollMask) {
    NetworkEventMask mask = NetworkEventMask::kNone;
    if ((epollMask & EPOLLIN) != 0) {
        mask |= NetworkEventMask::kReadable;
    }
    if ((epollMask & EPOLLOUT) != 0) {
        mask |= NetworkEventMask::kWritable;
    }
    if ((epollMask & EPOLLERR) != 0) {
        mask |= NetworkEventMask::kError;
    }
    if ((epollMask & EPOLLHUP) != 0) {
        mask |= NetworkEventMask::kHangup;
    }
#ifdef EPOLLRDHUP
    if ((epollMask & EPOLLRDHUP) != 0) {
        mask |= NetworkEventMask::kHangup;
    }
#endif
    return mask;
}

int timeoutToEpollTimeout(std::chrono::milliseconds timeout) {
    const auto count = timeout.count();
    if (count < 0) {
        return -1;
    }
    if (count > INT_MAX) {
        return INT_MAX;
    }
    return static_cast<int>(count);
}
}  // namespace

EpollEventLoop::EpollEventLoop()
    : epollFd_(::epoll_create1(EPOLL_CLOEXEC)),
      closed_(false),
      nextRegistrationId_(1) {}

EpollEventLoop::~EpollEventLoop() {
    close();
}

NetworkEventLoopStatus EpollEventLoop::registerFd(
    NetworkFdToken token,
    NetworkEventRole role,
    NetworkEventMask interestMask) {
    const NetworkEventLoopStatus openStatus = ensureOpen();
    if (openStatus != NetworkEventLoopStatus::kOk) {
        return openStatus;
    }
    if (!isValidNetworkFdToken(token)) {
        return NetworkEventLoopStatus::kInvalidToken;
    }
    if (!isValidNetworkEventMask(interestMask)) {
        return NetworkEventLoopStatus::kInvalidMask;
    }
    if (registrationsByFd_.find(token.fd) != registrationsByFd_.end()) {
        return NetworkEventLoopStatus::kAlreadyRegistered;
    }

    const uint64_t registrationId = nextRegistrationId();
    epoll_event event{};
    event.events = toEpollMask(interestMask);
    event.data.u64 = registrationId;
    if (::epoll_ctl(epollFd_, EPOLL_CTL_ADD, token.fd, &event) < 0) {
        return NetworkEventLoopStatus::kBackendError;
    }

    registrationsByFd_.emplace(
        token.fd,
        Registration{registrationId, token, role, interestMask});
    fdByRegistrationId_.emplace(registrationId, token.fd);
    return NetworkEventLoopStatus::kOk;
}

NetworkEventLoopStatus EpollEventLoop::modifyFd(
    NetworkFdToken token,
    NetworkEventMask interestMask) {
    const NetworkEventLoopStatus openStatus = ensureOpen();
    if (openStatus != NetworkEventLoopStatus::kOk) {
        return openStatus;
    }
    if (!isValidNetworkFdToken(token)) {
        return NetworkEventLoopStatus::kInvalidToken;
    }
    if (!isValidNetworkEventMask(interestMask)) {
        return NetworkEventLoopStatus::kInvalidMask;
    }

    auto registrationIt = registrationsByFd_.find(token.fd);
    if (registrationIt == registrationsByFd_.end()) {
        return NetworkEventLoopStatus::kUnknownFd;
    }
    if (registrationIt->second.token.generation != token.generation) {
        return NetworkEventLoopStatus::kStaleToken;
    }

    epoll_event event{};
    event.events = toEpollMask(interestMask);
    event.data.u64 = registrationIt->second.registrationId;
    if (::epoll_ctl(epollFd_, EPOLL_CTL_MOD, token.fd, &event) < 0) {
        return NetworkEventLoopStatus::kBackendError;
    }

    registrationIt->second.interestMask = interestMask;
    return NetworkEventLoopStatus::kOk;
}

NetworkEventLoopStatus EpollEventLoop::unregisterFd(NetworkFdToken token) {
    const NetworkEventLoopStatus openStatus = ensureOpen();
    if (openStatus != NetworkEventLoopStatus::kOk) {
        return openStatus;
    }
    if (!isValidNetworkFdToken(token)) {
        return NetworkEventLoopStatus::kInvalidToken;
    }

    auto registrationIt = registrationsByFd_.find(token.fd);
    if (registrationIt == registrationsByFd_.end()) {
        return NetworkEventLoopStatus::kUnknownFd;
    }
    if (registrationIt->second.token.generation != token.generation) {
        return NetworkEventLoopStatus::kStaleToken;
    }

    if (::epoll_ctl(epollFd_, EPOLL_CTL_DEL, token.fd, nullptr) < 0) {
        return NetworkEventLoopStatus::kBackendError;
    }

    fdByRegistrationId_.erase(registrationIt->second.registrationId);
    registrationsByFd_.erase(registrationIt);
    return NetworkEventLoopStatus::kOk;
}

NetworkEventLoopWaitStatus EpollEventLoop::wait(
    std::chrono::milliseconds timeout,
    std::vector<NetworkEvent>& outEvents) {
    outEvents.clear();
    const NetworkEventLoopStatus openStatus = ensureOpen();
    if (openStatus == NetworkEventLoopStatus::kClosed) {
        return NetworkEventLoopWaitStatus::kClosed;
    }
    if (openStatus != NetworkEventLoopStatus::kOk) {
        return NetworkEventLoopWaitStatus::kBackendError;
    }

    std::array<epoll_event, kMaxEpollEvents> events{};
    const int readyCount = ::epoll_wait(
        epollFd_,
        events.data(),
        static_cast<int>(events.size()),
        timeoutToEpollTimeout(timeout));
    if (readyCount == 0) {
        return NetworkEventLoopWaitStatus::kTimeout;
    }
    if (readyCount < 0) {
        return errno == EINTR ? NetworkEventLoopWaitStatus::kTimeout
                              : NetworkEventLoopWaitStatus::kBackendError;
    }

    outEvents.reserve(outEvents.size() + static_cast<size_t>(readyCount));
    for (int index = 0; index < readyCount; ++index) {
        const uint64_t registrationId = events[static_cast<size_t>(index)].data.u64;
        const auto fdIt = fdByRegistrationId_.find(registrationId);
        if (fdIt == fdByRegistrationId_.end()) {
            continue;
        }

        const auto registrationIt = registrationsByFd_.find(fdIt->second);
        if (registrationIt == registrationsByFd_.end() ||
            registrationIt->second.registrationId != registrationId) {
            continue;
        }

        const Registration& registration = registrationIt->second;
        const NetworkEventMask readyMask =
            toNetworkMask(events[static_cast<size_t>(index)].events);
        if (readyMask == NetworkEventMask::kNone) {
            continue;
        }

        outEvents.push_back(
            NetworkEvent{
                registration.token,
                registration.role,
                readyMask});
    }

    return outEvents.empty() ? NetworkEventLoopWaitStatus::kTimeout
                             : NetworkEventLoopWaitStatus::kReady;
}

void EpollEventLoop::close() {
    if (epollFd_ >= 0) {
        ::close(epollFd_);
        epollFd_ = -1;
    }
    closed_ = true;
    registrationsByFd_.clear();
    fdByRegistrationId_.clear();
}

NetworkEventLoopStatus EpollEventLoop::ensureOpen() const {
    if (closed_) {
        return NetworkEventLoopStatus::kClosed;
    }
    return epollFd_ >= 0 ? NetworkEventLoopStatus::kOk
                         : NetworkEventLoopStatus::kBackendError;
}

uint64_t EpollEventLoop::nextRegistrationId() {
    const uint64_t registrationId = nextRegistrationId_;
    ++nextRegistrationId_;
    if (nextRegistrationId_ == 0) {
        nextRegistrationId_ = 1;
    }
    return registrationId;
}
}  // namespace Net
