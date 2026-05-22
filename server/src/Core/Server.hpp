#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "Core/ClientConnection.hpp"
#include "Core/SessionManager.hpp"
#include "Game/OutboundSendQueue.hpp"
#include "Game/RoomEventDispatcher.hpp"
#include "Game/RoomEventMetrics.hpp"
#include "Game/RoomManager.hpp"
#include "Net/RudpInputCommandSequenceTracker.hpp"
#include "Net/RudpMetaResponseIdempotencyTracker.hpp"
#include "Net/RudpMetaResponsePayload.hpp"
#include "Net/RudpPeerRegistry.hpp"
#include "Net/RudpReliableEventSendQueue.hpp"
#include "Net/RudpRetransmissionFlush.hpp"
#include "Net/RudpSessionBinder.hpp"
#include "Net/RudpSocketDrain.hpp"
#include "Net/TcpPacket.hpp"
#include "Net/TcpListener.hpp"
#include "Net/UdpSocket.hpp"
#include "Util/Time.hpp"

namespace Core {
struct RudpServerDrainStats {
    size_t attempted{0};
    size_t delivered{0};
    size_t malformed{0};
    size_t invalidEndpoint{0};
    size_t ackOnly{0};
    size_t duplicate{0};
    size_t tooOld{0};
    size_t socketErrors{0};
};

struct RudpServerRetransmissionStats {
    size_t expired{0};
    size_t due{0};
    size_t resent{0};
    size_t sendErrors{0};
    size_t droppedPeers{0};
};

struct RudpServerBindingStats {
    size_t helloReceived{0};
    size_t bound{0};
    size_t refreshed{0};
    size_t unknownSession{0};
    size_t conflicts{0};
    size_t invalidEndpoint{0};
    size_t invalidPayload{0};
    size_t ignoredNonHello{0};
    size_t inputCandidates{0};
    size_t inputDecoded{0};
    size_t inputDecodeFailed{0};
    size_t unboundInputRejected{0};
    size_t unsupportedPacketIgnored{0};
    size_t inputSequenceAccepted{0};
    size_t inputSequenceDuplicateRejected{0};
    size_t inputSequenceStaleRejected{0};
    size_t inputSequenceAmbiguousRejected{0};
    size_t inputSequenceInvalidSessionRejected{0};
};

struct RudpServerReliableEventStats {
    size_t tracked{0};
    size_t duplicateSequence{0};
    size_t duplicateLogicalEvent{0};
    size_t invalidSession{0};
    size_t invalidDescriptor{0};
    size_t invalidPacketBytes{0};
};

struct RudpServerMetaResponseStats {
    size_t completedFirst{0};
    size_t completionDuplicate{0};
    size_t retryObserved{0};
    size_t retryDuplicate{0};
    size_t retryIgnoredAfterCompletion{0};
    size_t invalidPayload{0};
    size_t enqueued{0};
};

enum class RudpServerReliableEventTrackResult {
    kTracked,
    kDuplicateSequence,
    kDuplicateLogicalEvent,
    kInvalidSession,
    kInvalidDescriptor,
    kInvalidPacketBytes,
};

class Server {
public:
    explicit Server(uint16_t port);
    Server(uint16_t port, std::chrono::milliseconds rudpPeerTimeout);

    bool start();
    void run();
    void requestStop();
    uint16_t boundPort() const;
    uint16_t udpBoundPort() const;
    size_t activeConnectionCount() const;
    size_t sessionCount() const;
    RudpServerDrainStats rudpDrainStats() const;
    RudpServerRetransmissionStats rudpRetransmissionStats() const;
    RudpServerBindingStats rudpBindingStats() const;
    RudpServerReliableEventStats rudpReliableEventStats() const;
    RudpServerMetaResponseStats rudpMetaResponseStats() const;
    size_t rudpPeerCount() const;
    size_t rudpBindingCount() const;
    size_t rudpReliableEventPendingCount() const;

private:
    void tickOnce();
    void acceptNewClients(Util::TimePoint now);
    void processActiveConnections(Util::TimePoint now);
    void processRudpSocket(Util::TimePoint now);
    void processRudpDeliveries(
        const std::vector<Net::RudpPacketDelivery>& deliveries,
        Util::TimePoint now);
    void processRudpHelloDelivery(
        const Net::RudpPacketDelivery& delivery,
        Util::TimePoint now);
    void processRudpAdapterGate(const Net::RudpPacketDelivery& delivery);
    RudpServerReliableEventTrackResult trackRudpReliableEventForSession(
        uint64_t sessionId,
        const Net::RudpReliableEventDescriptor& descriptor,
        uint32_t sequence,
        const std::vector<uint8_t>& packetBytes,
        Util::TimePoint now);
    void enqueueRudpBattleStartEvent(
        const Game::RoomCommandResult& result,
        Util::TimePoint now);
    void enqueueRudpMonsterDeathEvent(
        const Game::RoomCommandResult& result,
        Util::TimePoint now);
    void enqueueRudpLootResolvedEvent(
        const Game::RoomCommandResult& result,
        Util::TimePoint now);
    bool observeRudpMetaResponseForSession(
        uint64_t sessionId,
        const Net::RudpMetaResponsePayload& payload,
        Util::TimePoint now);
    uint32_t nextRudpReliableEventSequenceForSession(uint64_t sessionId);
    void clearRudpReliableEventsForSession(uint64_t sessionId);
    void processRudpPeerLifecycle(Util::TimePoint now);
    void processRudpRetransmissions(Util::TimePoint now);
    void accumulateRudpDrainStats(const Net::RudpSocketDrainSummary& summary);
    void accumulateRudpRetransmissionStats(
        const Net::RudpRetransmissionFlushSummary& summary);
    size_t calculateRudpReliableEventPendingCount() const;
    std::vector<uint64_t> collectActiveSessionIds() const;
    std::vector<Net::TcpRoomEntry> collectRoomEntries() const;
    bool handleRoomPacket(
        ClientConnection& connection,
        const std::vector<uint8_t>& packet,
        std::vector<int>& disconnectedClients,
        bool& outRoomListChanged);
    bool dispatchTcpRoomEvent(
        ClientConnection& connection,
        Net::TcpPacketType failedType,
        const Game::RoomEvent& event,
        std::vector<int>& disconnectedClients);
    bool drainInlineRoomEvents(std::vector<int>& disconnectedClients);
    bool applyInlineRoomEvent(
        uint32_t roomId,
        const Game::RoomEvent& event,
        std::vector<int>& disconnectedClients);
    bool flushOutboundQueue(std::vector<int>& disconnectedClients);
    bool flushOutboundEnvelope(
        const Game::OutboundEnvelope& envelope,
        std::vector<int>& disconnectedClients);
    bool sendTcpError(
        int clientFd,
        Net::TcpPacketType failedType,
        Net::TcpErrorCode errorCode,
        std::vector<int>& disconnectedClients);
    bool sendPacketToClient(
        int clientFd,
        const uint8_t* data,
        size_t size,
        std::vector<int>& disconnectedClients);
    bool broadcastBattleStart(
        const Game::RoomCommandResult& result,
        std::vector<int>& disconnectedClients);
    bool broadcastMonsterSpawn(
        const Game::RoomCommandResult& result,
        std::vector<int>& disconnectedClients);
    bool broadcastMonsterDeath(
        const Game::RoomCommandResult& result,
        std::vector<int>& disconnectedClients);
    bool broadcastDropListSnapshot(
        const Game::RoomCommandResult& result,
        std::vector<int>& disconnectedClients);
    bool broadcastLootResolved(
        const Game::RoomCommandResult& result,
        std::vector<int>& disconnectedClients);
    bool sendLootRejected(
        int clientFd,
        const Game::RoomCommandResult& result,
        std::vector<int>& disconnectedClients);
    bool sendInventorySnapshot(
        int clientFd,
        const Game::InventorySnapshot& inventory,
        std::vector<int>& disconnectedClients);
    int findClientFdForSession(uint64_t sessionId) const;
    void broadcastStateSnapshots(bool clientListChanged, bool roomListChanged);
    bool disconnectClient(int clientFd);
    void closeAllConnections();

    friend struct ServerTestAccess;

    Net::TcpListener listener_;
    Net::UdpSocket udpSocket_;
    SessionManager sessionManager_;
    Game::RoomManager roomManager_;
    Game::RoomEventMetrics roomEventMetrics_;
    Game::RoomEventDispatcher roomEventDispatcher_;
    Game::OutboundSendQueue outboundSendQueue_;
    Net::RudpPeerRegistry rudpPeerRegistry_;
    Net::RudpSessionBinder rudpSessionBinder_;
    Net::RudpInputCommandSequenceTracker rudpInputCommandSequenceTracker_;
    Net::RudpMetaResponseIdempotencyTracker rudpMetaResponseIdempotencyTracker_;
    std::unordered_map<uint64_t, Net::RudpReliableEventSendQueue>
        rudpReliableEventQueues_;
    std::unordered_map<uint64_t, uint32_t> rudpReliableEventNextSequenceBySession_;
    std::unordered_map<int, std::unique_ptr<ClientConnection>> connections_;
    std::atomic<size_t> activeConnectionCount_;
    std::atomic<size_t> sessionCountSnapshot_;
    std::atomic<size_t> rudpPeerCountSnapshot_;
    std::atomic<size_t> rudpDrainAttempted_;
    std::atomic<size_t> rudpDrainDelivered_;
    std::atomic<size_t> rudpDrainMalformed_;
    std::atomic<size_t> rudpDrainInvalidEndpoint_;
    std::atomic<size_t> rudpDrainAckOnly_;
    std::atomic<size_t> rudpDrainDuplicate_;
    std::atomic<size_t> rudpDrainTooOld_;
    std::atomic<size_t> rudpDrainSocketErrors_;
    std::atomic<size_t> rudpRetransmissionExpired_;
    std::atomic<size_t> rudpRetransmissionDue_;
    std::atomic<size_t> rudpRetransmissionResent_;
    std::atomic<size_t> rudpRetransmissionSendErrors_;
    std::atomic<size_t> rudpRetransmissionDroppedPeers_;
    std::atomic<size_t> rudpBindingHelloReceived_;
    std::atomic<size_t> rudpBindingBound_;
    std::atomic<size_t> rudpBindingRefreshed_;
    std::atomic<size_t> rudpBindingUnknownSession_;
    std::atomic<size_t> rudpBindingConflicts_;
    std::atomic<size_t> rudpBindingInvalidEndpoint_;
    std::atomic<size_t> rudpBindingInvalidPayload_;
    std::atomic<size_t> rudpBindingIgnoredNonHello_;
    std::atomic<size_t> rudpBindingInputCandidates_;
    std::atomic<size_t> rudpBindingInputDecoded_;
    std::atomic<size_t> rudpBindingInputDecodeFailed_;
    std::atomic<size_t> rudpBindingUnboundInputRejected_;
    std::atomic<size_t> rudpBindingUnsupportedPacketIgnored_;
    std::atomic<size_t> rudpBindingInputSequenceAccepted_;
    std::atomic<size_t> rudpBindingInputSequenceDuplicateRejected_;
    std::atomic<size_t> rudpBindingInputSequenceStaleRejected_;
    std::atomic<size_t> rudpBindingInputSequenceAmbiguousRejected_;
    std::atomic<size_t> rudpBindingInputSequenceInvalidSessionRejected_;
    std::atomic<size_t> rudpBindingCountSnapshot_;
    std::atomic<size_t> rudpReliableEventTracked_;
    std::atomic<size_t> rudpReliableEventDuplicateSequence_;
    std::atomic<size_t> rudpReliableEventDuplicateLogicalEvent_;
    std::atomic<size_t> rudpReliableEventInvalidSession_;
    std::atomic<size_t> rudpReliableEventInvalidDescriptor_;
    std::atomic<size_t> rudpReliableEventInvalidPacketBytes_;
    std::atomic<size_t> rudpReliableEventPendingCountSnapshot_;
    std::atomic<size_t> rudpMetaResponseCompletedFirst_;
    std::atomic<size_t> rudpMetaResponseCompletionDuplicate_;
    std::atomic<size_t> rudpMetaResponseRetryObserved_;
    std::atomic<size_t> rudpMetaResponseRetryDuplicate_;
    std::atomic<size_t> rudpMetaResponseRetryIgnoredAfterCompletion_;
    std::atomic<size_t> rudpMetaResponseInvalidPayload_;
    std::atomic<size_t> rudpMetaResponseEnqueued_;
    std::atomic<bool> running_;
    uint16_t port_;
};
}  // namespace Core
