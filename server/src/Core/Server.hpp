#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include "Core/ClientConnection.hpp"
#include "Core/SessionManager.hpp"
#include "Game/OutboundSendQueue.hpp"
#include "Game/RoomEventDispatcher.hpp"
#include "Game/RoomEventMetrics.hpp"
#include "Game/RoomManager.hpp"
#include "Net/NetworkEventLoop.hpp"
#include "Net/RudpInputCommandSequenceTracker.hpp"
#include "Net/RudpMetaResponseIdempotencyTracker.hpp"
#include "Net/RudpMetaResponsePayload.hpp"
#include "Net/RudpMoveInputGuard.hpp"
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
    size_t inputNoRoomRejected{0};
    size_t moveAccepted{0};
    size_t moveDispatched{0};
    size_t moveApplyRejected{0};
    size_t moveInvalidReservedFlagsRejected{0};
    size_t moveRateLimitedRejected{0};
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

struct RudpServerSnapshotStats {
    size_t built{0};
    size_t sent{0};
    size_t sendErrors{0};
    size_t skippedNoBoundEndpoint{0};
    size_t serializeFailed{0};
};

enum class RudpServerReliableEventTrackResult {
    kTracked,
    kDuplicateSequence,
    kDuplicateLogicalEvent,
    kInvalidSession,
    kInvalidDescriptor,
    kInvalidPacketBytes,
    kNoBoundEndpoint,
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
    RudpServerSnapshotStats rudpSnapshotStats() const;
    size_t rudpPeerCount() const;
    size_t rudpBindingCount() const;
    size_t rudpReliableEventPendingCount() const;

private:
    void runTickLoop();
#if defined(__linux__)
    void runLinuxEpollLoop();
    void processLinuxEpollEvents(
        const std::vector<Net::NetworkEvent>& events,
        Util::TimePoint now);
#endif
    void tickOnce();
    void acceptNewClients(Util::TimePoint now);
    void processActiveConnections(Util::TimePoint now);
    void processRuntimeMaintenance(Util::TimePoint now);
    void processRuntimeTimerMaintenance(Util::TimePoint now);
    void processReadableTcpClient(
        ClientConnection& connection,
        Util::TimePoint now,
        std::vector<int>& disconnectedClients,
        bool& outRoomListChanged,
        bool drainUntilWouldBlock);
    bool flushTcpOutbound(
        ClientConnection& connection,
        std::vector<int>& disconnectedClients);
    bool sendOrQueueTcpPacket(
        ClientConnection& connection,
        const uint8_t* data,
        size_t size,
        std::vector<int>& disconnectedClients);
    bool registerTcpClientWithEventLoop(ClientConnection& connection);
    bool setTcpClientWriteInterest(ClientConnection& connection, bool enabled);
    uint64_t allocateTcpFdGeneration();
    void processRudpSocket(Util::TimePoint now);
    void processRudpDeliveries(
        const std::vector<Net::RudpPacketDelivery>& deliveries,
        Util::TimePoint now);
    void processRudpHelloDelivery(
        const Net::RudpPacketDelivery& delivery,
        Util::TimePoint now);
    void processRudpAdapterGate(
        const Net::RudpPacketDelivery& delivery,
        Util::TimePoint now);
    void dispatchRudpMoveInput(
        uint64_t sessionId,
        const Net::RudpInputCommandMoveArgs& move,
        Util::TimePoint now);
    bool integrateRudpMoveInput(uint64_t sessionId, Util::TimePoint now);
    void integrateRudpMoveInputsForRoom(uint32_t roomId, Util::TimePoint now);
    void pruneRudpMoveInputsWithoutRoom();
    RudpServerReliableEventTrackResult trackRudpReliableEventForSession(
        uint64_t sessionId,
        const Net::RudpReliableEventDescriptor& descriptor,
        uint32_t sequence,
        const std::vector<uint8_t>& packetBytes,
        Util::TimePoint now);
    RudpServerReliableEventTrackResult trackAndSendRudpReliableEventForSession(
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
    uint32_t nextRudpOutboundSequenceForSession(uint64_t sessionId);
    void clearRudpReliableEventsForSession(uint64_t sessionId);
    void clearRudpOutboundSequenceForSession(uint64_t sessionId);
    std::optional<Net::UdpEndpoint> findBoundRudpEndpointForSession(
        uint64_t sessionId) const;
    void processRudpReliableEventAcks(
        const std::vector<Net::RudpPacketDelivery>& deliveries);
    void consumeRudpReliableEventAck(
        const Net::UdpEndpoint& endpoint,
        uint32_t ack,
        uint32_t ackBits);
    void processRudpPeerLifecycle(Util::TimePoint now);
    void processRudpRetransmissions(Util::TimePoint now);
    void processRudpReliableEventRetransmissions(Util::TimePoint now);
    void processRudpMovementSnapshots(Util::TimePoint now);
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
    bool dispatchRudpRoomEvent(const Game::RoomEvent& event, Util::TimePoint now);
    bool drainInlineRoomEvents(std::vector<int>& disconnectedClients);
    bool drainInlineRudpRoomEvents(Util::TimePoint now);
    bool applyInlineRoomEvent(
        uint32_t roomId,
        const Game::RoomEvent& event,
        std::vector<int>& disconnectedClients);
    bool applyInlineRudpRoomEvent(
        uint32_t roomId,
        const Game::RoomEvent& event,
        Util::TimePoint now);
    bool flushOutboundQueue(std::vector<int>& disconnectedClients);
    bool flushRudpOutboundQueue(Util::TimePoint now);
    bool flushOutboundEnvelope(
        const Game::OutboundEnvelope& envelope,
        std::vector<int>& disconnectedClients);
    bool flushRudpOutboundEnvelope(
        const Game::OutboundEnvelope& envelope,
        Util::TimePoint now);
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
    Net::RudpMoveInputGuard rudpMoveInputGuard_;
    Net::RudpMetaResponseIdempotencyTracker rudpMetaResponseIdempotencyTracker_;
    struct RudpMoveDispatchState {
        Util::TimePoint lastIntegratedAt{};
        int16_t currentDirX{0};
        int16_t currentDirY{0};
    };
    std::unordered_map<uint64_t, RudpMoveDispatchState> rudpMoveDispatchStateBySession_;
    struct RudpSnapshotRoomState {
        Util::TimePoint lastSentAt{};
        uint32_t serverTick{0};
        bool hasSent{false};
    };
    std::unordered_map<uint32_t, RudpSnapshotRoomState> rudpSnapshotStateByRoom_;
    std::unordered_map<uint64_t, Net::RudpReliableEventSendQueue>
        rudpReliableEventQueues_;
    std::unordered_map<uint64_t, uint32_t> rudpOutboundNextSequenceBySession_;
    std::unordered_map<int, std::unique_ptr<ClientConnection>> connections_;
    Net::NetworkEventLoop* networkEventLoop_;
    uint64_t nextTcpFdGeneration_;
    std::atomic<int> linuxWakeupFd_;
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
    std::atomic<size_t> rudpBindingInputNoRoomRejected_;
    std::atomic<size_t> rudpBindingMoveAccepted_;
    std::atomic<size_t> rudpBindingMoveDispatched_;
    std::atomic<size_t> rudpBindingMoveApplyRejected_;
    std::atomic<size_t> rudpBindingMoveInvalidReservedFlagsRejected_;
    std::atomic<size_t> rudpBindingMoveRateLimitedRejected_;
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
    std::atomic<size_t> rudpSnapshotBuilt_;
    std::atomic<size_t> rudpSnapshotSent_;
    std::atomic<size_t> rudpSnapshotSendErrors_;
    std::atomic<size_t> rudpSnapshotSkippedNoBoundEndpoint_;
    std::atomic<size_t> rudpSnapshotSerializeFailed_;
    std::atomic<bool> running_;
    uint16_t port_;
};
}  // namespace Core
