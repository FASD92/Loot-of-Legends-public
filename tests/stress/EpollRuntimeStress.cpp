#include "Core/Server.hpp"
#include "Net/RudpBattleStartPayload.hpp"
#include "Net/RudpHelloPayload.hpp"
#include "Net/RudpInputCommandPayload.hpp"
#include "Net/RudpPacket.hpp"
#include "Net/TcpPacket.hpp"
#include "Net/UdpSocket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

constexpr size_t kDefaultClientCount = 100;
constexpr size_t kDefaultRoomSize = 2;
constexpr size_t kDefaultDurationSec = 3;
constexpr size_t kDefaultTimeoutMs = 5000;
constexpr uint16_t kClientVersion = 1;
constexpr uint32_t kFirstClientId = 100000;
constexpr uint32_t kHelloSequenceBase = 1000;
constexpr uint32_t kReadySequenceBase = 100000;
constexpr uint32_t kAckSequenceBase = 200000;

struct StressConfig {
    size_t clientCount{kDefaultClientCount};
    size_t roomSize{kDefaultRoomSize};
    std::chrono::seconds duration{kDefaultDurationSec};
    std::chrono::milliseconds timeout{kDefaultTimeoutMs};
};

struct StressMetrics {
    std::vector<uint64_t> connectWelcomeUs;
    std::vector<uint64_t> roomSetupUs;
    std::vector<uint64_t> helloToBattleStartUs;
    size_t battleStartReceived{0};
    size_t ackSent{0};
};

struct StressClient {
    int tcpFd{-1};
    uint64_t sessionId{0};
    uint32_t clientId{0};
    Clock::time_point helloSentAt{};
    std::unique_ptr<Net::UdpSocket> udpSocket;

    StressClient() = default;
    StressClient(const StressClient&) = delete;
    StressClient& operator=(const StressClient&) = delete;

    StressClient(StressClient&& other) noexcept
        : tcpFd(std::exchange(other.tcpFd, -1)),
          sessionId(other.sessionId),
          clientId(other.clientId),
          helloSentAt(other.helloSentAt),
          udpSocket(std::move(other.udpSocket)) {}

    StressClient& operator=(StressClient&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        closeTcp();
        tcpFd = std::exchange(other.tcpFd, -1);
        sessionId = other.sessionId;
        clientId = other.clientId;
        helloSentAt = other.helloSentAt;
        udpSocket = std::move(other.udpSocket);
        return *this;
    }

    ~StressClient() {
        closeTcp();
    }

    void closeTcp() {
        if (tcpFd >= 0) {
            ::close(tcpFd);
            tcpFd = -1;
        }
    }
};

struct StressRoom {
    uint32_t roomId{0};
    size_t firstClient{0};
    size_t secondClient{0};
};

class RunningServer {
public:
    bool start() {
        if (!server_.start()) {
            return false;
        }
        thread_ = std::thread([this]() { server_.run(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return server_.boundPort() != 0 && server_.udpBoundPort() != 0;
    }

    ~RunningServer() {
        server_.requestStop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    Core::Server& server() {
        return server_;
    }

private:
    Core::Server server_{0};
    std::thread thread_;
};

void writeU32BE(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes[offset] = static_cast<uint8_t>((value >> 24) & 0xFF);
    bytes[offset + 1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    bytes[offset + 2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    bytes[offset + 3] = static_cast<uint8_t>(value & 0xFF);
}

bool parseSizeEnv(const char* name, size_t fallback, size_t& outValue) {
    const char* raw = std::getenv(name);
    if (raw == nullptr || raw[0] == '\0') {
        outValue = fallback;
        return true;
    }

    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(raw, &end, 10);
    if (end == raw || *end != '\0' ||
        parsed > static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
        std::cerr << "invalid " << name << "=" << raw << "\n";
        return false;
    }

    outValue = static_cast<size_t>(parsed);
    return true;
}

bool readConfig(StressConfig& config) {
    size_t durationSec = kDefaultDurationSec;
    size_t timeoutMs = kDefaultTimeoutMs;
    if (!parseSizeEnv("LOL_STRESS_CLIENTS", kDefaultClientCount, config.clientCount) ||
        !parseSizeEnv("LOL_STRESS_ROOM_SIZE", kDefaultRoomSize, config.roomSize) ||
        !parseSizeEnv("LOL_STRESS_DURATION_SEC", kDefaultDurationSec, durationSec) ||
        !parseSizeEnv("LOL_STRESS_TIMEOUT_MS", kDefaultTimeoutMs, timeoutMs)) {
        return false;
    }

    if (config.clientCount == 0 || config.roomSize == 0 ||
        config.clientCount % config.roomSize != 0) {
        std::cerr << "LOL_STRESS_CLIENTS must be positive and divisible by "
                  << "LOL_STRESS_ROOM_SIZE\n";
        return false;
    }
    if (config.roomSize != 2) {
        std::cerr << "Plan E6 stress uses the current two-player Ready/BattleStart "
                  << "room contract; LOL_STRESS_ROOM_SIZE must be 2\n";
        return false;
    }
    if (timeoutMs == 0) {
        std::cerr << "LOL_STRESS_TIMEOUT_MS must be positive\n";
        return false;
    }

    config.duration = std::chrono::seconds(durationSec);
    config.timeout = std::chrono::milliseconds(timeoutMs);
    return true;
}

uint64_t elapsedUs(Clock::time_point start, Clock::time_point end) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}

uint64_t percentile(std::vector<uint64_t> values, double p) {
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const double scaled = p * static_cast<double>(values.size() - 1);
    const size_t index = static_cast<size_t>(scaled + 0.5);
    return values[std::min(index, values.size() - 1)];
}

bool waitUntil(
    const std::function<bool()>& predicate,
    std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

int remainingTimeoutMs(Clock::time_point deadline) {
    const auto now = Clock::now();
    if (now >= deadline) {
        return 0;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    return static_cast<int>(std::max<int64_t>(1, remaining.count()));
}

bool recvAllUntil(int fd, uint8_t* buffer, size_t size, Clock::time_point deadline) {
    size_t receivedTotal = 0;
    while (receivedTotal < size) {
        pollfd readPoll{};
        readPoll.fd = fd;
        readPoll.events = POLLIN;
        const int pollResult = ::poll(&readPoll, 1, remainingTimeoutMs(deadline));
        if (pollResult <= 0) {
            return false;
        }
        if ((readPoll.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            return false;
        }

        const ssize_t received =
            ::recv(fd, buffer + receivedTotal, size - receivedTotal, 0);
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received <= 0) {
            return false;
        }
        receivedTotal += static_cast<size_t>(received);
    }
    return true;
}

bool sendAll(int fd, const uint8_t* data, size_t size) {
    size_t sentTotal = 0;
    while (sentTotal < size) {
        const ssize_t sent = ::send(fd, data + sentTotal, size - sentTotal, 0);
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent <= 0) {
            return false;
        }
        sentTotal += static_cast<size_t>(sent);
    }
    return true;
}

int connectToServer(uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

bool recvTcpPacketUntil(
    int fd,
    std::vector<uint8_t>& outPacket,
    Clock::time_point deadline) {
    std::array<uint8_t, Net::kTcpHeaderSize> headerBytes{};
    if (!recvAllUntil(fd, headerBytes.data(), headerBytes.size(), deadline)) {
        return false;
    }

    Net::TcpPacketHeader header;
    if (!Net::peekTcpPacketHeader(headerBytes.data(), headerBytes.size(), header) ||
        header.size < Net::kTcpHeaderSize ||
        header.size > Net::kMaxTcpPacketSize) {
        return false;
    }

    outPacket.assign(header.size, 0);
    std::copy(headerBytes.begin(), headerBytes.end(), outPacket.begin());
    if (header.size == Net::kTcpHeaderSize) {
        return true;
    }
    return recvAllUntil(
        fd,
        outPacket.data() + Net::kTcpHeaderSize,
        header.size - Net::kTcpHeaderSize,
        deadline);
}

bool recvTcpPacketOfTypeUntil(
    int fd,
    Net::TcpPacketType expectedType,
    std::vector<uint8_t>& outPacket,
    Clock::time_point deadline) {
    while (Clock::now() < deadline) {
        std::vector<uint8_t> packet;
        if (!recvTcpPacketUntil(fd, packet, deadline)) {
            return false;
        }
        Net::TcpPacketHeader header;
        if (!Net::peekTcpPacketHeader(packet.data(), packet.size(), header)) {
            return false;
        }
        if (header.type == expectedType) {
            outPacket = std::move(packet);
            return true;
        }
    }
    return false;
}

bool receiveWelcome(
    int fd,
    uint64_t& outSessionId,
    std::chrono::milliseconds timeout) {
    std::array<uint8_t, Net::kWelcomePacketSize> packet{};
    const auto deadline = Clock::now() + timeout;
    if (!recvAllUntil(fd, packet.data(), packet.size(), deadline)) {
        return false;
    }
    Net::TcpPacketHeader header;
    return Net::parseWelcomePacket(packet.data(), packet.size(), header, outSessionId);
}

bool receiveClientListSnapshot(
    int fd,
    std::chrono::milliseconds timeout) {
    std::vector<uint8_t> packet;
    if (!recvTcpPacketOfTypeUntil(
            fd,
            Net::TcpPacketType::kClientListSnapshot,
            packet,
            Clock::now() + timeout)) {
        return false;
    }
    Net::TcpPacketHeader header;
    std::vector<uint64_t> sessionIds;
    return Net::parseClientListSnapshotPacket(
        packet.data(),
        packet.size(),
        header,
        sessionIds);
}

bool receiveCreateRoomResponse(
    int fd,
    uint32_t& outRoomId,
    std::chrono::milliseconds timeout) {
    std::vector<uint8_t> packet;
    if (!recvTcpPacketOfTypeUntil(
            fd,
            Net::TcpPacketType::kCreateRoomResponse,
            packet,
            Clock::now() + timeout)) {
        return false;
    }
    Net::TcpPacketHeader header;
    uint16_t playerCount = 0;
    return Net::parseCreateRoomResponsePacket(
               packet.data(),
               packet.size(),
               header,
               outRoomId,
               playerCount) &&
           outRoomId != 0 && playerCount == 1;
}

bool receiveJoinRoomResponse(
    int fd,
    uint32_t roomId,
    std::chrono::milliseconds timeout) {
    std::vector<uint8_t> packet;
    if (!recvTcpPacketOfTypeUntil(
            fd,
            Net::TcpPacketType::kJoinRoomResponse,
            packet,
            Clock::now() + timeout)) {
        return false;
    }
    Net::TcpPacketHeader header;
    uint32_t joinedRoomId = 0;
    uint16_t playerCount = 0;
    return Net::parseJoinRoomResponsePacket(
               packet.data(),
               packet.size(),
               header,
               joinedRoomId,
               playerCount) &&
           joinedRoomId == roomId && playerCount == 2;
}

bool sendCreateRoomRequest(int fd) {
    std::vector<uint8_t> packet;
    return Net::serializeCreateRoomRequestPacket("Room", Net::kCreateRoomMaxCapacity, packet) &&
           sendAll(fd, packet.data(), packet.size());
}

bool sendJoinRoomRequest(int fd, uint32_t roomId) {
    std::array<uint8_t, Net::kRoomIdPacketSize> packet{};
    return Net::serializeJoinRoomRequestPacket(roomId, packet) &&
           sendAll(fd, packet.data(), packet.size());
}

Net::UdpEndpoint loopbackUdpEndpoint(uint16_t port) {
    Net::UdpEndpoint endpoint;
    auto* addr = reinterpret_cast<sockaddr_in6*>(&endpoint.addr);
    addr->sin6_family = AF_INET6;
    addr->sin6_addr = in6addr_loopback;
    addr->sin6_port = htons(port);
    endpoint.addrLen = sizeof(sockaddr_in6);
    return endpoint;
}

Net::RudpPacketHeader rudpHeader(
    Net::RudpChannelId channel,
    Net::RudpPacketType type,
    uint32_t sequence,
    uint8_t flags = 0) {
    Net::RudpPacketHeader header;
    header.flags = flags;
    header.channelId = static_cast<uint8_t>(channel);
    header.packetType = static_cast<uint16_t>(type);
    header.sequence = sequence;
    return header;
}

bool serializeRudp(
    const Net::RudpPacketHeader& header,
    const std::vector<uint8_t>& payload,
    std::vector<uint8_t>& outPacket) {
    return Net::serializeRudpPacket(header, payload, outPacket);
}

std::vector<uint8_t> readyPayload(uint32_t clientId) {
    std::vector<uint8_t> payload(Net::kRudpInputCommandPrefixSize, 0);
    writeU32BE(payload, 0, clientId);
    writeU32BE(payload, 4, 1);
    payload[8] = static_cast<uint8_t>(Net::RudpInputCommandOp::kReady);
    payload[9] = 0;
    return payload;
}

bool sendRudpHello(
    StressClient& client,
    uint16_t udpPort,
    uint32_t sequence) {
    std::vector<uint8_t> payload;
    if (!Net::serializeRudpHelloPayload(
            Net::RudpHelloPayload{kClientVersion, client.clientId, client.sessionId},
            payload)) {
        return false;
    }
    std::vector<uint8_t> packet;
    if (!serializeRudp(
            rudpHeader(Net::RudpChannelId::kControl, Net::RudpPacketType::kHello, sequence),
            payload,
            packet)) {
        return false;
    }
    client.helloSentAt = Clock::now();
    return client.udpSocket->sendTo(
        packet.data(),
        packet.size(),
        loopbackUdpEndpoint(udpPort));
}

bool sendReady(StressClient& client, uint16_t udpPort, uint32_t sequence) {
    std::vector<uint8_t> packet;
    if (!serializeRudp(
            rudpHeader(
                Net::RudpChannelId::kInput,
                Net::RudpPacketType::kInputCommand,
                sequence),
            readyPayload(client.clientId),
            packet)) {
        return false;
    }
    return client.udpSocket->sendTo(
        packet.data(),
        packet.size(),
        loopbackUdpEndpoint(udpPort));
}

bool sendAck(StressClient& client, uint16_t udpPort, uint32_t ack, uint32_t sequence) {
    Net::RudpPacketHeader header =
        rudpHeader(
            Net::RudpChannelId::kControl,
            Net::RudpPacketType::kHello,
            sequence,
            Net::kRudpFlagAckOnly);
    header.ack = ack;

    std::vector<uint8_t> packet;
    if (!serializeRudp(header, {}, packet)) {
        return false;
    }
    return client.udpSocket->sendTo(
        packet.data(),
        packet.size(),
        loopbackUdpEndpoint(udpPort));
}

bool receiveBattleStart(
    StressClient& client,
    uint32_t expectedRoomId,
    Net::RudpPacketHeader& outHeader,
    std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    std::array<uint8_t, Net::kMaxRudpPacketSize> buffer{};
    while (Clock::now() < deadline) {
        Net::UdpEndpoint endpoint;
        const ssize_t received =
            client.udpSocket->receiveFrom(buffer.data(), buffer.size(), endpoint);
        if (received < 0) {
            return false;
        }
        if (received == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        Net::RudpPacketHeader header;
        std::vector<uint8_t> payload;
        if (!Net::parseRudpPacket(
                buffer.data(),
                static_cast<size_t>(received),
                header,
                payload)) {
            continue;
        }
        if (header.flags != Net::kRudpFlagReliable ||
            header.channelId != static_cast<uint8_t>(Net::RudpChannelId::kEvent) ||
            header.packetType != static_cast<uint16_t>(Net::RudpPacketType::kBattleStart)) {
            continue;
        }

        Net::RudpBattleStartPayload battleStart;
        if (!Net::parseRudpBattleStartPayload(
                payload.data(),
                payload.size(),
                battleStart) ||
            battleStart.roomId != expectedRoomId) {
            return false;
        }

        outHeader = header;
        return true;
    }
    return false;
}

bool connectClient(
    RunningServer& runningServer,
    StressClient& client,
    size_t clientIndex,
    const StressConfig& config,
    StressMetrics& metrics) {
    const auto startedAt = Clock::now();
    client.tcpFd = connectToServer(runningServer.server().boundPort());
    if (client.tcpFd < 0) {
        std::cerr << "failed to connect client " << clientIndex << "\n";
        return false;
    }
    client.clientId = kFirstClientId + static_cast<uint32_t>(clientIndex);
    client.udpSocket = std::make_unique<Net::UdpSocket>();
    if (!client.udpSocket->open(0)) {
        std::cerr << "failed to open UDP socket for client " << clientIndex << "\n";
        return false;
    }
    if (!receiveWelcome(client.tcpFd, client.sessionId, config.timeout)) {
        std::cerr << "failed to receive welcome for client " << clientIndex << "\n";
        return false;
    }
    metrics.connectWelcomeUs.push_back(elapsedUs(startedAt, Clock::now()));

    if (!receiveClientListSnapshot(client.tcpFd, config.timeout)) {
        std::cerr << "failed to receive client list for client " << clientIndex << "\n";
        return false;
    }
    return true;
}

bool setupRoomPair(
    RunningServer& runningServer,
    std::vector<StressClient>& clients,
    std::vector<StressRoom>& rooms,
    const StressConfig& config,
    StressMetrics& metrics,
    size_t roomIndex) {
    const size_t firstIndex = clients.size();
    clients.emplace_back();
    if (!connectClient(runningServer, clients.back(), firstIndex, config, metrics)) {
        return false;
    }

    const size_t secondIndex = clients.size();
    clients.emplace_back();
    if (!connectClient(runningServer, clients.back(), secondIndex, config, metrics)) {
        return false;
    }

    const auto setupStartedAt = Clock::now();
    if (!sendCreateRoomRequest(clients[firstIndex].tcpFd)) {
        std::cerr << "failed to send create room for room " << roomIndex << "\n";
        return false;
    }

    uint32_t roomId = 0;
    if (!receiveCreateRoomResponse(clients[firstIndex].tcpFd, roomId, config.timeout)) {
        std::cerr << "failed to receive create room response for room " << roomIndex << "\n";
        return false;
    }

    if (!sendJoinRoomRequest(clients[secondIndex].tcpFd, roomId)) {
        std::cerr << "failed to send join room for room " << roomIndex << "\n";
        return false;
    }
    if (!receiveJoinRoomResponse(clients[secondIndex].tcpFd, roomId, config.timeout)) {
        std::cerr << "failed to receive join room response for room " << roomIndex << "\n";
        return false;
    }

    metrics.roomSetupUs.push_back(elapsedUs(setupStartedAt, Clock::now()));
    rooms.push_back(StressRoom{roomId, firstIndex, secondIndex});
    return true;
}

void printLatencySummary(const char* name, const std::vector<uint64_t>& values) {
    std::cout << name
              << " count=" << values.size()
              << " p50_us=" << percentile(values, 0.50)
              << " p95_us=" << percentile(values, 0.95)
              << "\n";
}

bool runStress(const StressConfig& config) {
    RunningServer runningServer;
    if (!runningServer.start()) {
        std::cerr << "failed to start server\n";
        return false;
    }

    const uint16_t tcpPort = runningServer.server().boundPort();
    const uint16_t udpPort = runningServer.server().udpBoundPort();
    std::cout << "Plan E6 Linux epoll stress start"
              << " clients=" << config.clientCount
              << " roomSize=" << config.roomSize
              << " rooms=" << (config.clientCount / config.roomSize)
              << " durationSec=" << config.duration.count()
              << " timeoutMs=" << config.timeout.count()
              << " tcpPort=" << tcpPort
              << " udpPort=" << udpPort
              << "\n";

    StressMetrics metrics;
    std::vector<StressClient> clients;
    clients.reserve(config.clientCount);
    std::vector<StressRoom> rooms;
    rooms.reserve(config.clientCount / config.roomSize);

    const auto totalStartedAt = Clock::now();
    for (size_t roomIndex = 0; roomIndex < config.clientCount / config.roomSize; ++roomIndex) {
        if (!setupRoomPair(runningServer, clients, rooms, config, metrics, roomIndex)) {
            return false;
        }
    }

    for (size_t i = 0; i < clients.size(); ++i) {
        if (!sendRudpHello(
                clients[i],
                udpPort,
                kHelloSequenceBase + static_cast<uint32_t>(i))) {
            std::cerr << "failed to send RUDP Hello for client " << i << "\n";
            return false;
        }
    }
    if (!waitUntil(
            [&runningServer, expected = clients.size()]() {
                return runningServer.server().rudpBindingCount() >= expected;
            },
            config.timeout)) {
        std::cerr << "RUDP binding count did not reach " << clients.size() << "\n";
        return false;
    }

    for (size_t i = 0; i < clients.size(); ++i) {
        if (!sendReady(
                clients[i],
                udpPort,
                kReadySequenceBase + static_cast<uint32_t>(i))) {
            std::cerr << "failed to send RUDP Ready for client " << i << "\n";
            return false;
        }
    }
    if (!waitUntil(
            [&runningServer, expected = clients.size()]() {
                const Core::RudpServerBindingStats stats =
                    runningServer.server().rudpBindingStats();
                return stats.inputSequenceAccepted >= expected &&
                       runningServer.server().rudpReliableEventPendingCount() >= expected;
            },
            config.timeout)) {
        const Core::RudpServerBindingStats stats =
            runningServer.server().rudpBindingStats();
        std::cerr << "RUDP Ready did not produce pending BattleStart events"
                  << " accepted=" << stats.inputSequenceAccepted
                  << " pending=" << runningServer.server().rudpReliableEventPendingCount()
                  << "\n";
        return false;
    }

    for (size_t roomIndex = 0; roomIndex < rooms.size(); ++roomIndex) {
        const StressRoom& room = rooms[roomIndex];
        const std::array<size_t, 2> roomClients{room.firstClient, room.secondClient};
        for (const size_t clientIndex : roomClients) {
            Net::RudpPacketHeader battleHeader;
            if (!receiveBattleStart(
                    clients[clientIndex],
                    room.roomId,
                    battleHeader,
                    config.timeout)) {
                std::cerr << "failed to receive BattleStart for client "
                          << clientIndex << " room=" << room.roomId << "\n";
                return false;
            }
            ++metrics.battleStartReceived;
            metrics.helloToBattleStartUs.push_back(
                elapsedUs(clients[clientIndex].helloSentAt, Clock::now()));
            if (!sendAck(
                    clients[clientIndex],
                    udpPort,
                    battleHeader.sequence,
                    kAckSequenceBase + static_cast<uint32_t>(clientIndex))) {
                std::cerr << "failed to send ACK for client " << clientIndex << "\n";
                return false;
            }
            ++metrics.ackSent;
        }
    }

    if (!waitUntil(
            [&runningServer]() {
                return runningServer.server().rudpReliableEventPendingCount() == 0U;
            },
            config.timeout)) {
        std::cerr << "RUDP reliable event pending queue did not drain"
                  << " pending=" << runningServer.server().rudpReliableEventPendingCount()
                  << "\n";
        return false;
    }

    if (config.duration.count() > 0) {
        std::this_thread::sleep_for(config.duration);
    }

    const auto totalEndedAt = Clock::now();
    const Core::RudpServerBindingStats bindingStats =
        runningServer.server().rudpBindingStats();
    const Core::RudpServerDrainStats drainStats =
        runningServer.server().rudpDrainStats();
    const Core::RudpServerRetransmissionStats retransmissionStats =
        runningServer.server().rudpRetransmissionStats();

    std::cout << "Plan E6 Linux epoll stress result"
              << " total_us=" << elapsedUs(totalStartedAt, totalEndedAt)
              << " activeConnections=" << runningServer.server().activeConnectionCount()
              << " sessions=" << runningServer.server().sessionCount()
              << " rudpBindings=" << runningServer.server().rudpBindingCount()
              << " battleStartReceived=" << metrics.battleStartReceived
              << " ackSent=" << metrics.ackSent
              << " pendingReliable=" << runningServer.server().rudpReliableEventPendingCount()
              << "\n";
    std::cout << "RUDP binding stats"
              << " helloReceived=" << bindingStats.helloReceived
              << " bound=" << bindingStats.bound
              << " inputDecoded=" << bindingStats.inputDecoded
              << " inputSequenceAccepted=" << bindingStats.inputSequenceAccepted
              << " inputDecodeFailed=" << bindingStats.inputDecodeFailed
              << "\n";
    std::cout << "RUDP drain/retransmission stats"
              << " attempted=" << drainStats.attempted
              << " delivered=" << drainStats.delivered
              << " ackOnly=" << drainStats.ackOnly
              << " malformed=" << drainStats.malformed
              << " duplicate=" << drainStats.duplicate
              << " retransmitted=" << retransmissionStats.resent
              << " droppedPeers=" << retransmissionStats.droppedPeers
              << "\n";
    printLatencySummary("connect_welcome", metrics.connectWelcomeUs);
    printLatencySummary("room_setup", metrics.roomSetupUs);
    printLatencySummary("hello_to_battle_start", metrics.helloToBattleStartUs);

    const bool hardGatePassed =
        runningServer.server().activeConnectionCount() == config.clientCount &&
        runningServer.server().sessionCount() == config.clientCount &&
        runningServer.server().rudpBindingCount() == config.clientCount &&
        metrics.battleStartReceived == config.clientCount &&
        metrics.ackSent == config.clientCount &&
        runningServer.server().rudpReliableEventPendingCount() == 0U &&
        bindingStats.inputSequenceAccepted >= config.clientCount &&
        drainStats.ackOnly >= config.clientCount;
    if (!hardGatePassed) {
        std::cerr << "Plan E6 hard gate failed\n";
        return false;
    }

    std::cout << "Plan E6 hard gate passed\n";
    return true;
}
}  // namespace

int main() {
    StressConfig config;
    if (!readConfig(config)) {
        return 2;
    }

    return runStress(config) ? 0 : 1;
}
