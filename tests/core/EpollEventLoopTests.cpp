#include <gtest/gtest.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

#include "Platform/Linux/EpollEventLoop.hpp"

namespace {
class UniqueFd {
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) : fd_(fd) {}
    ~UniqueFd() {
        reset();
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    int get() const {
        return fd_;
    }

    void reset(int fd = -1) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_{-1};
};

struct PipePair {
    UniqueFd readFd;
    UniqueFd writeFd;
};

PipePair makePipePair() {
    std::array<int, 2> fds{-1, -1};
    EXPECT_EQ(::pipe2(fds.data(), O_NONBLOCK | O_CLOEXEC), 0);
    return PipePair{UniqueFd(fds[0]), UniqueFd(fds[1])};
}

bool writeBytes(int fd, const std::vector<uint8_t>& bytes) {
    const ssize_t written = ::write(fd, bytes.data(), bytes.size());
    return written == static_cast<ssize_t>(bytes.size());
}

size_t drainUntilEagain(int fd) {
    size_t total = 0;
    std::array<uint8_t, 16> buffer{};
    while (true) {
        const ssize_t received = ::read(fd, buffer.data(), buffer.size());
        if (received > 0) {
            total += static_cast<size_t>(received);
            continue;
        }
        if (received < 0 && errno == EAGAIN) {
            return total;
        }
        return total;
    }
}

bool hasReadyMask(
    const Net::NetworkEvent& event,
    Net::NetworkEventMask expectedMask) {
    return Net::hasAnyNetworkEventMask(event.readyMask, expectedMask);
}
}  // namespace

TEST(EpollEventLoopTests, PipeReadableReadinessUsesRegisteredTokenAndRole) {
    PipePair pipe = makePipePair();
    ASSERT_GE(pipe.readFd.get(), 0);
    ASSERT_GE(pipe.writeFd.get(), 0);

    Net::EpollEventLoop loop;
    const Net::NetworkFdToken token{pipe.readFd.get(), 1};
    ASSERT_EQ(
        loop.registerFd(
            token,
            Net::NetworkEventRole::kTcpClient,
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kOk);
    ASSERT_TRUE(writeBytes(pipe.writeFd.get(), {0x01, 0x02}));

    std::vector<Net::NetworkEvent> events;
    EXPECT_EQ(
        loop.wait(std::chrono::milliseconds(100), events),
        Net::NetworkEventLoopWaitStatus::kReady);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].token, token);
    EXPECT_EQ(events[0].role, Net::NetworkEventRole::kTcpClient);
    EXPECT_TRUE(hasReadyMask(events[0], Net::NetworkEventMask::kReadable));
}

TEST(EpollEventLoopTests, NonBlockingDrainStopsAtEagainAfterReadableEvent) {
    PipePair pipe = makePipePair();
    ASSERT_GE(pipe.readFd.get(), 0);
    ASSERT_GE(pipe.writeFd.get(), 0);

    Net::EpollEventLoop loop;
    const Net::NetworkFdToken token{pipe.readFd.get(), 1};
    ASSERT_EQ(
        loop.registerFd(
            token,
            Net::NetworkEventRole::kTcpClient,
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kOk);
    ASSERT_TRUE(writeBytes(pipe.writeFd.get(), {0x10, 0x11, 0x12}));

    std::vector<Net::NetworkEvent> events;
    ASSERT_EQ(
        loop.wait(std::chrono::milliseconds(100), events),
        Net::NetworkEventLoopWaitStatus::kReady);

    EXPECT_EQ(drainUntilEagain(pipe.readFd.get()), 3U);
    errno = 0;
    std::array<uint8_t, 1> buffer{};
    EXPECT_EQ(::read(pipe.readFd.get(), buffer.data(), buffer.size()), -1);
    EXPECT_EQ(errno, EAGAIN);
}

TEST(EpollEventLoopTests, EventFdReadableWakeupIsDelivered) {
    UniqueFd wakeupFd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    ASSERT_GE(wakeupFd.get(), 0);

    Net::EpollEventLoop loop;
    const Net::NetworkFdToken token{wakeupFd.get(), 1};
    ASSERT_EQ(
        loop.registerFd(
            token,
            Net::NetworkEventRole::kWakeup,
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kOk);

    const uint64_t value = 1;
    ASSERT_EQ(
        ::write(wakeupFd.get(), &value, sizeof(value)),
        static_cast<ssize_t>(sizeof(value)));

    std::vector<Net::NetworkEvent> events;
    EXPECT_EQ(
        loop.wait(std::chrono::milliseconds(100), events),
        Net::NetworkEventLoopWaitStatus::kReady);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].token, token);
    EXPECT_EQ(events[0].role, Net::NetworkEventRole::kWakeup);
    EXPECT_TRUE(hasReadyMask(events[0], Net::NetworkEventMask::kReadable));
}

TEST(EpollEventLoopTests, TimerFdExpirationIsDeliveredAsTimerReadable) {
    UniqueFd timerFd(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
    ASSERT_GE(timerFd.get(), 0);

    Net::EpollEventLoop loop;
    const Net::NetworkFdToken token{timerFd.get(), 1};
    ASSERT_EQ(
        loop.registerFd(
            token,
            Net::NetworkEventRole::kTimer,
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kOk);

    itimerspec timerSpec{};
    timerSpec.it_value.tv_nsec = 1000000;
    ASSERT_EQ(::timerfd_settime(timerFd.get(), 0, &timerSpec, nullptr), 0);

    std::vector<Net::NetworkEvent> events;
    EXPECT_EQ(
        loop.wait(std::chrono::milliseconds(200), events),
        Net::NetworkEventLoopWaitStatus::kReady);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].token, token);
    EXPECT_EQ(events[0].role, Net::NetworkEventRole::kTimer);
    EXPECT_TRUE(hasReadyMask(events[0], Net::NetworkEventMask::kReadable));

    uint64_t expirations = 0;
    EXPECT_EQ(
        ::read(timerFd.get(), &expirations, sizeof(expirations)),
        static_cast<ssize_t>(sizeof(expirations)));
    EXPECT_GT(expirations, 0U);
}

TEST(EpollEventLoopTests, WritableInterestCanBeEnabledAndDisabled) {
    UniqueFd eventFd(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    ASSERT_GE(eventFd.get(), 0);

    Net::EpollEventLoop loop;
    const Net::NetworkFdToken token{eventFd.get(), 1};
    ASSERT_EQ(
        loop.registerFd(
            token,
            Net::NetworkEventRole::kWakeup,
            Net::NetworkEventMask::kWritable),
        Net::NetworkEventLoopStatus::kOk);

    std::vector<Net::NetworkEvent> events;
    ASSERT_EQ(
        loop.wait(std::chrono::milliseconds(100), events),
        Net::NetworkEventLoopWaitStatus::kReady);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_TRUE(hasReadyMask(events[0], Net::NetworkEventMask::kWritable));

    ASSERT_EQ(
        loop.modifyFd(token, Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kOk);
    EXPECT_EQ(
        loop.wait(std::chrono::milliseconds(10), events),
        Net::NetworkEventLoopWaitStatus::kTimeout);
}

TEST(EpollEventLoopTests, RejectsDuplicateUnknownStaleInvalidAndClosedOperations) {
    PipePair pipe = makePipePair();
    ASSERT_GE(pipe.readFd.get(), 0);

    Net::EpollEventLoop loop;
    const Net::NetworkFdToken token{pipe.readFd.get(), 1};
    ASSERT_EQ(
        loop.registerFd(
            token,
            Net::NetworkEventRole::kTcpClient,
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kOk);
    EXPECT_EQ(
        loop.registerFd(
            token,
            Net::NetworkEventRole::kTcpClient,
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kAlreadyRegistered);
    EXPECT_EQ(
        loop.modifyFd(
            Net::NetworkFdToken{pipe.writeFd.get(), 1},
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kUnknownFd);
    EXPECT_EQ(
        loop.modifyFd(
            Net::NetworkFdToken{pipe.readFd.get(), 2},
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kStaleToken);
    EXPECT_EQ(
        loop.registerFd(
            Net::NetworkFdToken{-1, 1},
            Net::NetworkEventRole::kTcpClient,
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kInvalidToken);
    EXPECT_EQ(
        loop.modifyFd(token, Net::NetworkEventMask::kNone),
        Net::NetworkEventLoopStatus::kInvalidMask);

    loop.close();
    EXPECT_EQ(
        loop.unregisterFd(token),
        Net::NetworkEventLoopStatus::kClosed);
}

TEST(EpollEventLoopTests, UnregisterPreventsOldGenerationAndAllowsNewGeneration) {
    PipePair pipe = makePipePair();
    ASSERT_GE(pipe.readFd.get(), 0);
    ASSERT_GE(pipe.writeFd.get(), 0);

    Net::EpollEventLoop loop;
    const Net::NetworkFdToken oldToken{pipe.readFd.get(), 1};
    const Net::NetworkFdToken newToken{pipe.readFd.get(), 2};
    ASSERT_EQ(
        loop.registerFd(
            oldToken,
            Net::NetworkEventRole::kTcpClient,
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kOk);
    ASSERT_TRUE(writeBytes(pipe.writeFd.get(), {0x33}));
    ASSERT_EQ(loop.unregisterFd(oldToken), Net::NetworkEventLoopStatus::kOk);
    ASSERT_EQ(
        loop.registerFd(
            newToken,
            Net::NetworkEventRole::kTcpClient,
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kOk);
    EXPECT_EQ(
        loop.modifyFd(oldToken, Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kStaleToken);

    std::vector<Net::NetworkEvent> events;
    EXPECT_EQ(
        loop.wait(std::chrono::milliseconds(100), events),
        Net::NetworkEventLoopWaitStatus::kReady);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].token, newToken);
}
