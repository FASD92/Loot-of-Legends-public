#include "Core/Server.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iterator>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "Core/RudpInputCommandRoomEventTranslator.hpp"
#include "Core/Session.hpp"
#include "Game/RoomActor.hpp"
#include "Net/RudpBattleStartPayload.hpp"
#include "Net/RudpGameEventPayload.hpp"
#include "Net/RudpHelloPayload.hpp"
#include "Net/RudpInputCommandPayload.hpp"
#include "Net/RudpStateSnapshotPayload.hpp"
#include "Net/TcpPacket.hpp"
#include "Util/Time.hpp"
#if defined(__linux__)
#include <errno.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "Platform/Linux/EpollEventLoop.hpp"
#endif

namespace {
constexpr std::chrono::milliseconds kSessionTimeout(10000);
constexpr std::chrono::milliseconds kLoopSleep(1);
constexpr size_t kReceiveBufferSize = 512;
constexpr size_t kRudpMaxPacketsPerTick = 64;
constexpr size_t kServerRoomEventQueueCapacity = 128;
constexpr size_t kTcpOutboundPendingLimit = 256 * 1024;
constexpr std::chrono::milliseconds kRudpSnapshotInterval(100);
constexpr uint64_t kTcpListenerGeneration = 1;
#if defined(__linux__)
constexpr uint64_t kUdpSocketGeneration = 2;
constexpr uint64_t kRuntimeTimerGeneration = 3;
constexpr uint64_t kRuntimeWakeupGeneration = 4;
constexpr std::chrono::milliseconds kLinuxEpollWaitForever(-1);
#endif

uint64_t currentUnixTimeMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void markClientForDisconnect(std::vector<int>& disconnectedClients, int clientFd) {
    if (std::find(disconnectedClients.begin(), disconnectedClients.end(), clientFd) ==
        disconnectedClients.end()) {
        disconnectedClients.push_back(clientFd);
    }
}

bool isRoomMember(const Game::RoomCommandResult& result, uint64_t sessionId) {
    return std::find(
               result.playerSessionIds.begin(),
               result.playerSessionIds.end(),
               sessionId) != result.playerSessionIds.end();
}

bool buildRudpBattleStartPayload(
    const Game::RoomCommandResult& result,
    Net::RudpBattleStartPayload& outPayload) {
    outPayload = Net::RudpBattleStartPayload{};
    if (result.room.roomId == 0 || result.playerSessionIds.size() != 2u) {
        return false;
    }

    const uint64_t firstSessionId = result.playerSessionIds[0];
    const uint64_t secondSessionId = result.playerSessionIds[1];
    if (firstSessionId == 0 || secondSessionId == 0 ||
        firstSessionId == secondSessionId) {
        return false;
    }

    outPayload.roomId = result.room.roomId;
    outPayload.playerASessionId = std::min(firstSessionId, secondSessionId);
    outPayload.playerBSessionId = std::max(firstSessionId, secondSessionId);
    return true;
}

std::string battleStartLogicalKey(const Net::RudpBattleStartPayload& payload) {
    return "BattleStart:" + std::to_string(payload.roomId) + ":" +
        std::to_string(payload.playerASessionId) + ":" +
        std::to_string(payload.playerBSessionId);
}

std::string monsterDeathLogicalKey(
    const Net::RudpMonsterDeathGameEventPayload& payload) {
    return "MonsterDeath:" + std::to_string(payload.roomId) + ":" +
        std::to_string(payload.monsterId);
}

std::string lootResolvedLogicalKey(
    const Net::RudpLootResolvedGameEventPayload& payload) {
    return "LootResolved:" + std::to_string(payload.roomId) + ":" +
        std::to_string(payload.dropId);
}

bool buildRudpReliableEventPacket(
    const Net::RudpReliableEventDescriptor& descriptor,
    uint32_t sequence,
    const std::vector<uint8_t>& payloadBytes,
    std::vector<uint8_t>& outPacket) {
    Net::RudpPacketHeader header;
    header.flags = Net::kRudpFlagReliable;
    header.channelId = descriptor.channelId;
    header.packetType = descriptor.packetType;
    header.sequence = sequence;
    header.ack = 0;
    header.ackBits = 0;
    return Net::serializeRudpPacket(header, payloadBytes, outPacket);
}

bool buildRudpUnreliableSnapshotPacket(
    uint32_t sequence,
    const std::vector<uint8_t>& payloadBytes,
    std::vector<uint8_t>& outPacket) {
    Net::RudpPacketHeader header;
    header.flags = 0;
    header.channelId = static_cast<uint8_t>(Net::RudpChannelId::kSnapshot);
    header.packetType = static_cast<uint16_t>(Net::RudpPacketType::kStateSnapshot);
    header.sequence = sequence;
    header.ack = 0;
    header.ackBits = 0;
    return Net::serializeRudpPacket(header, payloadBytes, outPacket);
}

uint32_t elapsedMillisClamped(Util::TimePoint previous, Util::TimePoint current) {
    if (current <= previous) {
        return 0;
    }

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(current - previous);
    if (elapsed.count() <= 0) {
        return 0;
    }

    const uint64_t elapsedMs = static_cast<uint64_t>(elapsed.count());
    if (elapsedMs > std::numeric_limits<uint32_t>::max()) {
        return std::numeric_limits<uint32_t>::max();
    }
    return static_cast<uint32_t>(elapsedMs);
}

std::vector<Net::TcpDropEntry> toTcpDropEntries(const std::vector<Game::Drop>& drops) {
    std::vector<Net::TcpDropEntry> entries;
    entries.reserve(drops.size());
    for (const Game::Drop& drop : drops) {
        entries.push_back(Net::TcpDropEntry{drop.dropId, drop.itemId, drop.quantity});
    }
    return entries;
}

std::vector<Net::TcpInventoryEntry> toTcpInventoryEntries(
    const std::vector<Game::InventoryEntry>& entries) {
    std::vector<Net::TcpInventoryEntry> tcpEntries;
    tcpEntries.reserve(entries.size());
    for (const Game::InventoryEntry& entry : entries) {
        tcpEntries.push_back(Net::TcpInventoryEntry{entry.itemId, entry.quantity});
    }
    return tcpEntries;
}

Net::TcpLootRejectReason toTcpLootRejectReason(Game::LootRejectReason reason) {
    switch (reason) {
    case Game::LootRejectReason::kAlreadyClaimed:
        return Net::TcpLootRejectReason::kAlreadyClaimed;
    case Game::LootRejectReason::kOverweight:
        return Net::TcpLootRejectReason::kOverweight;
    case Game::LootRejectReason::kNone:
    default:
        return Net::TcpLootRejectReason::kNone;
    }
}
// Game 도메인 정산 결과를 TCP 패킷 구조로 변환하는 어댑터
Net::TcpSettlementReason toTcpSettlementReason(Game::SettlementReason reason) {
    switch (reason) {
    case Game::SettlementReason::kDisconnect:
        return Net::TcpSettlementReason::kDisconnect;
    case Game::SettlementReason::kServerShutdown:
        return Net::TcpSettlementReason::kServerShutdown;
    case Game::SettlementReason::kForcedClose:
        return Net::TcpSettlementReason::kForcedClose;
    case Game::SettlementReason::kNormal:
    default:
        return Net::TcpSettlementReason::kNormal;
    }
}

std::vector<Net::TcpSettlementInventoryDelta> toTcpSettlementInventoryDeltas(
    const std::vector<Game::SettlementInventoryDelta>& deltas) {
    std::vector<Net::TcpSettlementInventoryDelta> tcpDeltas;
    tcpDeltas.reserve(deltas.size());
    for (const Game::SettlementInventoryDelta& delta : deltas) {
        tcpDeltas.push_back(
            Net::TcpSettlementInventoryDelta{
                delta.itemId,
                delta.quantityDelta,
                delta.sourceDropId});
    }
    return tcpDeltas;
}

Net::TcpSettlementResult toTcpSettlementResult(const Game::SettlementResult& settlement) {
    Net::TcpSettlementResult tcpSettlement;
    tcpSettlement.settlementId = settlement.settlementId;
    tcpSettlement.sessionId = settlement.sessionId;
    tcpSettlement.accountId = settlement.accountId;
    tcpSettlement.roomId = settlement.roomId;
    tcpSettlement.startedAtUnixMs = settlement.startedAtUnixMs;
    tcpSettlement.finishedAtUnixMs = settlement.finishedAtUnixMs;
    tcpSettlement.goldDelta = settlement.goldDelta;
    tcpSettlement.reason = toTcpSettlementReason(settlement.reason);
    tcpSettlement.inventoryDeltas =
        toTcpSettlementInventoryDeltas(settlement.inventoryDeltas);
    return tcpSettlement;
}

// 게임 룸 로직에서 나온 에러를 네트워크 프로토콜용 에러 코드로 변환하는 어댑터
Net::TcpErrorCode toTcpErrorCode(Game::RoomCommandError error) {
    switch (error) {
    case Game::RoomCommandError::kFull:
        return Net::TcpErrorCode::kFull;
    case Game::RoomCommandError::kNotFound:
        return Net::TcpErrorCode::kNotFound;
    case Game::RoomCommandError::kAlreadyInRoom:
        return Net::TcpErrorCode::kAlreadyInRoom;
    case Game::RoomCommandError::kNotInRoom:
        return Net::TcpErrorCode::kNotInRoom;
    case Game::RoomCommandError::kNone:
    default:
        return Net::TcpErrorCode::kNone;
    }
}

Net::TcpPacketType requestTypeFromRoomEventType(Game::RoomEventType type) {
    switch (type) {
    case Game::RoomEventType::kReady:
        return Net::TcpPacketType::kReadyRoomRequest;
    case Game::RoomEventType::kMonsterDeath:
        return Net::TcpPacketType::kMonsterDeathRequest;
    case Game::RoomEventType::kClickLoot:
        return Net::TcpPacketType::kClickLootRequest;
    }

    return Net::TcpPacketType::kReadyRoomRequest;
}

Net::TcpErrorCode toTcpErrorCode(Game::RoomEventDispatcherEnqueueStatus status) {
    switch (status) {
    case Game::RoomEventDispatcherEnqueueStatus::kRejectedBackpressure:
    case Game::RoomEventDispatcherEnqueueStatus::kRejectedShutdown:
        return Net::TcpErrorCode::kFull;
    case Game::RoomEventDispatcherEnqueueStatus::kRejectedUnknownRoom:
        return Net::TcpErrorCode::kNotFound;
    case Game::RoomEventDispatcherEnqueueStatus::kEnqueued:
    default:
        return Net::TcpErrorCode::kNone;
    }
}

Game::RoomCommandResult roomCommandResultFromOutboundEnvelope(
    const Game::OutboundEnvelope& envelope) {
    Game::RoomCommandResult result(
        envelope.error == Game::RoomCommandError::kNone,
        envelope.error,
        envelope.room,
        envelope.playerSessionIds,
        false,
        false,
        false,
        envelope.monster,
        envelope.drops);
    result.lootRejectReason = envelope.lootRejectReason;
    result.winnerSessionId = envelope.winnerSessionId;
    result.drop = envelope.drop;
    result.inventory = envelope.inventory;
    return result;
}

Net::NetworkEventMask tcpClientInterestMask(bool writable) {
    Net::NetworkEventMask mask =
        Net::NetworkEventMask::kReadable |
        Net::NetworkEventMask::kError |
        Net::NetworkEventMask::kHangup;
    if (writable) {
        mask |= Net::NetworkEventMask::kWritable;
    }
    return mask;
}

#if defined(__linux__)
timespec toTimespec(std::chrono::milliseconds duration) {
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
    timespec spec{};
    spec.tv_sec = static_cast<time_t>(
        nanoseconds.count() / 1000000000LL);
    spec.tv_nsec = static_cast<long>(
        nanoseconds.count() % 1000000000LL);
    return spec;
}

bool armPeriodicTimerFd(int timerFd, std::chrono::milliseconds interval) {
    if (timerFd < 0 || interval.count() <= 0) {
        return false;
    }

    itimerspec timerSpec{};
    timerSpec.it_value = toTimespec(interval);
    timerSpec.it_interval = toTimespec(interval);
    return ::timerfd_settime(timerFd, 0, &timerSpec, nullptr) == 0;
}

bool drainCounterFd(int fd) {
    uint64_t value = 0;
    while (true) {
        const ssize_t received = ::read(fd, &value, sizeof(value));
        if (received == static_cast<ssize_t>(sizeof(value))) {
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }
        return false;
    }
}

void writeCounterFd(int fd) {
    if (fd < 0) {
        return;
    }

    const uint64_t value = 1;
    while (true) {
        const ssize_t sent = ::write(fd, &value, sizeof(value));
        if (sent == static_cast<ssize_t>(sizeof(value))) {
            return;
        }
        if (sent < 0 && errno == EINTR) {
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        return;
    }
}

#endif
}  // namespace

namespace Core {
Server::Server(uint16_t port)
    : Server(port, kSessionTimeout) {}

Server::Server(uint16_t port, std::chrono::milliseconds rudpPeerTimeout)
    : sessionManager_(kSessionTimeout),
      roomManager_(),
      roomEventDispatcher_(kServerRoomEventQueueCapacity, &roomEventMetrics_),
      rudpPeerRegistry_(rudpPeerTimeout),
      activeConnectionCount_(0),
      sessionCountSnapshot_(0),
      rudpPeerCountSnapshot_(0),
      rudpDrainAttempted_(0),
      rudpDrainDelivered_(0),
      rudpDrainMalformed_(0),
      rudpDrainInvalidEndpoint_(0),
      rudpDrainAckOnly_(0),
      rudpDrainDuplicate_(0),
      rudpDrainTooOld_(0),
      rudpDrainSocketErrors_(0),
      rudpRetransmissionExpired_(0),
      rudpRetransmissionDue_(0),
      rudpRetransmissionResent_(0),
      rudpRetransmissionSendErrors_(0),
      rudpRetransmissionDroppedPeers_(0),
      rudpBindingHelloReceived_(0),
      rudpBindingBound_(0),
      rudpBindingRefreshed_(0),
      rudpBindingUnknownSession_(0),
      rudpBindingConflicts_(0),
      rudpBindingInvalidEndpoint_(0),
      rudpBindingInvalidPayload_(0),
      rudpBindingIgnoredNonHello_(0),
      rudpBindingInputCandidates_(0),
      rudpBindingInputDecoded_(0),
      rudpBindingInputDecodeFailed_(0),
      rudpBindingUnboundInputRejected_(0),
      rudpBindingUnsupportedPacketIgnored_(0),
      rudpBindingInputSequenceAccepted_(0),
      rudpBindingInputSequenceDuplicateRejected_(0),
      rudpBindingInputSequenceStaleRejected_(0),
      rudpBindingInputSequenceAmbiguousRejected_(0),
      rudpBindingInputSequenceInvalidSessionRejected_(0),
      rudpBindingInputNoRoomRejected_(0),
      rudpBindingMoveAccepted_(0),
      rudpBindingMoveDispatched_(0),
      rudpBindingMoveApplyRejected_(0),
      rudpBindingMoveInvalidReservedFlagsRejected_(0),
      rudpBindingMoveRateLimitedRejected_(0),
      rudpBindingCountSnapshot_(0),
      rudpReliableEventTracked_(0),
      rudpReliableEventDuplicateSequence_(0),
      rudpReliableEventDuplicateLogicalEvent_(0),
      rudpReliableEventInvalidSession_(0),
      rudpReliableEventInvalidDescriptor_(0),
      rudpReliableEventInvalidPacketBytes_(0),
      rudpReliableEventPendingCountSnapshot_(0),
      rudpMetaResponseCompletedFirst_(0),
      rudpMetaResponseCompletionDuplicate_(0),
      rudpMetaResponseRetryObserved_(0),
      rudpMetaResponseRetryDuplicate_(0),
      rudpMetaResponseRetryIgnoredAfterCompletion_(0),
      rudpMetaResponseInvalidPayload_(0),
      rudpMetaResponseEnqueued_(0),
      rudpSnapshotBuilt_(0),
      rudpSnapshotSent_(0),
      rudpSnapshotSendErrors_(0),
      rudpSnapshotSkippedNoBoundEndpoint_(0),
      rudpSnapshotSerializeFailed_(0),
      networkEventLoop_(nullptr),
      nextTcpFdGeneration_(1),
      linuxWakeupFd_(-1),
      running_(false),
      port_(port) {}

bool Server::start() {
    if (!listener_.open(port_)) {
        return false;
    }
    if (!udpSocket_.open(port_)) {
        listener_.close();
        running_.store(false);
        return false;
    }
    running_.store(true);
    return true;
}

void Server::run() {
#if defined(__linux__)
    runLinuxEpollLoop();
#else
    runTickLoop();
#endif
}

void Server::runTickLoop() {
    while (running_.load()) {
        tickOnce();
        std::this_thread::sleep_for(kLoopSleep);
    }

    closeAllConnections();
    udpSocket_.close();
}

#if defined(__linux__)
void Server::runLinuxEpollLoop() {
    Net::EpollEventLoop eventLoop;
    networkEventLoop_ = &eventLoop;

    const int timerFd = ::timerfd_create(
        CLOCK_MONOTONIC,
        TFD_NONBLOCK | TFD_CLOEXEC);
    const int wakeupFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (timerFd < 0 || wakeupFd < 0 ||
        !armPeriodicTimerFd(timerFd, kLoopSleep)) {
        running_.store(false);
    }
    linuxWakeupFd_.store(wakeupFd, std::memory_order_release);

    const Net::NetworkEventLoopStatus listenerStatus = eventLoop.registerFd(
        Net::NetworkFdToken{listener_.fd(), kTcpListenerGeneration},
        Net::NetworkEventRole::kTcpListener,
        tcpClientInterestMask(false));
    if (listenerStatus != Net::NetworkEventLoopStatus::kOk) {
        running_.store(false);
    }

    const Net::NetworkEventLoopStatus udpStatus = eventLoop.registerFd(
        Net::NetworkFdToken{udpSocket_.fd(), kUdpSocketGeneration},
        Net::NetworkEventRole::kUdpSocket,
        Net::NetworkEventMask::kReadable |
            Net::NetworkEventMask::kError |
            Net::NetworkEventMask::kHangup);
    if (udpStatus != Net::NetworkEventLoopStatus::kOk) {
        running_.store(false);
    }

    const Net::NetworkEventLoopStatus timerStatus = eventLoop.registerFd(
        Net::NetworkFdToken{timerFd, kRuntimeTimerGeneration},
        Net::NetworkEventRole::kTimer,
        Net::NetworkEventMask::kReadable |
            Net::NetworkEventMask::kError |
            Net::NetworkEventMask::kHangup);
    if (timerStatus != Net::NetworkEventLoopStatus::kOk) {
        running_.store(false);
    }

    const Net::NetworkEventLoopStatus wakeupStatus = eventLoop.registerFd(
        Net::NetworkFdToken{wakeupFd, kRuntimeWakeupGeneration},
        Net::NetworkEventRole::kWakeup,
        Net::NetworkEventMask::kReadable |
            Net::NetworkEventMask::kError |
            Net::NetworkEventMask::kHangup);
    if (wakeupStatus != Net::NetworkEventLoopStatus::kOk) {
        running_.store(false);
    }

    while (running_.load()) {
        std::vector<Net::NetworkEvent> events;
        const Net::NetworkEventLoopWaitStatus waitStatus =
            eventLoop.wait(kLinuxEpollWaitForever, events);
        const Util::TimePoint now = Util::now();

        if (waitStatus == Net::NetworkEventLoopWaitStatus::kReady) {
            processLinuxEpollEvents(events, now);
        } else if (waitStatus == Net::NetworkEventLoopWaitStatus::kClosed ||
                   waitStatus == Net::NetworkEventLoopWaitStatus::kBackendError) {
            running_.store(false);
        }
    }

    closeAllConnections();
    linuxWakeupFd_.store(-1, std::memory_order_release);
    eventLoop.close();
    if (timerFd >= 0) {
        ::close(timerFd);
    }
    if (wakeupFd >= 0) {
        ::close(wakeupFd);
    }
    udpSocket_.close();
    networkEventLoop_ = nullptr;
}

void Server::processLinuxEpollEvents(
    const std::vector<Net::NetworkEvent>& events,
    Util::TimePoint now) {
    std::vector<int> disconnectedClients;
    bool roomListChanged = false;

    auto isDisconnectPending = [&disconnectedClients](int clientFd) {
        return std::find(
                   disconnectedClients.begin(),
                   disconnectedClients.end(),
                   clientFd) != disconnectedClients.end();
    };

    for (const Net::NetworkEvent& event : events) {
        if (event.role == Net::NetworkEventRole::kTcpListener) {
            if (Net::hasAnyNetworkEventMask(
                    event.readyMask,
                    Net::NetworkEventMask::kError |
                        Net::NetworkEventMask::kHangup)) {
                running_.store(false);
                continue;
            }
            if (Net::hasAnyNetworkEventMask(event.readyMask, Net::NetworkEventMask::kReadable)) {
                acceptNewClients(now);
            }
            continue;
        }

        if (event.role == Net::NetworkEventRole::kUdpSocket) {
            if (Net::hasAnyNetworkEventMask(
                    event.readyMask,
                    Net::NetworkEventMask::kError |
                        Net::NetworkEventMask::kHangup)) {
                running_.store(false);
                continue;
            }
            if (Net::hasAnyNetworkEventMask(event.readyMask, Net::NetworkEventMask::kReadable)) {
                processRudpSocket(now);
            }
            continue;
        }

        if (event.role == Net::NetworkEventRole::kTimer) {
            if (Net::hasAnyNetworkEventMask(
                    event.readyMask,
                    Net::NetworkEventMask::kError |
                        Net::NetworkEventMask::kHangup) ||
                !drainCounterFd(event.token.fd)) {
                running_.store(false);
                continue;
            }
            if (Net::hasAnyNetworkEventMask(event.readyMask, Net::NetworkEventMask::kReadable)) {
                processRuntimeTimerMaintenance(now);
            }
            continue;
        }

        if (event.role == Net::NetworkEventRole::kWakeup) {
            if (Net::hasAnyNetworkEventMask(
                    event.readyMask,
                    Net::NetworkEventMask::kError |
                        Net::NetworkEventMask::kHangup) ||
                !drainCounterFd(event.token.fd)) {
                running_.store(false);
            }
            continue;
        }

        if (event.role != Net::NetworkEventRole::kTcpClient) {
            continue;
        }

        auto connectionIt = connections_.find(event.token.fd);
        if (connectionIt == connections_.end() ||
            connectionIt->second->fdGeneration() != event.token.generation) {
            continue;
        }

        ClientConnection& connection = *connectionIt->second;
        if (Net::hasAnyNetworkEventMask(
                event.readyMask,
                Net::NetworkEventMask::kError |
                    Net::NetworkEventMask::kHangup)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            continue;
        }

        if (Net::hasAnyNetworkEventMask(event.readyMask, Net::NetworkEventMask::kReadable)) {
            processReadableTcpClient(
                connection,
                now,
                disconnectedClients,
                roomListChanged,
                true);
        }

        if (!isDisconnectPending(connection.clientFd()) &&
            Net::hasAnyNetworkEventMask(event.readyMask, Net::NetworkEventMask::kWritable)) {
            flushTcpOutbound(connection, disconnectedClients);
        }
    }

    bool roomChangedByDisconnect = false;
    for (int clientFd : disconnectedClients) {
        roomChangedByDisconnect = disconnectClient(clientFd) || roomChangedByDisconnect;
    }

    if (!disconnectedClients.empty() || roomListChanged || roomChangedByDisconnect) {
        broadcastStateSnapshots(
            !disconnectedClients.empty(),
            roomListChanged || roomChangedByDisconnect);
    }
}
#endif

void Server::requestStop() {
    const bool wasRunning = running_.exchange(false);
#if defined(__linux__)
    if (wasRunning) {
        writeCounterFd(linuxWakeupFd_.load(std::memory_order_acquire));
    }
#endif
}

uint16_t Server::boundPort() const {
    return listener_.boundPort();
}

uint16_t Server::udpBoundPort() const {
    return udpSocket_.boundPort();
}

size_t Server::activeConnectionCount() const {
    return activeConnectionCount_.load(std::memory_order_relaxed);
}

size_t Server::sessionCount() const {
    return sessionCountSnapshot_.load(std::memory_order_relaxed);
}

RudpServerDrainStats Server::rudpDrainStats() const {
    RudpServerDrainStats stats;
    stats.attempted = rudpDrainAttempted_.load(std::memory_order_relaxed);
    stats.delivered = rudpDrainDelivered_.load(std::memory_order_relaxed);
    stats.malformed = rudpDrainMalformed_.load(std::memory_order_relaxed);
    stats.invalidEndpoint =
        rudpDrainInvalidEndpoint_.load(std::memory_order_relaxed);
    stats.ackOnly = rudpDrainAckOnly_.load(std::memory_order_relaxed);
    stats.duplicate = rudpDrainDuplicate_.load(std::memory_order_relaxed);
    stats.tooOld = rudpDrainTooOld_.load(std::memory_order_relaxed);
    stats.socketErrors = rudpDrainSocketErrors_.load(std::memory_order_relaxed);
    return stats;
}

RudpServerRetransmissionStats Server::rudpRetransmissionStats() const {
    RudpServerRetransmissionStats stats;
    stats.expired = rudpRetransmissionExpired_.load(std::memory_order_relaxed);
    stats.due = rudpRetransmissionDue_.load(std::memory_order_relaxed);
    stats.resent = rudpRetransmissionResent_.load(std::memory_order_relaxed);
    stats.sendErrors =
        rudpRetransmissionSendErrors_.load(std::memory_order_relaxed);
    stats.droppedPeers =
        rudpRetransmissionDroppedPeers_.load(std::memory_order_relaxed);
    return stats;
}

RudpServerBindingStats Server::rudpBindingStats() const {
    RudpServerBindingStats stats;
    stats.helloReceived = rudpBindingHelloReceived_.load(std::memory_order_relaxed);
    stats.bound = rudpBindingBound_.load(std::memory_order_relaxed);
    stats.refreshed = rudpBindingRefreshed_.load(std::memory_order_relaxed);
    stats.unknownSession =
        rudpBindingUnknownSession_.load(std::memory_order_relaxed);
    stats.conflicts = rudpBindingConflicts_.load(std::memory_order_relaxed);
    stats.invalidEndpoint =
        rudpBindingInvalidEndpoint_.load(std::memory_order_relaxed);
    stats.invalidPayload =
        rudpBindingInvalidPayload_.load(std::memory_order_relaxed);
    stats.ignoredNonHello =
        rudpBindingIgnoredNonHello_.load(std::memory_order_relaxed);
    stats.inputCandidates =
        rudpBindingInputCandidates_.load(std::memory_order_relaxed);
    stats.inputDecoded =
        rudpBindingInputDecoded_.load(std::memory_order_relaxed);
    stats.inputDecodeFailed =
        rudpBindingInputDecodeFailed_.load(std::memory_order_relaxed);
    stats.unboundInputRejected =
        rudpBindingUnboundInputRejected_.load(std::memory_order_relaxed);
    stats.unsupportedPacketIgnored =
        rudpBindingUnsupportedPacketIgnored_.load(std::memory_order_relaxed);
    stats.inputSequenceAccepted =
        rudpBindingInputSequenceAccepted_.load(std::memory_order_relaxed);
    stats.inputSequenceDuplicateRejected =
        rudpBindingInputSequenceDuplicateRejected_.load(std::memory_order_relaxed);
    stats.inputSequenceStaleRejected =
        rudpBindingInputSequenceStaleRejected_.load(std::memory_order_relaxed);
    stats.inputSequenceAmbiguousRejected =
        rudpBindingInputSequenceAmbiguousRejected_.load(std::memory_order_relaxed);
    stats.inputSequenceInvalidSessionRejected =
        rudpBindingInputSequenceInvalidSessionRejected_.load(std::memory_order_relaxed);
    stats.inputNoRoomRejected =
        rudpBindingInputNoRoomRejected_.load(std::memory_order_relaxed);
    stats.moveAccepted =
        rudpBindingMoveAccepted_.load(std::memory_order_relaxed);
    stats.moveDispatched =
        rudpBindingMoveDispatched_.load(std::memory_order_relaxed);
    stats.moveApplyRejected =
        rudpBindingMoveApplyRejected_.load(std::memory_order_relaxed);
    stats.moveInvalidReservedFlagsRejected =
        rudpBindingMoveInvalidReservedFlagsRejected_.load(std::memory_order_relaxed);
    stats.moveRateLimitedRejected =
        rudpBindingMoveRateLimitedRejected_.load(std::memory_order_relaxed);
    return stats;
}

RudpServerReliableEventStats Server::rudpReliableEventStats() const {
    RudpServerReliableEventStats stats;
    stats.tracked = rudpReliableEventTracked_.load(std::memory_order_relaxed);
    stats.duplicateSequence =
        rudpReliableEventDuplicateSequence_.load(std::memory_order_relaxed);
    stats.duplicateLogicalEvent =
        rudpReliableEventDuplicateLogicalEvent_.load(std::memory_order_relaxed);
    stats.invalidSession =
        rudpReliableEventInvalidSession_.load(std::memory_order_relaxed);
    stats.invalidDescriptor =
        rudpReliableEventInvalidDescriptor_.load(std::memory_order_relaxed);
    stats.invalidPacketBytes =
        rudpReliableEventInvalidPacketBytes_.load(std::memory_order_relaxed);
    return stats;
}

RudpServerMetaResponseStats Server::rudpMetaResponseStats() const {
    RudpServerMetaResponseStats stats;
    stats.completedFirst =
        rudpMetaResponseCompletedFirst_.load(std::memory_order_relaxed);
    stats.completionDuplicate =
        rudpMetaResponseCompletionDuplicate_.load(std::memory_order_relaxed);
    stats.retryObserved =
        rudpMetaResponseRetryObserved_.load(std::memory_order_relaxed);
    stats.retryDuplicate =
        rudpMetaResponseRetryDuplicate_.load(std::memory_order_relaxed);
    stats.retryIgnoredAfterCompletion =
        rudpMetaResponseRetryIgnoredAfterCompletion_.load(std::memory_order_relaxed);
    stats.invalidPayload =
        rudpMetaResponseInvalidPayload_.load(std::memory_order_relaxed);
    stats.enqueued = rudpMetaResponseEnqueued_.load(std::memory_order_relaxed);
    return stats;
}

RudpServerSnapshotStats Server::rudpSnapshotStats() const {
    RudpServerSnapshotStats stats;
    stats.built = rudpSnapshotBuilt_.load(std::memory_order_relaxed);
    stats.sent = rudpSnapshotSent_.load(std::memory_order_relaxed);
    stats.sendErrors = rudpSnapshotSendErrors_.load(std::memory_order_relaxed);
    stats.skippedNoBoundEndpoint =
        rudpSnapshotSkippedNoBoundEndpoint_.load(std::memory_order_relaxed);
    stats.serializeFailed =
        rudpSnapshotSerializeFailed_.load(std::memory_order_relaxed);
    return stats;
}

size_t Server::rudpPeerCount() const {
    return rudpPeerCountSnapshot_.load(std::memory_order_relaxed);
}

size_t Server::rudpBindingCount() const {
    return rudpBindingCountSnapshot_.load(std::memory_order_relaxed);
}

size_t Server::rudpReliableEventPendingCount() const {
    return rudpReliableEventPendingCountSnapshot_.load(std::memory_order_relaxed);
}

void Server::tickOnce() {
    Util::TimePoint now = Util::now();
    acceptNewClients(now);
    processActiveConnections(now);
    processRuntimeMaintenance(now);
}

void Server::processRuntimeMaintenance(Util::TimePoint now) {
    processRudpSocket(now);
    processRuntimeTimerMaintenance(now);
}

void Server::processRuntimeTimerMaintenance(Util::TimePoint now) {
    processRudpPeerLifecycle(now);
    processRudpRetransmissions(now);
    processRudpReliableEventRetransmissions(now);
    processRudpMovementSnapshots(now);
    sessionManager_.tick(now);
    sessionCountSnapshot_.store(sessionManager_.size(), std::memory_order_relaxed);
}

void Server::acceptNewClients(Util::TimePoint now) {
    bool membershipChanged = false;
    std::vector<int> disconnectedClients;

    while (true) {
        int clientFd = -1;
        Net::TcpEndpoint endpoint;
        Net::AcceptStatus status = listener_.acceptClient(clientFd, endpoint);
        if (status == Net::AcceptStatus::kWouldBlock) {
            break;
        }
        if (status == Net::AcceptStatus::kError) {
            continue;
        }

        std::string remoteKey = Net::endpointToString(endpoint);
        auto session = sessionManager_.findOrCreate(remoteKey, now);
        if (!session || session->isBlocked()) {
            listener_.closeClient(clientFd);
            continue;
        }

        std::array<uint8_t, Net::kWelcomePacketSize> welcomePacket{};
        session->updateLastHeard(now);
        Net::serializeWelcomePacket(session->sessionId(), welcomePacket);

        auto connection = std::make_unique<ClientConnection>(
            clientFd,
            session->sessionId(),
            remoteKey,
            now);
        connection->setFdGeneration(allocateTcpFdGeneration());
        ClientConnection& storedConnection = *connection;
        connections_.emplace(clientFd, std::move(connection));
        activeConnectionCount_.store(connections_.size(), std::memory_order_relaxed);
        sessionCountSnapshot_.store(sessionManager_.size(), std::memory_order_relaxed);

        if (!registerTcpClientWithEventLoop(storedConnection) ||
            !sendPacketToClient(
                clientFd,
                welcomePacket.data(),
                welcomePacket.size(),
                disconnectedClients)) {
            markClientForDisconnect(disconnectedClients, clientFd);
            continue;
        }

        membershipChanged = true;
    }

    bool roomChangedByDisconnect = false;
    for (int clientFd : disconnectedClients) {
        roomChangedByDisconnect = disconnectClient(clientFd) || roomChangedByDisconnect;
    }

    if (membershipChanged || roomChangedByDisconnect) {
        sessionCountSnapshot_.store(sessionManager_.size(), std::memory_order_relaxed);
        broadcastStateSnapshots(true, roomChangedByDisconnect);
    }
}

void Server::processActiveConnections(Util::TimePoint now) {
    std::vector<int> disconnectedClients;
    bool roomListChanged = false;

    for (const auto& entry : connections_) {
        processReadableTcpClient(
            *entry.second,
            now,
            disconnectedClients,
            roomListChanged,
            false);
    }

    bool roomChangedByDisconnect = false;
    for (int clientFd : disconnectedClients) {
        roomChangedByDisconnect = disconnectClient(clientFd) || roomChangedByDisconnect;
    }

    if (!disconnectedClients.empty() || roomListChanged || roomChangedByDisconnect) {
        broadcastStateSnapshots(
            !disconnectedClients.empty(),
            roomListChanged || roomChangedByDisconnect);
    }
}

void Server::processReadableTcpClient(
    ClientConnection& connection,
    Util::TimePoint now,
    std::vector<int>& disconnectedClients,
    bool& outRoomListChanged,
    bool drainUntilWouldBlock) {
    std::array<uint8_t, kReceiveBufferSize> buffer{};
    std::vector<uint8_t> framedPacket;

    while (true) {
        size_t received = 0;
        Net::ReceiveStatus status = listener_.receiveFromClient(
            connection.clientFd(),
            buffer.data(),
            buffer.size(),
            received);

        if (status == Net::ReceiveStatus::kWouldBlock) {
            return;
        }

        if (status == Net::ReceiveStatus::kClosed || status == Net::ReceiveStatus::kError) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return;
        }

        connection.updateLastHeard(now);
        auto session = sessionManager_.find(connection.remoteKey());
        if (!session) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return;
        }
        session->updateLastHeard(now);

        if (!connection.packetReader().appendBytes(buffer.data(), received)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return;
        }

        while (true) {
            Net::TcpPacketReadResult readResult =
                connection.packetReader().tryReadPacket(framedPacket);
            if (readResult == Net::TcpPacketReadResult::kNeedMoreData) {
                break;
            }

            if (readResult == Net::TcpPacketReadResult::kInvalidPacket) {
                markClientForDisconnect(disconnectedClients, connection.clientFd());
                return;
            }

            if (!handleRoomPacket(
                    connection,
                    framedPacket,
                    disconnectedClients,
                    outRoomListChanged)) {
                return;
            }
        }

        if (!drainUntilWouldBlock) {
            return;
        }
    }
}

void Server::processRudpSocket(Util::TimePoint now) {
    const Net::RudpSocketDrainSummary summary =
        Net::drainRudpSocket(udpSocket_, rudpPeerRegistry_, now, kRudpMaxPacketsPerTick);
    accumulateRudpDrainStats(summary);
    processRudpReliableEventAcks(summary.ackOnlyDeliveries);
    processRudpReliableEventAcks(summary.deliveries);
    processRudpDeliveries(summary.deliveries, now);
}

void Server::processRudpDeliveries(
    const std::vector<Net::RudpPacketDelivery>& deliveries,
    Util::TimePoint now) {
    for (const Net::RudpPacketDelivery& delivery : deliveries) {
        if (delivery.header.packetType != static_cast<uint16_t>(Net::RudpPacketType::kHello)) {
            rudpBindingIgnoredNonHello_.fetch_add(1, std::memory_order_relaxed);
            processRudpAdapterGate(delivery, now);
            continue;
        }
        processRudpHelloDelivery(delivery, now);
    }
}

void Server::processRudpAdapterGate(
    const Net::RudpPacketDelivery& delivery,
    Util::TimePoint now) {
    if (delivery.header.packetType !=
        static_cast<uint16_t>(Net::RudpPacketType::kInputCommand)) {
        rudpBindingUnsupportedPacketIgnored_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const std::optional<uint64_t> sessionId =
        rudpSessionBinder_.findSessionId(delivery.endpoint);
    if (!sessionId.has_value()) {
        rudpBindingUnboundInputRejected_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    rudpBindingInputCandidates_.fetch_add(1, std::memory_order_relaxed);
    Net::RudpInputCommandPayload input;
    if (!Net::parseRudpInputCommandPayload(
            delivery.payload.data(),
            delivery.payload.size(),
            input)) {
        rudpBindingInputDecodeFailed_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    rudpBindingInputDecoded_.fetch_add(1, std::memory_order_relaxed);
    const Net::RudpInputCommandSequenceResult sequenceResult =
        rudpInputCommandSequenceTracker_.record(*sessionId, input.cmdSeq);
    switch (sequenceResult) {
    case Net::RudpInputCommandSequenceResult::kAcceptedFirst:
    case Net::RudpInputCommandSequenceResult::kAcceptedNewer:
        rudpBindingInputSequenceAccepted_.fetch_add(1, std::memory_order_relaxed);
        break;
    case Net::RudpInputCommandSequenceResult::kDuplicate:
        rudpBindingInputSequenceDuplicateRejected_.fetch_add(
            1,
            std::memory_order_relaxed);
        return;
    case Net::RudpInputCommandSequenceResult::kStale:
        rudpBindingInputSequenceStaleRejected_.fetch_add(
            1,
            std::memory_order_relaxed);
        return;
    case Net::RudpInputCommandSequenceResult::kAmbiguous:
        rudpBindingInputSequenceAmbiguousRejected_.fetch_add(
            1,
            std::memory_order_relaxed);
        return;
    case Net::RudpInputCommandSequenceResult::kInvalidSession:
        rudpBindingInputSequenceInvalidSessionRejected_.fetch_add(
            1,
            std::memory_order_relaxed);
        return;
    }

    const std::optional<uint32_t> roomId =
        roomManager_.findRoomIdForSession(*sessionId);
    if (!roomId.has_value()) {
        rudpBindingInputNoRoomRejected_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (input.op == Net::RudpInputCommandOp::kMove) {
        const Net::RudpMoveInputGuardResult moveGuardResult =
            rudpMoveInputGuard_.record(*sessionId, input.move, now);
        switch (moveGuardResult) {
        case Net::RudpMoveInputGuardResult::kAccepted:
            rudpBindingMoveAccepted_.fetch_add(1, std::memory_order_relaxed);
            dispatchRudpMoveInput(*sessionId, input.move, now);
            return;
        case Net::RudpMoveInputGuardResult::kInvalidReservedFlags:
            rudpBindingMoveInvalidReservedFlagsRejected_.fetch_add(
                1,
                std::memory_order_relaxed);
            return;
        case Net::RudpMoveInputGuardResult::kRateLimited:
            rudpBindingMoveRateLimitedRejected_.fetch_add(
                1,
                std::memory_order_relaxed);
            return;
        case Net::RudpMoveInputGuardResult::kInvalidSession:
            rudpBindingInputSequenceInvalidSessionRejected_.fetch_add(
                1,
                std::memory_order_relaxed);
            return;
        }
    }

    const RudpInputCommandRoomEventTranslateResult translateResult =
        translateRudpInputCommandToRoomEvent(*sessionId, *roomId, input);
    if (translateResult.status !=
            RudpInputCommandRoomEventTranslateStatus::kTranslated ||
        !translateResult.event.has_value()) {
        return;
    }

    dispatchRudpRoomEvent(*translateResult.event, now);
}

void Server::dispatchRudpMoveInput(
    uint64_t sessionId,
    const Net::RudpInputCommandMoveArgs& move,
    Util::TimePoint now) {
    auto stateIt = rudpMoveDispatchStateBySession_.find(sessionId);
    if (stateIt == rudpMoveDispatchStateBySession_.end()) {
        const Game::MovementCommandResult result =
            roomManager_.applyMovement(sessionId, move.dirX, move.dirY, 0);
        if (!result.ok) {
            rudpBindingMoveApplyRejected_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        rudpMoveDispatchStateBySession_.emplace(
            sessionId,
            RudpMoveDispatchState{now, move.dirX, move.dirY});
        rudpBindingMoveDispatched_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (!integrateRudpMoveInput(sessionId, now)) {
        rudpBindingMoveApplyRejected_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    stateIt = rudpMoveDispatchStateBySession_.find(sessionId);
    if (stateIt == rudpMoveDispatchStateBySession_.end()) {
        rudpBindingMoveApplyRejected_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    RudpMoveDispatchState& state = stateIt->second;
    state.currentDirX = move.dirX;
    state.currentDirY = move.dirY;
    rudpBindingMoveDispatched_.fetch_add(1, std::memory_order_relaxed);
}

bool Server::integrateRudpMoveInput(uint64_t sessionId, Util::TimePoint now) {
    auto stateIt = rudpMoveDispatchStateBySession_.find(sessionId);
    if (stateIt == rudpMoveDispatchStateBySession_.end()) {
        return false;
    }

    if (!roomManager_.findRoomIdForSession(sessionId).has_value()) {
        rudpMoveDispatchStateBySession_.erase(stateIt);
        return false;
    }

    RudpMoveDispatchState& state = stateIt->second;
    const uint32_t elapsedMs = elapsedMillisClamped(state.lastIntegratedAt, now);
    const Game::MovementCommandResult result = roomManager_.applyMovement(
        sessionId,
        state.currentDirX,
        state.currentDirY,
        elapsedMs);
    if (!result.ok) {
        rudpMoveDispatchStateBySession_.erase(sessionId);
        return false;
    }

    state.lastIntegratedAt = now;
    return true;
}

void Server::integrateRudpMoveInputsForRoom(uint32_t roomId, Util::TimePoint now) {
    for (auto stateIt = rudpMoveDispatchStateBySession_.begin();
         stateIt != rudpMoveDispatchStateBySession_.end();) {
        const uint64_t sessionId = stateIt->first;
        ++stateIt;

        const std::optional<uint32_t> sessionRoomId =
            roomManager_.findRoomIdForSession(sessionId);
        if (!sessionRoomId.has_value()) {
            rudpMoveDispatchStateBySession_.erase(sessionId);
            continue;
        }
        if (*sessionRoomId == roomId) {
            integrateRudpMoveInput(sessionId, now);
        }
    }
}

void Server::pruneRudpMoveInputsWithoutRoom() {
    for (auto stateIt = rudpMoveDispatchStateBySession_.begin();
         stateIt != rudpMoveDispatchStateBySession_.end();) {
        if (!roomManager_.findRoomIdForSession(stateIt->first).has_value()) {
            stateIt = rudpMoveDispatchStateBySession_.erase(stateIt);
        } else {
            ++stateIt;
        }
    }
}

RudpServerReliableEventTrackResult Server::trackRudpReliableEventForSession(
    uint64_t sessionId,
    const Net::RudpReliableEventDescriptor& descriptor,
    uint32_t sequence,
    const std::vector<uint8_t>& packetBytes,
    Util::TimePoint now) {
    if (sessionId == 0) {
        rudpReliableEventInvalidSession_.fetch_add(1, std::memory_order_relaxed);
        return RudpServerReliableEventTrackResult::kInvalidSession;
    }

    auto queueIt = rudpReliableEventQueues_.find(sessionId);
    if (queueIt == rudpReliableEventQueues_.end()) {
        Net::RudpReliableEventSendQueue queue;
        const Net::RudpReliableEventTrackResult result =
            queue.track(descriptor, sequence, packetBytes, now);
        switch (result) {
        case Net::RudpReliableEventTrackResult::kTracked:
            rudpReliableEventQueues_.emplace(sessionId, std::move(queue));
            rudpReliableEventTracked_.fetch_add(1, std::memory_order_relaxed);
            rudpReliableEventPendingCountSnapshot_.store(
                calculateRudpReliableEventPendingCount(),
                std::memory_order_relaxed);
            return RudpServerReliableEventTrackResult::kTracked;
        case Net::RudpReliableEventTrackResult::kDuplicateSequence:
            rudpReliableEventDuplicateSequence_.fetch_add(
                1,
                std::memory_order_relaxed);
            return RudpServerReliableEventTrackResult::kDuplicateSequence;
        case Net::RudpReliableEventTrackResult::kDuplicateLogicalEvent:
            rudpReliableEventDuplicateLogicalEvent_.fetch_add(
                1,
                std::memory_order_relaxed);
            return RudpServerReliableEventTrackResult::kDuplicateLogicalEvent;
        case Net::RudpReliableEventTrackResult::kInvalidDescriptor:
            rudpReliableEventInvalidDescriptor_.fetch_add(
                1,
                std::memory_order_relaxed);
            return RudpServerReliableEventTrackResult::kInvalidDescriptor;
        case Net::RudpReliableEventTrackResult::kInvalidPacketBytes:
            rudpReliableEventInvalidPacketBytes_.fetch_add(
                1,
                std::memory_order_relaxed);
            return RudpServerReliableEventTrackResult::kInvalidPacketBytes;
        }
    }

    const Net::RudpReliableEventTrackResult result =
        queueIt->second.track(descriptor, sequence, packetBytes, now);
    switch (result) {
    case Net::RudpReliableEventTrackResult::kTracked:
        rudpReliableEventTracked_.fetch_add(1, std::memory_order_relaxed);
        rudpReliableEventPendingCountSnapshot_.store(
            calculateRudpReliableEventPendingCount(),
            std::memory_order_relaxed);
        return RudpServerReliableEventTrackResult::kTracked;
    case Net::RudpReliableEventTrackResult::kDuplicateSequence:
        rudpReliableEventDuplicateSequence_.fetch_add(
            1,
            std::memory_order_relaxed);
        return RudpServerReliableEventTrackResult::kDuplicateSequence;
    case Net::RudpReliableEventTrackResult::kDuplicateLogicalEvent:
        rudpReliableEventDuplicateLogicalEvent_.fetch_add(
            1,
            std::memory_order_relaxed);
        return RudpServerReliableEventTrackResult::kDuplicateLogicalEvent;
    case Net::RudpReliableEventTrackResult::kInvalidDescriptor:
        rudpReliableEventInvalidDescriptor_.fetch_add(
            1,
            std::memory_order_relaxed);
        return RudpServerReliableEventTrackResult::kInvalidDescriptor;
    case Net::RudpReliableEventTrackResult::kInvalidPacketBytes:
        rudpReliableEventInvalidPacketBytes_.fetch_add(
            1,
            std::memory_order_relaxed);
        return RudpServerReliableEventTrackResult::kInvalidPacketBytes;
    }

    rudpReliableEventInvalidDescriptor_.fetch_add(1, std::memory_order_relaxed);
    return RudpServerReliableEventTrackResult::kInvalidDescriptor;
}

RudpServerReliableEventTrackResult Server::trackAndSendRudpReliableEventForSession(
    uint64_t sessionId,
    const Net::RudpReliableEventDescriptor& descriptor,
    uint32_t sequence,
    const std::vector<uint8_t>& packetBytes,
    Util::TimePoint now) {
    if (sessionId == 0) {
        rudpReliableEventInvalidSession_.fetch_add(1, std::memory_order_relaxed);
        return RudpServerReliableEventTrackResult::kInvalidSession;
    }

    const std::optional<Net::UdpEndpoint> endpoint =
        findBoundRudpEndpointForSession(sessionId);
    if (!endpoint.has_value()) {
        return RudpServerReliableEventTrackResult::kNoBoundEndpoint;
    }

    const RudpServerReliableEventTrackResult trackResult =
        trackRudpReliableEventForSession(
            sessionId,
            descriptor,
            sequence,
            packetBytes,
            now);
    if (trackResult != RudpServerReliableEventTrackResult::kTracked) {
        return trackResult;
    }

    udpSocket_.sendTo(packetBytes.data(), packetBytes.size(), *endpoint);
    return RudpServerReliableEventTrackResult::kTracked;
}

void Server::enqueueRudpBattleStartEvent(
    const Game::RoomCommandResult& result,
    Util::TimePoint now) {
    Net::RudpBattleStartPayload payload;
    if (!buildRudpBattleStartPayload(result, payload)) {
        return;
    }

    std::vector<uint8_t> payloadBytes;
    if (!Net::serializeRudpBattleStartPayload(payload, payloadBytes)) {
        return;
    }

    const Net::RudpReliableEventDescriptor descriptor{
        Net::RudpReliableEventKind::kBattleStart,
        battleStartLogicalKey(payload),
        static_cast<uint16_t>(Net::RudpPacketType::kBattleStart),
        static_cast<uint8_t>(Net::RudpChannelId::kEvent)};

    for (uint64_t sessionId : result.playerSessionIds) {
        if (!findBoundRudpEndpointForSession(sessionId).has_value()) {
            continue;
        }

        const uint32_t sequence = nextRudpOutboundSequenceForSession(sessionId);
        std::vector<uint8_t> packetBytes;
        if (!buildRudpReliableEventPacket(
                descriptor,
                sequence,
                payloadBytes,
                packetBytes)) {
            continue;
        }

        trackAndSendRudpReliableEventForSession(
            sessionId,
            descriptor,
            sequence,
            packetBytes,
            now);
    }
}

void Server::enqueueRudpMonsterDeathEvent(
    const Game::RoomCommandResult& result,
    Util::TimePoint now) {
    const Net::RudpMonsterDeathGameEventPayload payload{
        result.room.roomId,
        result.monster.monsterId};

    std::vector<uint8_t> payloadBytes;
    if (!Net::serializeRudpMonsterDeathGameEventPayload(payload, payloadBytes)) {
        return;
    }

    const Net::RudpReliableEventDescriptor descriptor{
        Net::RudpReliableEventKind::kMonsterDeath,
        monsterDeathLogicalKey(payload),
        static_cast<uint16_t>(Net::RudpPacketType::kGameEvent),
        static_cast<uint8_t>(Net::RudpChannelId::kEvent)};

    for (uint64_t sessionId : result.playerSessionIds) {
        if (!findBoundRudpEndpointForSession(sessionId).has_value()) {
            continue;
        }

        const uint32_t sequence = nextRudpOutboundSequenceForSession(sessionId);
        std::vector<uint8_t> packetBytes;
        if (!buildRudpReliableEventPacket(
                descriptor,
                sequence,
                payloadBytes,
                packetBytes)) {
            continue;
        }

        trackAndSendRudpReliableEventForSession(
            sessionId,
            descriptor,
            sequence,
            packetBytes,
            now);
    }
}

void Server::enqueueRudpLootResolvedEvent(
    const Game::RoomCommandResult& result,
    Util::TimePoint now) {
    const Net::RudpLootResolvedGameEventPayload payload{
        result.room.roomId,
        result.drop.dropId,
        result.winnerSessionId,
        result.drop.itemId,
        result.drop.quantity};

    std::vector<uint8_t> payloadBytes;
    if (!Net::serializeRudpLootResolvedGameEventPayload(payload, payloadBytes)) {
        return;
    }

    const Net::RudpReliableEventDescriptor descriptor{
        Net::RudpReliableEventKind::kLootResolved,
        lootResolvedLogicalKey(payload),
        static_cast<uint16_t>(Net::RudpPacketType::kGameEvent),
        static_cast<uint8_t>(Net::RudpChannelId::kEvent)};

    for (uint64_t sessionId : result.playerSessionIds) {
        if (!findBoundRudpEndpointForSession(sessionId).has_value()) {
            continue;
        }

        const uint32_t sequence = nextRudpOutboundSequenceForSession(sessionId);
        std::vector<uint8_t> packetBytes;
        if (!buildRudpReliableEventPacket(
                descriptor,
                sequence,
                payloadBytes,
                packetBytes)) {
            continue;
        }

        trackAndSendRudpReliableEventForSession(
            sessionId,
            descriptor,
            sequence,
            packetBytes,
            now);
    }
}

bool Server::observeRudpMetaResponseForSession(
    uint64_t sessionId,
    const Net::RudpMetaResponsePayload& payload,
    Util::TimePoint now) {
    if (sessionId == 0) {
        return false;
    }

    std::vector<uint8_t> payloadBytes;
    if (!Net::serializeRudpMetaResponsePayload(payload, payloadBytes)) {
        rudpMetaResponseInvalidPayload_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const Net::RudpMetaResponseIdempotencyResult idempotencyResult =
        rudpMetaResponseIdempotencyTracker_.record(
            payload.settlementId,
            payload.status);
    switch (idempotencyResult) {
    case Net::RudpMetaResponseIdempotencyResult::kCompletedFirst:
        rudpMetaResponseCompletedFirst_.fetch_add(
            1,
            std::memory_order_relaxed);
        break;
    case Net::RudpMetaResponseIdempotencyResult::kRetryObserved:
        rudpMetaResponseRetryObserved_.fetch_add(1, std::memory_order_relaxed);
        break;
    case Net::RudpMetaResponseIdempotencyResult::kCompletionDuplicate:
        rudpMetaResponseCompletionDuplicate_.fetch_add(
            1,
            std::memory_order_relaxed);
        return false;
    case Net::RudpMetaResponseIdempotencyResult::kRetryDuplicate:
        rudpMetaResponseRetryDuplicate_.fetch_add(1, std::memory_order_relaxed);
        return false;
    case Net::RudpMetaResponseIdempotencyResult::kRetryIgnoredAfterCompletion:
        rudpMetaResponseRetryIgnoredAfterCompletion_.fetch_add(
            1,
            std::memory_order_relaxed);
        return false;
    case Net::RudpMetaResponseIdempotencyResult::kInvalidSettlementId:
        rudpMetaResponseInvalidPayload_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const Net::RudpReliableEventDescriptor descriptor{
        Net::RudpReliableEventKind::kMetaResponse,
        payload.settlementId,
        static_cast<uint16_t>(Net::RudpPacketType::kMetaResponse),
        static_cast<uint8_t>(Net::RudpChannelId::kControl)};

    const uint32_t sequence = nextRudpOutboundSequenceForSession(sessionId);
    std::vector<uint8_t> packetBytes;
    if (!buildRudpReliableEventPacket(
            descriptor,
            sequence,
            payloadBytes,
            packetBytes)) {
        rudpMetaResponseInvalidPayload_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const RudpServerReliableEventTrackResult trackResult =
        trackRudpReliableEventForSession(
            sessionId,
            descriptor,
            sequence,
            packetBytes,
            now);
    if (trackResult != RudpServerReliableEventTrackResult::kTracked) {
        return false;
    }

    rudpMetaResponseEnqueued_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

uint32_t Server::nextRudpOutboundSequenceForSession(uint64_t sessionId) {
    uint32_t& nextSequence = rudpOutboundNextSequenceBySession_[sessionId];
    if (nextSequence == 0) {
        nextSequence = 1;
    }

    const uint32_t sequence = nextSequence;
    ++nextSequence;
    if (nextSequence == 0) {
        nextSequence = 1;
    }
    return sequence;
}

void Server::clearRudpReliableEventsForSession(uint64_t sessionId) {
    rudpReliableEventQueues_.erase(sessionId);
    rudpReliableEventPendingCountSnapshot_.store(
        calculateRudpReliableEventPendingCount(),
        std::memory_order_relaxed);
}

void Server::clearRudpOutboundSequenceForSession(uint64_t sessionId) {
    rudpOutboundNextSequenceBySession_.erase(sessionId);
}

std::optional<Net::UdpEndpoint> Server::findBoundRudpEndpointForSession(
    uint64_t sessionId) const {
    if (sessionId == 0) {
        return std::nullopt;
    }

    std::optional<Net::UdpEndpoint> endpoint;
    rudpPeerRegistry_.forEachPeer(
        [this, sessionId, &endpoint](
            const Net::UdpEndpoint& candidate,
            const Net::RudpPeer&) {
            if (endpoint.has_value()) {
                return;
            }

            const std::optional<uint64_t> boundSessionId =
                rudpSessionBinder_.findSessionId(candidate);
            if (boundSessionId.has_value() && *boundSessionId == sessionId) {
                endpoint = candidate;
            }
        });
    return endpoint;
}

void Server::processRudpReliableEventAcks(
    const std::vector<Net::RudpPacketDelivery>& deliveries) {
    for (const Net::RudpPacketDelivery& delivery : deliveries) {
        consumeRudpReliableEventAck(
            delivery.endpoint,
            delivery.header.ack,
            delivery.header.ackBits);
    }
}

void Server::consumeRudpReliableEventAck(
    const Net::UdpEndpoint& endpoint,
    uint32_t ack,
    uint32_t ackBits) {
    const std::optional<uint64_t> sessionId =
        rudpSessionBinder_.findSessionId(endpoint);
    if (!sessionId.has_value()) {
        return;
    }

    auto queueIt = rudpReliableEventQueues_.find(*sessionId);
    if (queueIt == rudpReliableEventQueues_.end()) {
        return;
    }

    if (queueIt->second.consumeAck(ack, ackBits) == 0) {
        return;
    }

    if (queueIt->second.pendingCount() == 0) {
        rudpReliableEventQueues_.erase(queueIt);
    }
    rudpReliableEventPendingCountSnapshot_.store(
        calculateRudpReliableEventPendingCount(),
        std::memory_order_relaxed);
}

void Server::processRudpHelloDelivery(
    const Net::RudpPacketDelivery& delivery,
    Util::TimePoint now) {
    rudpBindingHelloReceived_.fetch_add(1, std::memory_order_relaxed);

    Net::RudpHelloPayload hello;
    if (!Net::parseRudpHelloPayload(
            delivery.payload.data(),
            delivery.payload.size(),
            hello)) {
        rudpBindingInvalidPayload_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    auto session = sessionManager_.findBySessionId(hello.sessionId);
    if (!session) {
        rudpBindingUnknownSession_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const Net::RudpSessionBindResult bindResult =
        rudpSessionBinder_.bind(delivery.endpoint, hello.sessionId);
    switch (bindResult) {
    case Net::RudpSessionBindResult::kBoundNew:
        rudpBindingBound_.fetch_add(1, std::memory_order_relaxed);
        session->updateLastHeard(now);
        break;
    case Net::RudpSessionBindResult::kRefreshed:
        rudpBindingRefreshed_.fetch_add(1, std::memory_order_relaxed);
        session->updateLastHeard(now);
        break;
    case Net::RudpSessionBindResult::kEndpointConflict:
    case Net::RudpSessionBindResult::kSessionConflict:
        rudpBindingConflicts_.fetch_add(1, std::memory_order_relaxed);
        break;
    case Net::RudpSessionBindResult::kInvalidEndpoint:
        rudpBindingInvalidEndpoint_.fetch_add(1, std::memory_order_relaxed);
        break;
    }

    rudpBindingCountSnapshot_.store(
        rudpSessionBinder_.size(),
        std::memory_order_relaxed);
}

void Server::processRudpPeerLifecycle(Util::TimePoint now) {
    rudpPeerRegistry_.tick(now);
    rudpPeerCountSnapshot_.store(rudpPeerRegistry_.size(), std::memory_order_relaxed);
}

void Server::processRudpRetransmissions(Util::TimePoint now) {
    const Net::RudpRetransmissionFlushSummary summary =
        Net::flushRudpRetransmissions(udpSocket_, rudpPeerRegistry_, now);
    accumulateRudpRetransmissionStats(summary);
}

void Server::processRudpReliableEventRetransmissions(Util::TimePoint now) {
    for (auto queueIt = rudpReliableEventQueues_.begin();
         queueIt != rudpReliableEventQueues_.end();) {
        const uint64_t sessionId = queueIt->first;
        Net::RudpReliableEventSendQueue& queue = queueIt->second;

        const std::vector<uint32_t> expiredSequences =
            queue.expiredSequences(now);
        for (uint32_t sequence : expiredSequences) {
            if (queue.remove(sequence)) {
                rudpRetransmissionExpired_.fetch_add(1, std::memory_order_relaxed);
            }
        }

        const std::optional<Net::UdpEndpoint> endpoint =
            findBoundRudpEndpointForSession(sessionId);
        const std::vector<uint32_t> dueSequences =
            queue.dueForRetransmission(now);
        for (uint32_t sequence : dueSequences) {
            rudpRetransmissionDue_.fetch_add(1, std::memory_order_relaxed);
            const std::vector<uint8_t>* packetBytes = queue.packetBytes(sequence);
            if (!endpoint.has_value() ||
                packetBytes == nullptr ||
                packetBytes->empty()) {
                rudpRetransmissionSendErrors_.fetch_add(
                    1,
                    std::memory_order_relaxed);
                continue;
            }

            if (!udpSocket_.sendTo(
                    packetBytes->data(),
                    packetBytes->size(),
                    *endpoint)) {
                rudpRetransmissionSendErrors_.fetch_add(
                    1,
                    std::memory_order_relaxed);
                continue;
            }

            if (!queue.markRetransmitted(sequence, now)) {
                rudpRetransmissionSendErrors_.fetch_add(
                    1,
                    std::memory_order_relaxed);
                continue;
            }

            rudpRetransmissionResent_.fetch_add(1, std::memory_order_relaxed);
        }

        if (queue.pendingCount() == 0) {
            queueIt = rudpReliableEventQueues_.erase(queueIt);
        } else {
            ++queueIt;
        }
    }

    rudpReliableEventPendingCountSnapshot_.store(
        calculateRudpReliableEventPendingCount(),
        std::memory_order_relaxed);
}

void Server::processRudpMovementSnapshots(Util::TimePoint now) {
    const std::vector<Game::RoomSummary> roomSummaries = roomManager_.roomList();
    pruneRudpMoveInputsWithoutRoom();

    for (auto stateIt = rudpSnapshotStateByRoom_.begin();
         stateIt != rudpSnapshotStateByRoom_.end();) {
        const uint32_t roomId = stateIt->first;
        const bool roomStillExists = std::any_of(
            roomSummaries.begin(),
            roomSummaries.end(),
            [roomId](const Game::RoomSummary& summary) {
                return summary.roomId == roomId;
            });
        if (!roomStillExists) {
            stateIt = rudpSnapshotStateByRoom_.erase(stateIt);
        } else {
            ++stateIt;
        }
    }

    struct SnapshotRecipient {
        uint64_t sessionId{0};
        Net::UdpEndpoint endpoint{};
    };

    for (const Game::RoomSummary& summary : roomSummaries) {
        const Game::Room* room = roomManager_.findRoom(summary.roomId);
        if (room == nullptr || room->movementSnapshots().empty()) {
            continue;
        }

        const std::vector<Game::MovementSnapshot>& movements =
            room->movementSnapshots();
        std::vector<SnapshotRecipient> recipients;
        recipients.reserve(movements.size());
        for (const Game::MovementSnapshot& movement : movements) {
            const std::optional<Net::UdpEndpoint> endpoint =
                findBoundRudpEndpointForSession(movement.sessionId);
            if (endpoint.has_value()) {
                recipients.push_back(SnapshotRecipient{movement.sessionId, *endpoint});
            }
        }

        if (recipients.empty()) {
            rudpSnapshotSkippedNoBoundEndpoint_.fetch_add(
                1,
                std::memory_order_relaxed);
            continue;
        }

        RudpSnapshotRoomState& state = rudpSnapshotStateByRoom_[summary.roomId];
        if (state.hasSent && now - state.lastSentAt < kRudpSnapshotInterval) {
            continue;
        }

        integrateRudpMoveInputsForRoom(summary.roomId, now);
        room = roomManager_.findRoom(summary.roomId);
        if (room == nullptr || room->movementSnapshots().empty()) {
            continue;
        }
        const std::vector<Game::MovementSnapshot>& updatedMovements =
            room->movementSnapshots();

        Net::RudpStateSnapshotPayload snapshot;
        snapshot.roomId = room->roomId();
        snapshot.serverTick = state.serverTick + 1;
        snapshot.players.reserve(updatedMovements.size());
        for (const Game::MovementSnapshot& movement : updatedMovements) {
            snapshot.players.push_back(Net::RudpStateSnapshotPlayer{
                movement.sessionId,
                movement.position.x,
                movement.position.y});
        }

        std::vector<uint8_t> payloadBytes;
        if (!Net::serializeRudpStateSnapshotPayload(snapshot, payloadBytes)) {
            rudpSnapshotSerializeFailed_.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        state.serverTick = snapshot.serverTick;
        state.lastSentAt = now;
        state.hasSent = true;
        rudpSnapshotBuilt_.fetch_add(1, std::memory_order_relaxed);

        for (const SnapshotRecipient& recipient : recipients) {
            const uint32_t sequence =
                nextRudpOutboundSequenceForSession(recipient.sessionId);
            std::vector<uint8_t> packetBytes;
            if (!buildRudpUnreliableSnapshotPacket(
                    sequence,
                    payloadBytes,
                    packetBytes) ||
                !udpSocket_.sendTo(
                    packetBytes.data(),
                    packetBytes.size(),
                    recipient.endpoint)) {
                rudpSnapshotSendErrors_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            rudpSnapshotSent_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void Server::accumulateRudpDrainStats(const Net::RudpSocketDrainSummary& summary) {
    rudpDrainAttempted_.fetch_add(summary.attempted, std::memory_order_relaxed);
    rudpDrainDelivered_.fetch_add(summary.delivered, std::memory_order_relaxed);
    rudpDrainMalformed_.fetch_add(summary.malformed, std::memory_order_relaxed);
    rudpDrainInvalidEndpoint_.fetch_add(
        summary.invalidEndpoint,
        std::memory_order_relaxed);
    rudpDrainAckOnly_.fetch_add(summary.ackOnly, std::memory_order_relaxed);
    rudpDrainDuplicate_.fetch_add(summary.duplicate, std::memory_order_relaxed);
    rudpDrainTooOld_.fetch_add(summary.tooOld, std::memory_order_relaxed);
    rudpDrainSocketErrors_.fetch_add(summary.socketErrors, std::memory_order_relaxed);
}

void Server::accumulateRudpRetransmissionStats(
    const Net::RudpRetransmissionFlushSummary& summary) {
    rudpRetransmissionExpired_.fetch_add(summary.expired, std::memory_order_relaxed);
    rudpRetransmissionDue_.fetch_add(summary.due, std::memory_order_relaxed);
    rudpRetransmissionResent_.fetch_add(summary.resent, std::memory_order_relaxed);
    rudpRetransmissionSendErrors_.fetch_add(
        summary.sendErrors,
        std::memory_order_relaxed);
    rudpRetransmissionDroppedPeers_.fetch_add(
        summary.droppedPeers,
        std::memory_order_relaxed);
    if (summary.droppedPeers > 0) {
        rudpPeerCountSnapshot_.store(
            rudpPeerRegistry_.size(),
            std::memory_order_relaxed);
    }
}

size_t Server::calculateRudpReliableEventPendingCount() const {
    size_t total = 0;
    for (const auto& entry : rudpReliableEventQueues_) {
        total += entry.second.pendingCount();
    }
    return total;
}

std::vector<uint64_t> Server::collectActiveSessionIds() const {
    std::vector<uint64_t> sessionIds;
    sessionIds.reserve(connections_.size());
    for (const auto& entry : connections_) {
        sessionIds.push_back(entry.second->sessionId());
    }

    std::sort(sessionIds.begin(), sessionIds.end());
    return sessionIds;
}

std::vector<Net::TcpRoomEntry> Server::collectRoomEntries() const {
    const std::vector<Game::RoomSummary> rooms = roomManager_.roomList();
    std::vector<Net::TcpRoomEntry> entries;
    entries.reserve(rooms.size());
    for (const Game::RoomSummary& room : rooms) {
        entries.push_back(Net::TcpRoomEntry{room.roomId, room.playerCount, room.maxPlayers});
    }
    return entries;
}

bool Server::handleRoomPacket(
    ClientConnection& connection,   // 이 패킷을 보낸 클라이언트 연결 객체
    const std::vector<uint8_t>& packet,     // 이미 프레이밍이 끝난 패킷 1개 바이트 배열
    std::vector<int>& disconnectedClients,  // 끊어야 할 소켓 fd 목록
    bool& outRoomListChanged) {
    auto session = sessionManager_.find(connection.remoteKey());
    if (!session) {
        markClientForDisconnect(disconnectedClients, connection.clientFd());
        return false;
    }

    Net::TcpPacketHeader header;
    if (!Net::peekTcpPacketHeader(packet.data(), packet.size(), header)) {
        markClientForDisconnect(disconnectedClients, connection.clientFd());
        return false;
    }

    switch (header.type) {
    case Net::TcpPacketType::kCreateRoomRequest: {
        if (!Net::parseCreateRoomRequestPacket(packet.data(), packet.size(), header)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return false;
        }
        // 서버가 sessionId 기준으로 룸을 생성. 서버 권한 구조에 맞음.
        const Game::RoomCommandResult result = roomManager_.createRoom(session->sessionId());
        if (!result.ok) {
            std::array<uint8_t, Net::kErrorPacketSize> errorPacket{};   // 고정 길이 에러 패킷 버퍼
            Net::serializeErrorPacket(
                Net::TcpPacketType::kCreateRoomRequest,
                toTcpErrorCode(result.error),
                errorPacket);
            return sendPacketToClient(
                connection.clientFd(),
                errorPacket.data(),
                errorPacket.size(),
                disconnectedClients);
        }

        std::array<uint8_t, Net::kRoomStatusPacketSize> responsePacket{};
        Net::serializeCreateRoomResponsePacket(
            result.room.roomId,
            result.room.playerCount,
            responsePacket);
        if (!sendPacketToClient(
                connection.clientFd(),
                responsePacket.data(),
                responsePacket.size(),
                disconnectedClients)) {
            return false;
        }

        roomEventDispatcher_.registerRoom(result.room.roomId);
        outRoomListChanged = true;
        return true;
    }

    case Net::TcpPacketType::kJoinRoomRequest: {
        uint32_t roomId = 0;
        if (!Net::parseJoinRoomRequestPacket(packet.data(), packet.size(), header, roomId)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return false;
        }

        const Game::RoomCommandResult result = roomManager_.joinRoom(session->sessionId(), roomId);
        if (!result.ok) {
            std::array<uint8_t, Net::kErrorPacketSize> errorPacket{};
            Net::serializeErrorPacket(
                Net::TcpPacketType::kJoinRoomRequest,
                toTcpErrorCode(result.error),
                errorPacket);
            return sendPacketToClient(
                connection.clientFd(),
                errorPacket.data(),
                errorPacket.size(),
                disconnectedClients);
        }

        std::array<uint8_t, Net::kRoomStatusPacketSize> responsePacket{};
        Net::serializeJoinRoomResponsePacket(
            result.room.roomId,
            result.room.playerCount,
            responsePacket);
        if (!sendPacketToClient(
                connection.clientFd(),
                responsePacket.data(),
                responsePacket.size(),
                disconnectedClients)) {
            return false;
        }

        outRoomListChanged = true;
        return true;
    }

    case Net::TcpPacketType::kLeaveRoomRequest: {
        if (!Net::parseLeaveRoomRequestPacket(packet.data(), packet.size(), header)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return false;
        }

        const Game::RoomCommandResult result = roomManager_.leaveRoom(session->sessionId());
        if (!result.ok) {
            std::array<uint8_t, Net::kErrorPacketSize> errorPacket{};
            Net::serializeErrorPacket(
                Net::TcpPacketType::kLeaveRoomRequest,
                toTcpErrorCode(result.error),
                errorPacket);
            return sendPacketToClient(
                connection.clientFd(),
                errorPacket.data(),
                errorPacket.size(),
                disconnectedClients);
        }

        rudpMoveDispatchStateBySession_.erase(session->sessionId());

        std::array<uint8_t, Net::kRoomIdPacketSize> responsePacket{};
        Net::serializeLeaveRoomResponsePacket(result.room.roomId, responsePacket);
        if (!sendPacketToClient(
                connection.clientFd(),
                responsePacket.data(),
                responsePacket.size(),
                disconnectedClients)) {
            return false;
        }

        outRoomListChanged = true;
        return true;
    }

    case Net::TcpPacketType::kReadyRoomRequest: {
        if (!Net::parseReadyRoomRequestPacket(packet.data(), packet.size(), header)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return false;
        }

        const auto roomId = roomManager_.findRoomIdForSession(session->sessionId());
        if (!roomId.has_value()) {
            return sendTcpError(
                connection.clientFd(),
                Net::TcpPacketType::kReadyRoomRequest,
                Net::TcpErrorCode::kNotInRoom,
                disconnectedClients);
        }

        return dispatchTcpRoomEvent(
            connection,
            Net::TcpPacketType::kReadyRoomRequest,
            Game::makeReadyRoomEvent(session->sessionId(), *roomId),
            disconnectedClients);
    }

    case Net::TcpPacketType::kMonsterDeathRequest: {
        uint32_t monsterId = 0;
        if (!Net::parseMonsterDeathRequestPacket(packet.data(), packet.size(), header, monsterId)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return false;
        }

        const auto roomId = roomManager_.findRoomIdForSession(session->sessionId());
        if (!roomId.has_value()) {
            return sendTcpError(
                connection.clientFd(),
                Net::TcpPacketType::kMonsterDeathRequest,
                Net::TcpErrorCode::kNotInRoom,
                disconnectedClients);
        }

        if (monsterId == 0) {
            return sendTcpError(
                connection.clientFd(),
                Net::TcpPacketType::kMonsterDeathRequest,
                Net::TcpErrorCode::kNotFound,
                disconnectedClients);
        }

        return dispatchTcpRoomEvent(
            connection,
            Net::TcpPacketType::kMonsterDeathRequest,
            Game::makeMonsterDeathRoomEvent(session->sessionId(), *roomId, monsterId),
            disconnectedClients);
    }

    case Net::TcpPacketType::kClickLootRequest: {
        uint32_t dropId = 0;
        if (!Net::parseClickLootRequestPacket(packet.data(), packet.size(), header, dropId)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return false;
        }

        const auto roomId = roomManager_.findRoomIdForSession(session->sessionId());
        if (!roomId.has_value()) {
            return sendTcpError(
                connection.clientFd(),
                Net::TcpPacketType::kClickLootRequest,
                Net::TcpErrorCode::kNotInRoom,
                disconnectedClients);
        }

        if (dropId == 0) {
            return sendTcpError(
                connection.clientFd(),
                Net::TcpPacketType::kClickLootRequest,
                Net::TcpErrorCode::kNotFound,
                disconnectedClients);
        }

        return dispatchTcpRoomEvent(
            connection,
            Net::TcpPacketType::kClickLootRequest,
            Game::makeClickLootRoomEvent(session->sessionId(), *roomId, dropId),
            disconnectedClients);
    }

    case Net::TcpPacketType::kSmokeCreateCenterDropRequest: {
        if (!Net::parseSmokeCreateCenterDropRequestPacket(packet.data(), packet.size(), header)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return false;
        }

        const Game::RoomCommandResult result =
            roomManager_.createCenterDropForSmoke(session->sessionId());
        if (!result.ok) {
            return sendTcpError(
                connection.clientFd(),
                Net::TcpPacketType::kSmokeCreateCenterDropRequest,
                toTcpErrorCode(result.error),
                disconnectedClients);
        }

        return broadcastDropListSnapshot(result, disconnectedClients);
    }

    case Net::TcpPacketType::kSmokePlacePlayersAroundCenterDropRequest: {
        if (!Net::parseSmokePlacePlayersAroundCenterDropRequestPacket(
                packet.data(),
                packet.size(),
                header)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return false;
        }

        const Game::SmokePlayerPlacementResult result =
            roomManager_.placePlayersAroundCenterDropForSmoke(session->sessionId());
        if (!result.ok) {
            return sendTcpError(
                connection.clientFd(),
                Net::TcpPacketType::kSmokePlacePlayersAroundCenterDropRequest,
                toTcpErrorCode(result.error),
                disconnectedClients);
        }

        return true;
    }

    case Net::TcpPacketType::kFinishSessionRequest: {
        if (!Net::parseFinishSessionRequestPacket(packet.data(), packet.size(), header)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return false;
        }

        const Game::SettlementCommandResult result =
            roomManager_.buildSettlementResult(session->sessionId(), currentUnixTimeMs());
        if (!result.ok) {
            std::array<uint8_t, Net::kErrorPacketSize> errorPacket{};
            Net::serializeErrorPacket(
                Net::TcpPacketType::kFinishSessionRequest,
                toTcpErrorCode(result.error),
                errorPacket);
            return sendPacketToClient(
                connection.clientFd(),
                errorPacket.data(),
                errorPacket.size(),
                disconnectedClients);
        }

        std::vector<uint8_t> responsePacket;
        const Net::TcpSettlementResult settlement = toTcpSettlementResult(result.settlement);
        if (!Net::serializeSettlementResultPacket(settlement, responsePacket)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return false;
        }

        return sendPacketToClient(
            connection.clientFd(),
            responsePacket.data(),
            responsePacket.size(),
            disconnectedClients);
    }

    default:
        markClientForDisconnect(disconnectedClients, connection.clientFd());
        return false;
    }
}

bool Server::dispatchTcpRoomEvent(
    ClientConnection& connection,
    Net::TcpPacketType failedType,
    const Game::RoomEvent& event,
    std::vector<int>& disconnectedClients) {
    const Game::RoomEventDispatcherEnqueueResult enqueueResult =
        roomEventDispatcher_.enqueue(event);
    if (enqueueResult.status != Game::RoomEventDispatcherEnqueueStatus::kEnqueued) {
        return sendTcpError(
            connection.clientFd(),
            failedType,
            toTcpErrorCode(enqueueResult.status),
            disconnectedClients);
    }

    return drainInlineRoomEvents(disconnectedClients);
}

bool Server::dispatchRudpRoomEvent(
    const Game::RoomEvent& event,
    Util::TimePoint now) {
    const Game::RoomEventDispatcherEnqueueResult enqueueResult =
        roomEventDispatcher_.enqueue(event);
    if (enqueueResult.status != Game::RoomEventDispatcherEnqueueStatus::kEnqueued) {
        return false;
    }

    return drainInlineRudpRoomEvents(now);
}

bool Server::drainInlineRoomEvents(std::vector<int>& disconnectedClients) {
    bool allSucceeded = true;
    uint32_t roomId = 0;
    while (roomEventDispatcher_.tryPopActiveRoom(roomId)) {
        Game::RoomEvent event;
        while (roomEventDispatcher_.tryDequeueRoomEvent(roomId, event)) {
            if (!applyInlineRoomEvent(roomId, event, disconnectedClients)) {
                allSucceeded = false;
            }
        }
        roomEventDispatcher_.completeRoomProcessing(roomId);
    }

    return allSucceeded;
}

bool Server::drainInlineRudpRoomEvents(Util::TimePoint now) {
    bool allSucceeded = true;
    uint32_t roomId = 0;
    while (roomEventDispatcher_.tryPopActiveRoom(roomId)) {
        Game::RoomEvent event;
        while (roomEventDispatcher_.tryDequeueRoomEvent(roomId, event)) {
            if (!applyInlineRudpRoomEvent(roomId, event, now)) {
                allSucceeded = false;
            }
        }
        roomEventDispatcher_.completeRoomProcessing(roomId);
    }

    return allSucceeded;
}

bool Server::applyInlineRoomEvent(
    uint32_t roomId,
    const Game::RoomEvent& event,
    std::vector<int>& disconnectedClients) {
    Game::RoomActor actor(roomId);
    const Game::RoomEventApplyResult result = actor.apply(roomManager_, event);
    roomEventMetrics_.recordProcessed();

    if (result.status == Game::RoomEventApplyStatus::kInvalidEvent ||
        result.status == Game::RoomEventApplyStatus::kRoomMismatch) {
        const int clientFd = findClientFdForSession(event.sessionId);
        if (clientFd < 0) {
            return true;
        }

        return sendTcpError(
            clientFd,
            requestTypeFromRoomEventType(event.type),
            Net::TcpErrorCode::kNotInRoom,
            disconnectedClients);
    }

    outboundSendQueue_.enqueueFromRoomEventApplyResult(event, result);
    if (result.status == Game::RoomEventApplyStatus::kApplied &&
        result.commandResult.battleJustStarted) {
        const Game::RoomCommandResult spawnResult =
            roomManager_.spawnMonster(result.commandResult.room.roomId);
        if (spawnResult.ok && spawnResult.monsterJustSpawned) {
            outboundSendQueue_.enqueueRoomCommandBroadcasts(spawnResult);
        }
    }

    return flushOutboundQueue(disconnectedClients);
}

bool Server::applyInlineRudpRoomEvent(
    uint32_t roomId,
    const Game::RoomEvent& event,
    Util::TimePoint now) {
    Game::RoomActor actor(roomId);
    const Game::RoomEventApplyResult result = actor.apply(roomManager_, event);
    roomEventMetrics_.recordProcessed();

    if (result.status == Game::RoomEventApplyStatus::kInvalidEvent ||
        result.status == Game::RoomEventApplyStatus::kRoomMismatch) {
        return true;
    }

    outboundSendQueue_.enqueueFromRoomEventApplyResult(event, result);
    if (result.status == Game::RoomEventApplyStatus::kApplied &&
        result.commandResult.battleJustStarted) {
        const Game::RoomCommandResult spawnResult =
            roomManager_.spawnMonster(result.commandResult.room.roomId);
        if (spawnResult.ok && spawnResult.monsterJustSpawned) {
            outboundSendQueue_.enqueueRoomCommandBroadcasts(spawnResult);
        }
    }

    return flushRudpOutboundQueue(now);
}

bool Server::flushOutboundQueue(std::vector<int>& disconnectedClients) {
    bool allSucceeded = true;
    Game::OutboundEnvelope envelope;
    while (outboundSendQueue_.tryPop(envelope)) {
        if (!flushOutboundEnvelope(envelope, disconnectedClients)) {
            allSucceeded = false;
        }
    }

    return allSucceeded;
}

bool Server::flushRudpOutboundQueue(Util::TimePoint now) {
    bool allSucceeded = true;
    Game::OutboundEnvelope envelope;
    while (outboundSendQueue_.tryPop(envelope)) {
        if (!flushRudpOutboundEnvelope(envelope, now)) {
            allSucceeded = false;
        }
    }

    return allSucceeded;
}

bool Server::flushOutboundEnvelope(
    const Game::OutboundEnvelope& envelope,
    std::vector<int>& disconnectedClients) {
    const Game::RoomCommandResult result = roomCommandResultFromOutboundEnvelope(envelope);
    switch (envelope.message) {
    case Game::OutboundMessageType::kError: {
        const int clientFd = findClientFdForSession(envelope.targetSessionId);
        if (clientFd < 0) {
            return true;
        }
        return sendTcpError(
            clientFd,
            requestTypeFromRoomEventType(envelope.sourceEventType),
            toTcpErrorCode(envelope.error),
            disconnectedClients);
    }
    case Game::OutboundMessageType::kReadyRoomResponse: {
        const int clientFd = findClientFdForSession(envelope.targetSessionId);
        if (clientFd < 0) {
            return true;
        }
        std::array<uint8_t, Net::kReadyRoomStatusPacketSize> packet{};
        if (!Net::serializeReadyRoomResponsePacket(
                envelope.room.roomId,
                envelope.room.readyPlayerCount,
                envelope.room.playerCount,
                packet)) {
            return false;
        }
        return sendPacketToClient(clientFd, packet.data(), packet.size(), disconnectedClients);
    }
    case Game::OutboundMessageType::kBattleStart:
        return broadcastBattleStart(result, disconnectedClients);
    case Game::OutboundMessageType::kMonsterSpawn:
        return broadcastMonsterSpawn(result, disconnectedClients);
    case Game::OutboundMessageType::kMonsterDeath:
        return broadcastMonsterDeath(result, disconnectedClients);
    case Game::OutboundMessageType::kDropListSnapshot:
        return broadcastDropListSnapshot(result, disconnectedClients);
    case Game::OutboundMessageType::kLootRejected: {
        const int clientFd = findClientFdForSession(envelope.targetSessionId);
        if (clientFd < 0) {
            return true;
        }
        return sendLootRejected(clientFd, result, disconnectedClients);
    }
    case Game::OutboundMessageType::kLootResolved:
        return broadcastLootResolved(result, disconnectedClients);
    case Game::OutboundMessageType::kInventorySnapshot: {
        const int clientFd = findClientFdForSession(envelope.targetSessionId);
        if (clientFd < 0) {
            return true;
        }
        return sendInventorySnapshot(clientFd, envelope.inventory, disconnectedClients);
    }
    }

    return false;
}

bool Server::flushRudpOutboundEnvelope(
    const Game::OutboundEnvelope& envelope,
    Util::TimePoint now) {
    const Game::RoomCommandResult result = roomCommandResultFromOutboundEnvelope(envelope);
    switch (envelope.message) {
    case Game::OutboundMessageType::kBattleStart:
        enqueueRudpBattleStartEvent(result, now);
        return true;
    case Game::OutboundMessageType::kMonsterDeath:
        enqueueRudpMonsterDeathEvent(result, now);
        return true;
    case Game::OutboundMessageType::kLootResolved:
        enqueueRudpLootResolvedEvent(result, now);
        return true;
    case Game::OutboundMessageType::kError:
    case Game::OutboundMessageType::kReadyRoomResponse:
    case Game::OutboundMessageType::kMonsterSpawn:
    case Game::OutboundMessageType::kDropListSnapshot:
    case Game::OutboundMessageType::kLootRejected:
    case Game::OutboundMessageType::kInventorySnapshot:
        return true;
    }

    return false;
}

bool Server::sendTcpError(
    int clientFd,
    Net::TcpPacketType failedType,
    Net::TcpErrorCode errorCode,
    std::vector<int>& disconnectedClients) {
    std::array<uint8_t, Net::kErrorPacketSize> packet{};
    if (!Net::serializeErrorPacket(failedType, errorCode, packet)) {
        return false;
    }

    return sendPacketToClient(clientFd, packet.data(), packet.size(), disconnectedClients);
}

bool Server::flushTcpOutbound(
    ClientConnection& connection,
    std::vector<int>& disconnectedClients) {
    while (connection.hasPendingOutbound()) {
        const Net::SendResult result = listener_.sendSomeToClient(
            connection.clientFd(),
            connection.pendingOutboundData(),
            connection.pendingOutboundSize());
        if (result.status == Net::SendStatus::kSent) {
            connection.consumeOutboundBytes(result.bytesSent);
            continue;
        }
        if (result.status == Net::SendStatus::kWouldBlock) {
            if (!setTcpClientWriteInterest(connection, true)) {
                markClientForDisconnect(disconnectedClients, connection.clientFd());
                return false;
            }
            return true;
        }

        markClientForDisconnect(disconnectedClients, connection.clientFd());
        return false;
    }

    if (!setTcpClientWriteInterest(connection, false)) {
        markClientForDisconnect(disconnectedClients, connection.clientFd());
        return false;
    }
    return true;
}

bool Server::sendOrQueueTcpPacket(
    ClientConnection& connection,
    const uint8_t* data,
    size_t size,
    std::vector<int>& disconnectedClients) {
    const uint8_t* pendingData = data;
    size_t pendingSize = size;

    if (!connection.hasPendingOutbound()) {
        const Net::SendResult result =
            listener_.sendSomeToClient(connection.clientFd(), data, size);
        if (result.status == Net::SendStatus::kSent) {
            if (result.bytesSent >= size) {
                return true;
            }
            pendingData = data + result.bytesSent;
            pendingSize = size - result.bytesSent;
        } else if (result.status != Net::SendStatus::kWouldBlock) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return false;
        }
    }

    if (!connection.enqueueOutbound(
            pendingData,
            pendingSize,
            kTcpOutboundPendingLimit)) {
        markClientForDisconnect(disconnectedClients, connection.clientFd());
        return false;
    }

    if (!setTcpClientWriteInterest(connection, true)) {
        markClientForDisconnect(disconnectedClients, connection.clientFd());
        return false;
    }

    return true;
}

bool Server::registerTcpClientWithEventLoop(ClientConnection& connection) {
    if (networkEventLoop_ == nullptr) {
        return true;
    }

    const bool writable = connection.hasPendingOutbound();
    const Net::NetworkEventLoopStatus status = networkEventLoop_->registerFd(
        Net::NetworkFdToken{connection.clientFd(), connection.fdGeneration()},
        Net::NetworkEventRole::kTcpClient,
        tcpClientInterestMask(writable));
    if (status != Net::NetworkEventLoopStatus::kOk) {
        return false;
    }

    connection.setTcpWriteInterestEnabled(writable);
    return true;
}

bool Server::setTcpClientWriteInterest(ClientConnection& connection, bool enabled) {
    if (networkEventLoop_ == nullptr) {
        connection.setTcpWriteInterestEnabled(false);
        return true;
    }
    if (connection.tcpWriteInterestEnabled() == enabled) {
        return true;
    }

    const Net::NetworkEventLoopStatus status = networkEventLoop_->modifyFd(
        Net::NetworkFdToken{connection.clientFd(), connection.fdGeneration()},
        tcpClientInterestMask(enabled));
    if (status != Net::NetworkEventLoopStatus::kOk) {
        return false;
    }

    connection.setTcpWriteInterestEnabled(enabled);
    return true;
}

uint64_t Server::allocateTcpFdGeneration() {
    const uint64_t generation = nextTcpFdGeneration_;
    ++nextTcpFdGeneration_;
    if (nextTcpFdGeneration_ == 0) {
        nextTcpFdGeneration_ = 1;
    }
    return generation;
}

bool Server::sendPacketToClient(
    int clientFd,
    const uint8_t* data,
    size_t size,
    std::vector<int>& disconnectedClients) {
    if (networkEventLoop_ != nullptr) {
        auto connectionIt = connections_.find(clientFd);
        if (connectionIt == connections_.end()) {
            markClientForDisconnect(disconnectedClients, clientFd);
            return false;
        }
        return sendOrQueueTcpPacket(
            *connectionIt->second,
            data,
            size,
            disconnectedClients);
    }

    if (listener_.sendToClient(clientFd, data, size)) {
        return true;
    }

    markClientForDisconnect(disconnectedClients, clientFd);
    return false;
}

bool Server::broadcastBattleStart(
    const Game::RoomCommandResult& result,
    std::vector<int>& disconnectedClients) {
    if (result.playerSessionIds.size() < 2u) {
        return true;
    }
    enqueueRudpBattleStartEvent(result, Util::now());

    // 패킷을 client마다 따로 만들지 않고 한 번만 만듦. 어차피 같은 room의 두 참가자에게 보내는 내용이 동일하니까. 즉, 직렬화 비용은 한 번만 지불하고 재사용한다.
    std::array<uint8_t, Net::kBattleStartPacketSize> packet{};
    Net::serializeBattleStartPacket(
        result.room.roomId,
        result.playerSessionIds[0],
        result.playerSessionIds[1],
        packet);

    bool allSucceeded = true;   // 전체 성공 여부 추적 변수. 하나라도 실패 시 false로 바꾼다.
    for (const auto& entry : connections_) {    // 서버에 연결된 모든 clients 순회
        const uint64_t sessionId = entry.second->sessionId();   // entry.second는 ClientConnection 객체 포인터
        if (!isRoomMember(result, sessionId)) {
            continue;
        }

        if (!sendPacketToClient(entry.first, packet.data(), packet.size(), disconnectedClients)) {
            allSucceeded = false;
        }
    }

    return allSucceeded;
}

bool Server::broadcastMonsterSpawn(
    const Game::RoomCommandResult& result,
    std::vector<int>& disconnectedClients) {
    std::array<uint8_t, Net::kMonsterSpawnPacketSize> packet{};
    Net::serializeMonsterSpawnPacket(
        result.room.roomId,
        result.monster.monsterId,
        result.monster.monsterTypeId,
        result.monster.maxHp,
        packet);

    bool allSucceeded = true;
    for (const auto& entry : connections_) {
        if (!isRoomMember(result, entry.second->sessionId())) {
            continue;
        }

        if (!sendPacketToClient(entry.first, packet.data(), packet.size(), disconnectedClients)) {
            allSucceeded = false;
        }
    }

    return allSucceeded;
}

bool Server::broadcastMonsterDeath(
    const Game::RoomCommandResult& result,
    std::vector<int>& disconnectedClients) {
    enqueueRudpMonsterDeathEvent(result, Util::now());

    std::array<uint8_t, Net::kMonsterDeathPacketSize> packet{};
    Net::serializeMonsterDeathPacket(result.room.roomId, result.monster.monsterId, packet);

    bool allSucceeded = true;
    for (const auto& entry : connections_) {
        if (!isRoomMember(result, entry.second->sessionId())) {
            continue;
        }

        if (!sendPacketToClient(entry.first, packet.data(), packet.size(), disconnectedClients)) {
            allSucceeded = false;
        }
    }

    return allSucceeded;
}

bool Server::broadcastDropListSnapshot(
    const Game::RoomCommandResult& result,
    std::vector<int>& disconnectedClients) {
    std::vector<uint8_t> packet;
    const std::vector<Net::TcpDropEntry> drops = toTcpDropEntries(result.drops);
    if (!Net::serializeDropListSnapshotPacket(result.room.roomId, drops, packet)) {
        return false;
    }

    bool allSucceeded = true;
    for (const auto& entry : connections_) {
        if (!isRoomMember(result, entry.second->sessionId())) {
            continue;
        }

        if (!sendPacketToClient(entry.first, packet.data(), packet.size(), disconnectedClients)) {
            allSucceeded = false;
        }
    }

    return allSucceeded;
}

bool Server::broadcastLootResolved(
    const Game::RoomCommandResult& result,
    std::vector<int>& disconnectedClients) {
    enqueueRudpLootResolvedEvent(result, Util::now());

    std::array<uint8_t, Net::kLootResolvedPacketSize> packet{};
    Net::serializeLootResolvedPacket(
        result.room.roomId,
        result.drop.dropId,
        result.winnerSessionId,
        result.drop.itemId,
        result.drop.quantity,
        packet);

    bool allSucceeded = true;
    for (const auto& entry : connections_) {
        if (!isRoomMember(result, entry.second->sessionId())) {
            continue;
        }

        if (!sendPacketToClient(entry.first, packet.data(), packet.size(), disconnectedClients)) {
            allSucceeded = false;
        }
    }

    return allSucceeded;
}

bool Server::sendLootRejected(
    int clientFd,
    const Game::RoomCommandResult& result,
    std::vector<int>& disconnectedClients) {
    std::array<uint8_t, Net::kLootRejectedPacketSize> packet{};
    Net::serializeLootRejectedPacket(
        result.room.roomId,
        result.drop.dropId,
        toTcpLootRejectReason(result.lootRejectReason),
        packet);
    return sendPacketToClient(clientFd, packet.data(), packet.size(), disconnectedClients);
}

bool Server::sendInventorySnapshot(
    int clientFd,
    const Game::InventorySnapshot& inventory,
    std::vector<int>& disconnectedClients) {
    std::vector<uint8_t> packet;
    const std::vector<Net::TcpInventoryEntry> entries = toTcpInventoryEntries(inventory.entries);
    if (!Net::serializeInventorySnapshotPacket(
            inventory.sessionId,
            inventory.currentWeight,
            inventory.maxWeight,
            entries,
            packet)) {
        return false;
    }

    return sendPacketToClient(clientFd, packet.data(), packet.size(), disconnectedClients);
}

int Server::findClientFdForSession(uint64_t sessionId) const {
    for (const auto& entry : connections_) {
        if (entry.second->sessionId() == sessionId) {
            return entry.first;
        }
    }

    return -1;
}

void Server::broadcastStateSnapshots(bool clientListChanged, bool roomListChanged) {
    while (!connections_.empty() && (clientListChanged || roomListChanged)) {
        std::vector<uint8_t> clientSnapshotPacket;
        std::vector<uint8_t> roomSnapshotPacket;

        if (clientListChanged) {
            const std::vector<uint64_t> sessionIds = collectActiveSessionIds();
            if (!Net::serializeClientListSnapshotPacket(sessionIds, clientSnapshotPacket)) {
                return;
            }
        }

        if (roomListChanged) {
            const std::vector<Net::TcpRoomEntry> rooms = collectRoomEntries();
            if (!Net::serializeRoomListSnapshotPacket(rooms, roomSnapshotPacket)) {
                return;
            }
        }

        std::vector<int> failedClients;
        for (const auto& entry : connections_) {
            if (clientListChanged &&
                !sendPacketToClient(
                    entry.first,
                    clientSnapshotPacket.data(),
                    clientSnapshotPacket.size(),
                    failedClients)) {
                continue;
            }

            if (roomListChanged &&
                !sendPacketToClient(
                    entry.first,
                    roomSnapshotPacket.data(),
                    roomSnapshotPacket.size(),
                    failedClients)) {
                continue;
            }
        }

        if (failedClients.empty()) {
            return;
        }

        clientListChanged = true;
        roomListChanged = true;
        for (int clientFd : failedClients) {
            disconnectClient(clientFd);
        }
    }
}

bool Server::disconnectClient(int clientFd) {
    auto it = connections_.find(clientFd);
    if (it == connections_.end()) {
        return false;
    }

    const uint64_t sessionId = it->second->sessionId();
    const bool roomChanged = roomManager_.leaveRoom(sessionId).ok;
    if (networkEventLoop_ != nullptr) {
        networkEventLoop_->unregisterFd(
            Net::NetworkFdToken{clientFd, it->second->fdGeneration()});
    }
    rudpSessionBinder_.removeBySessionId(sessionId);
    rudpInputCommandSequenceTracker_.removeSession(sessionId);
    rudpMoveInputGuard_.removeSession(sessionId);
    rudpMoveDispatchStateBySession_.erase(sessionId);
    clearRudpReliableEventsForSession(sessionId);
    clearRudpOutboundSequenceForSession(sessionId);
    rudpBindingCountSnapshot_.store(
        rudpSessionBinder_.size(),
        std::memory_order_relaxed);
    sessionManager_.remove(it->second->remoteKey());
    listener_.closeClient(clientFd);
    connections_.erase(it);
    activeConnectionCount_.store(connections_.size(), std::memory_order_relaxed);
    sessionCountSnapshot_.store(sessionManager_.size(), std::memory_order_relaxed);
    return roomChanged;
}

void Server::closeAllConnections() {
    std::vector<int> clientFds;
    clientFds.reserve(connections_.size());
    for (const auto& entry : connections_) {
        clientFds.push_back(entry.first);
    }

    for (int clientFd : clientFds) {
        disconnectClient(clientFd);
    }

    activeConnectionCount_.store(0, std::memory_order_relaxed);
    sessionCountSnapshot_.store(sessionManager_.size(), std::memory_order_relaxed);
}
}  // namespace Core
