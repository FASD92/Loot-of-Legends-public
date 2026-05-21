#include "Core/Server.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <iterator>
#include <thread>
#include <utility>
#include <vector>

#include "Core/Session.hpp"
#include "Net/RudpHelloPayload.hpp"
#include "Net/RudpInputCommandPayload.hpp"
#include "Net/TcpPacket.hpp"
#include "Util/Time.hpp"

namespace {
constexpr std::chrono::milliseconds kSessionTimeout(10000);
constexpr std::chrono::milliseconds kLoopSleep(1);
constexpr size_t kReceiveBufferSize = 512;
constexpr size_t kRudpMaxPacketsPerTick = 64;

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
}  // namespace

namespace Core {
Server::Server(uint16_t port)
    : Server(port, kSessionTimeout) {}

Server::Server(uint16_t port, std::chrono::milliseconds rudpPeerTimeout)
    : sessionManager_(kSessionTimeout),
      roomManager_(),
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
      rudpBindingCountSnapshot_(0),
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
    while (running_.load()) {
        tickOnce();
        std::this_thread::sleep_for(kLoopSleep);
    }

    closeAllConnections();
    udpSocket_.close();
}

void Server::requestStop() {
    running_.store(false);
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
    return stats;
}

size_t Server::rudpPeerCount() const {
    return rudpPeerCountSnapshot_.load(std::memory_order_relaxed);
}

size_t Server::rudpBindingCount() const {
    return rudpBindingCountSnapshot_.load(std::memory_order_relaxed);
}

void Server::tickOnce() {
    Util::TimePoint now = Util::now();
    acceptNewClients(now);
    processActiveConnections(now);
    processRudpSocket(now);
    processRudpPeerLifecycle(now);
    processRudpRetransmissions(now);
    sessionManager_.tick(now);
    sessionCountSnapshot_.store(sessionManager_.size(), std::memory_order_relaxed);
}

void Server::acceptNewClients(Util::TimePoint now) {
    bool membershipChanged = false;

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
        if (!listener_.sendToClient(clientFd, welcomePacket.data(), welcomePacket.size())) {
            listener_.closeClient(clientFd);
            sessionManager_.remove(remoteKey);
            sessionCountSnapshot_.store(sessionManager_.size(), std::memory_order_relaxed);
            continue;
        }

        connections_.emplace(
            clientFd,
            std::make_unique<ClientConnection>(clientFd, session->sessionId(), remoteKey, now));
        activeConnectionCount_.store(connections_.size(), std::memory_order_relaxed);
        sessionCountSnapshot_.store(sessionManager_.size(), std::memory_order_relaxed);
        membershipChanged = true;
    }

    if (membershipChanged) {
        broadcastStateSnapshots(true, false);
    }
}

void Server::processActiveConnections(Util::TimePoint now) {
    std::array<uint8_t, kReceiveBufferSize> buffer{};   // 소켓에서 읽은 원시 byte를 담을 임시 버퍼
    std::vector<int> disconnectedClients;
    std::vector<uint8_t> framedPacket;  // 프레이밍이 끝난 완전한 패킷
    bool roomListChanged = false;   // 이번 틱 동안 룸 목록이 바뀌었는지(create, join, leave) 기록하는 flag

    for (const auto& entry : connections_) {
        ClientConnection& connection = *entry.second;
        size_t received = 0;
        Net::ReceiveStatus status = listener_.receiveFromClient(
            connection.clientFd(),
            buffer.data(),
            buffer.size(),
            received);

        if (status == Net::ReceiveStatus::kWouldBlock) {
            continue;
        }

        if (status == Net::ReceiveStatus::kClosed || status == Net::ReceiveStatus::kError) {
            disconnectedClients.push_back(connection.clientFd());
            continue;
        }

        connection.updateLastHeard(now);
        auto session = sessionManager_.find(connection.remoteKey());
        if (!session) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            continue;
        }
        session->updateLastHeard(now);

        if (!connection.packetReader().appendBytes(buffer.data(), received)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            continue;
        }

        while (true) {
            Net::TcpPacketReadResult readResult =
                connection.packetReader().tryReadPacket(framedPacket);
            if (readResult == Net::TcpPacketReadResult::kNeedMoreData) {
                break;
            }

            if (readResult == Net::TcpPacketReadResult::kInvalidPacket) {
                markClientForDisconnect(disconnectedClients, connection.clientFd());
                break;
            }

            if (!handleRoomPacket(
                    connection,
                    framedPacket,
                    disconnectedClients,
                    roomListChanged)) {
                break;
            }
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

void Server::processRudpSocket(Util::TimePoint now) {
    const Net::RudpSocketDrainSummary summary =
        Net::drainRudpSocket(udpSocket_, rudpPeerRegistry_, now, kRudpMaxPacketsPerTick);
    accumulateRudpDrainStats(summary);
    processRudpDeliveries(summary.deliveries, now);
}

void Server::processRudpDeliveries(
    const std::vector<Net::RudpPacketDelivery>& deliveries,
    Util::TimePoint now) {
    for (const Net::RudpPacketDelivery& delivery : deliveries) {
        if (delivery.header.packetType != static_cast<uint16_t>(Net::RudpPacketType::kHello)) {
            rudpBindingIgnoredNonHello_.fetch_add(1, std::memory_order_relaxed);
            processRudpAdapterGate(delivery);
            continue;
        }
        processRudpHelloDelivery(delivery, now);
    }
}

void Server::processRudpAdapterGate(const Net::RudpPacketDelivery& delivery) {
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
        break;
    case Net::RudpInputCommandSequenceResult::kStale:
        rudpBindingInputSequenceStaleRejected_.fetch_add(
            1,
            std::memory_order_relaxed);
        break;
    case Net::RudpInputCommandSequenceResult::kAmbiguous:
        rudpBindingInputSequenceAmbiguousRejected_.fetch_add(
            1,
            std::memory_order_relaxed);
        break;
    case Net::RudpInputCommandSequenceResult::kInvalidSession:
        rudpBindingInputSequenceInvalidSessionRejected_.fetch_add(
            1,
            std::memory_order_relaxed);
        break;
    }
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

        const Game::RoomCommandResult result = roomManager_.markReady(session->sessionId());
        if (!result.ok) {   // 도메인 실패 처리: 방에 속하지 않았거나, 내부 매핑이 깨졌을 경우 <- 요청 형식은 ok!
            std::array<uint8_t, Net::kErrorPacketSize> errorPacket{};
            Net::serializeErrorPacket(
                Net::TcpPacketType::kReadyRoomRequest,
                toTcpErrorCode(result.error),
                errorPacket);
            return sendPacketToClient(
                connection.clientFd(),
                errorPacket.data(),
                errorPacket.size(),
                disconnectedClients);
        }

        std::array<uint8_t, Net::kReadyRoomStatusPacketSize> responsePacket{};
        Net::serializeReadyRoomResponsePacket(
            result.room.roomId,
            result.room.readyPlayerCount,
            result.room.playerCount,
            responsePacket);
        if (!sendPacketToClient(
                connection.clientFd(),
                responsePacket.data(),
                responsePacket.size(),
                disconnectedClients)) {
            return false;
        }
        // battleJustStarted는 RoomManager::markReady() 안에서 room.tryStartBattle() 결과를 담아 돌려준다.
        if (result.battleJustStarted && !broadcastBattleStart(result, disconnectedClients)) {
            return false;
        }

        if (result.battleJustStarted) {
            const Game::RoomCommandResult spawnResult = roomManager_.spawnMonster(result.room.roomId);
            if (spawnResult.ok &&
                spawnResult.monsterJustSpawned &&
                !broadcastMonsterSpawn(spawnResult, disconnectedClients)) {
                return false;
            }
        }

        return true;
    }

    case Net::TcpPacketType::kMonsterDeathRequest: {
        uint32_t monsterId = 0;
        if (!Net::parseMonsterDeathRequestPacket(packet.data(), packet.size(), header, monsterId)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return false;
        }

        const Game::RoomCommandResult result =
            roomManager_.defeatMonster(session->sessionId(), monsterId);
        if (!result.ok) {
            std::array<uint8_t, Net::kErrorPacketSize> errorPacket{};
            Net::serializeErrorPacket(
                Net::TcpPacketType::kMonsterDeathRequest,
                toTcpErrorCode(result.error),
                errorPacket);
            return sendPacketToClient(
                connection.clientFd(),
                errorPacket.data(),
                errorPacket.size(),
                disconnectedClients);
        }

        if (result.monsterJustDefeated &&
            !broadcastMonsterDeath(result, disconnectedClients)) {
            return false;
        }

        if (result.monsterJustDefeated &&
            !broadcastDropListSnapshot(result, disconnectedClients)) {
            return false;
        }

        return true;
    }

    case Net::TcpPacketType::kClickLootRequest: {
        uint32_t dropId = 0;
        if (!Net::parseClickLootRequestPacket(packet.data(), packet.size(), header, dropId)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return false;
        }

        const Game::RoomCommandResult result =
            roomManager_.claimLoot(session->sessionId(), dropId);
        if (!result.ok) {
            std::array<uint8_t, Net::kErrorPacketSize> errorPacket{};
            Net::serializeErrorPacket(
                Net::TcpPacketType::kClickLootRequest,
                toTcpErrorCode(result.error),
                errorPacket);
            return sendPacketToClient(
                connection.clientFd(),
                errorPacket.data(),
                errorPacket.size(),
                disconnectedClients);
        }

        if (result.lootRejected) {
            return sendLootRejected(connection.clientFd(), result, disconnectedClients);
        }

        if (result.lootJustClaimed &&
            !broadcastLootResolved(result, disconnectedClients)) {
            return false;
        }

        if (result.lootJustClaimed &&
            !sendInventorySnapshot(connection.clientFd(), result.inventory, disconnectedClients)) {
            return false;
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

bool Server::sendPacketToClient(
    int clientFd,
    const uint8_t* data,
    size_t size,
    std::vector<int>& disconnectedClients) {
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
                !listener_.sendToClient(
                    entry.first,
                    clientSnapshotPacket.data(),
                    clientSnapshotPacket.size())) {
                failedClients.push_back(entry.first);
                continue;
            }

            if (roomListChanged &&
                !listener_.sendToClient(
                    entry.first,
                    roomSnapshotPacket.data(),
                    roomSnapshotPacket.size())) {
                failedClients.push_back(entry.first);
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
    rudpSessionBinder_.removeBySessionId(sessionId);
    rudpInputCommandSequenceTracker_.removeSession(sessionId);
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
