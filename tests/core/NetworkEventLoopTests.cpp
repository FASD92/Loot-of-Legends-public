#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>

#include "Net/NetworkEventLoop.hpp"

namespace {
class FakeNetworkEventLoop final : public Net::NetworkEventLoop {
public:
    Net::NetworkEventLoopStatus registerFd(
        Net::NetworkFdToken token,
        Net::NetworkEventRole role,
        Net::NetworkEventMask interestMask) override {
        if (closed_) {
            return Net::NetworkEventLoopStatus::kClosed;
        }
        if (!Net::isValidNetworkFdToken(token)) {
            return Net::NetworkEventLoopStatus::kInvalidToken;
        }
        if (!Net::isValidNetworkEventMask(interestMask)) {
            return Net::NetworkEventLoopStatus::kInvalidMask;
        }
        if (registrations_.find(token.fd) != registrations_.end()) {
            return Net::NetworkEventLoopStatus::kAlreadyRegistered;
        }

        registrations_.emplace(token.fd, Registration{token, role, interestMask});
        return Net::NetworkEventLoopStatus::kOk;
    }

    Net::NetworkEventLoopStatus modifyFd(
        Net::NetworkFdToken token,
        Net::NetworkEventMask interestMask) override {
        if (closed_) {
            return Net::NetworkEventLoopStatus::kClosed;
        }
        if (!Net::isValidNetworkFdToken(token)) {
            return Net::NetworkEventLoopStatus::kInvalidToken;
        }
        if (!Net::isValidNetworkEventMask(interestMask)) {
            return Net::NetworkEventLoopStatus::kInvalidMask;
        }

        auto it = registrations_.find(token.fd);
        if (it == registrations_.end()) {
            return Net::NetworkEventLoopStatus::kUnknownFd;
        }
        if (it->second.token.generation != token.generation) {
            return Net::NetworkEventLoopStatus::kStaleToken;
        }

        it->second.interestMask = interestMask;
        return Net::NetworkEventLoopStatus::kOk;
    }

    Net::NetworkEventLoopStatus unregisterFd(Net::NetworkFdToken token) override {
        if (closed_) {
            return Net::NetworkEventLoopStatus::kClosed;
        }
        if (!Net::isValidNetworkFdToken(token)) {
            return Net::NetworkEventLoopStatus::kInvalidToken;
        }

        auto it = registrations_.find(token.fd);
        if (it == registrations_.end()) {
            return Net::NetworkEventLoopStatus::kUnknownFd;
        }
        if (it->second.token.generation != token.generation) {
            return Net::NetworkEventLoopStatus::kStaleToken;
        }

        registrations_.erase(it);
        return Net::NetworkEventLoopStatus::kOk;
    }

    Net::NetworkEventLoopWaitStatus wait(
        std::chrono::milliseconds timeout,
        std::vector<Net::NetworkEvent>& outEvents) override {
        (void)timeout;
        outEvents.clear();
        if (closed_) {
            return Net::NetworkEventLoopWaitStatus::kClosed;
        }

        while (!queuedEvents_.empty()) {
            const Net::NetworkEvent event = queuedEvents_.front();
            queuedEvents_.pop_front();

            const auto it = registrations_.find(event.token.fd);
            if (it == registrations_.end()) {
                continue;
            }
            const Registration& registration = it->second;
            if (registration.token.generation != event.token.generation ||
                !Net::hasAnyNetworkEventMask(event.readyMask, registration.interestMask)) {
                continue;
            }

            outEvents.push_back(
                Net::NetworkEvent{
                    registration.token,
                    registration.role,
                    event.readyMask & registration.interestMask});
        }

        return outEvents.empty()
            ? Net::NetworkEventLoopWaitStatus::kTimeout
            : Net::NetworkEventLoopWaitStatus::kReady;
    }

    void close() override {
        closed_ = true;
        registrations_.clear();
        queuedEvents_.clear();
    }

    void queueEvent(Net::NetworkEvent event) {
        queuedEvents_.push_back(event);
    }

    Net::NetworkEventMask interestFor(Net::NetworkFdToken token) const {
        const auto it = registrations_.find(token.fd);
        if (it == registrations_.end() || it->second.token.generation != token.generation) {
            return Net::NetworkEventMask::kNone;
        }
        return it->second.interestMask;
    }

private:
    struct Registration {
        Net::NetworkFdToken token{};
        Net::NetworkEventRole role{Net::NetworkEventRole::kTcpClient};
        Net::NetworkEventMask interestMask{Net::NetworkEventMask::kNone};
    };

    bool closed_{false};
    std::unordered_map<int, Registration> registrations_;
    std::deque<Net::NetworkEvent> queuedEvents_;
};

Net::NetworkEventMask readableWritable() {
    return Net::NetworkEventMask::kReadable | Net::NetworkEventMask::kWritable;
}
}  // namespace

TEST(NetworkEventLoopTests, MaskHelpersValidateAllowedBitsAndTokenGeneration) {
    EXPECT_TRUE(Net::isValidNetworkFdToken(Net::NetworkFdToken{3, 1}));
    EXPECT_FALSE(Net::isValidNetworkFdToken(Net::NetworkFdToken{-1, 1}));
    EXPECT_FALSE(Net::isValidNetworkFdToken(Net::NetworkFdToken{3, 0}));

    EXPECT_TRUE(Net::isValidNetworkEventMask(Net::NetworkEventMask::kReadable));
    EXPECT_TRUE(Net::isValidNetworkEventMask(readableWritable()));
    EXPECT_FALSE(Net::isValidNetworkEventMask(Net::NetworkEventMask::kNone));
    EXPECT_FALSE(Net::isValidNetworkEventMask(
        static_cast<Net::NetworkEventMask>(0x80)));
    EXPECT_TRUE(Net::hasAnyNetworkEventMask(
        readableWritable(),
        Net::NetworkEventMask::kWritable));
}

TEST(NetworkEventLoopTests, RegistersAndModifiesFdInterest) {
    FakeNetworkEventLoop loop;
    const Net::NetworkFdToken listenerToken{10, 1};

    EXPECT_EQ(
        loop.registerFd(
            listenerToken,
            Net::NetworkEventRole::kTcpListener,
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kOk);
    EXPECT_EQ(
        loop.registerFd(
            listenerToken,
            Net::NetworkEventRole::kTcpListener,
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kAlreadyRegistered);
    EXPECT_EQ(
        loop.modifyFd(listenerToken, readableWritable()),
        Net::NetworkEventLoopStatus::kOk);
    EXPECT_EQ(loop.interestFor(listenerToken), readableWritable());
}

TEST(NetworkEventLoopTests, RejectsUnknownStaleInvalidAndClosedOperations) {
    FakeNetworkEventLoop loop;
    const Net::NetworkFdToken token{20, 1};

    EXPECT_EQ(
        loop.modifyFd(token, Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kUnknownFd);
    EXPECT_EQ(
        loop.registerFd(
            Net::NetworkFdToken{-1, 1},
            Net::NetworkEventRole::kTcpClient,
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kInvalidToken);
    EXPECT_EQ(
        loop.registerFd(
            token,
            Net::NetworkEventRole::kTcpClient,
            static_cast<Net::NetworkEventMask>(0x80)),
        Net::NetworkEventLoopStatus::kInvalidMask);

    ASSERT_EQ(
        loop.registerFd(
            token,
            Net::NetworkEventRole::kTcpClient,
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kOk);
    EXPECT_EQ(
        loop.modifyFd(Net::NetworkFdToken{20, 2}, readableWritable()),
        Net::NetworkEventLoopStatus::kStaleToken);
    EXPECT_EQ(
        loop.unregisterFd(Net::NetworkFdToken{20, 2}),
        Net::NetworkEventLoopStatus::kStaleToken);
    EXPECT_EQ(
        loop.unregisterFd(Net::NetworkFdToken{21, 1}),
        Net::NetworkEventLoopStatus::kUnknownFd);

    loop.close();
    EXPECT_EQ(
        loop.registerFd(
            Net::NetworkFdToken{22, 1},
            Net::NetworkEventRole::kTcpClient,
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kClosed);

    std::vector<Net::NetworkEvent> events;
    EXPECT_EQ(
        loop.wait(std::chrono::milliseconds(1), events),
        Net::NetworkEventLoopWaitStatus::kClosed);
}

TEST(NetworkEventLoopTests, WaitReturnsReadyEventsInQueuedOrderAndFiltersStaleEvents) {
    FakeNetworkEventLoop loop;
    const Net::NetworkFdToken tcpToken{30, 1};
    const Net::NetworkFdToken udpToken{31, 1};

    ASSERT_EQ(
        loop.registerFd(
            tcpToken,
            Net::NetworkEventRole::kTcpClient,
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kOk);
    ASSERT_EQ(
        loop.registerFd(
            udpToken,
            Net::NetworkEventRole::kUdpSocket,
            readableWritable()),
        Net::NetworkEventLoopStatus::kOk);

    loop.queueEvent(
        Net::NetworkEvent{
            Net::NetworkFdToken{30, 2},
            Net::NetworkEventRole::kTcpClient,
            Net::NetworkEventMask::kReadable});
    loop.queueEvent(
        Net::NetworkEvent{
            Net::NetworkFdToken{99, 1},
            Net::NetworkEventRole::kTcpClient,
            Net::NetworkEventMask::kReadable});
    loop.queueEvent(
        Net::NetworkEvent{
            tcpToken,
            Net::NetworkEventRole::kTcpClient,
            Net::NetworkEventMask::kWritable});
    loop.queueEvent(
        Net::NetworkEvent{
            udpToken,
            Net::NetworkEventRole::kUdpSocket,
            Net::NetworkEventMask::kWritable});
    loop.queueEvent(
        Net::NetworkEvent{
            tcpToken,
            Net::NetworkEventRole::kTcpClient,
            Net::NetworkEventMask::kReadable});

    std::vector<Net::NetworkEvent> events;
    EXPECT_EQ(
        loop.wait(std::chrono::milliseconds(1), events),
        Net::NetworkEventLoopWaitStatus::kReady);
    ASSERT_EQ(events.size(), 2U);
    EXPECT_EQ(events[0].token, udpToken);
    EXPECT_EQ(events[0].role, Net::NetworkEventRole::kUdpSocket);
    EXPECT_EQ(events[0].readyMask, Net::NetworkEventMask::kWritable);
    EXPECT_EQ(events[1].token, tcpToken);
    EXPECT_EQ(events[1].role, Net::NetworkEventRole::kTcpClient);
    EXPECT_EQ(events[1].readyMask, Net::NetworkEventMask::kReadable);
}

TEST(NetworkEventLoopTests, WaitTimesOutWhenNoRegisteredInterestMatches) {
    FakeNetworkEventLoop loop;
    const Net::NetworkFdToken token{40, 1};
    ASSERT_EQ(
        loop.registerFd(
            token,
            Net::NetworkEventRole::kWakeup,
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kOk);

    loop.queueEvent(
        Net::NetworkEvent{
            token,
            Net::NetworkEventRole::kWakeup,
            Net::NetworkEventMask::kWritable});

    std::vector<Net::NetworkEvent> events;
    EXPECT_EQ(
        loop.wait(std::chrono::milliseconds(1), events),
        Net::NetworkEventLoopWaitStatus::kTimeout);
    EXPECT_TRUE(events.empty());
}

TEST(NetworkEventLoopTests, UnregisterPreventsOldGenerationDeliveryAndAllowsNewGeneration) {
    FakeNetworkEventLoop loop;
    const Net::NetworkFdToken oldToken{50, 1};
    const Net::NetworkFdToken newToken{50, 2};

    ASSERT_EQ(
        loop.registerFd(
            oldToken,
            Net::NetworkEventRole::kTimer,
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kOk);
    ASSERT_EQ(loop.unregisterFd(oldToken), Net::NetworkEventLoopStatus::kOk);
    ASSERT_EQ(
        loop.registerFd(
            newToken,
            Net::NetworkEventRole::kTimer,
            Net::NetworkEventMask::kReadable),
        Net::NetworkEventLoopStatus::kOk);

    loop.queueEvent(
        Net::NetworkEvent{
            oldToken,
            Net::NetworkEventRole::kTimer,
            Net::NetworkEventMask::kReadable});
    loop.queueEvent(
        Net::NetworkEvent{
            newToken,
            Net::NetworkEventRole::kTimer,
            Net::NetworkEventMask::kReadable});

    std::vector<Net::NetworkEvent> events;
    EXPECT_EQ(
        loop.wait(std::chrono::milliseconds(1), events),
        Net::NetworkEventLoopWaitStatus::kReady);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].token, newToken);
    EXPECT_EQ(events[0].role, Net::NetworkEventRole::kTimer);
}
