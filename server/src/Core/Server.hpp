#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Core/ClientConnection.hpp"
#include "Core/GameSessionAuthState.hpp"
#include "Core/MetaSessionClaimClient.hpp"
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
std::chrono::milliseconds defaultSessionTimeout();

constexpr size_t kRelease1PrimaryLatencyBucketCount = 13;
constexpr size_t kRudpReliableEventKindCount = 5;
constexpr size_t kRudpInputCommandOpMetricCount = 6;
constexpr size_t kTcpReadErrorErrnoMetricCount = 4;
constexpr size_t kTcpSendFailurePacketTypeMetricCount = 29;
inline constexpr std::array<uint32_t, kRelease1PrimaryLatencyBucketCount>
    kRelease1PrimaryLatencyBucketMs{
        1,
        2,
        5,
        10,
        20,
        33,
        50,
        75,
        100,
        150,
        200,
        500,
        1000};

struct RudpServerDrainStats {
    size_t attempted{0};
    size_t delivered{0};
    size_t malformed{0};
    size_t invalidEndpoint{0};
    size_t ackOnly{0};
    size_t duplicate{0};
    size_t tooOld{0};
    size_t socketErrors{0};
    size_t stoppedByWouldBlock{0};
    size_t stoppedByMaxPackets{0};
    size_t stoppedBySocketError{0};
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
    std::array<size_t, kRudpInputCommandOpMetricCount>
        inputNoRoomRejectedByOp{};
    size_t moveAccepted{0};
    size_t attackAccepted{0};
    size_t lootClaimAccepted{0};
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

struct Release1RuntimeMetricsSnapshot {
    size_t runtimeTickCount{0};
    size_t rudpDrainAttemptedCount{0};
    size_t rudpDrainDeliveredCount{0};
    size_t rudpDrainMalformedCount{0};
    size_t rudpDrainInvalidEndpointCount{0};
    size_t rudpDrainAckOnlyCount{0};
    size_t rudpDrainDuplicateCount{0};
    size_t rudpDrainTooOldCount{0};
    size_t rudpDrainSocketErrorCount{0};
    size_t rudpDrainStoppedByWouldBlockCount{0};
    size_t rudpDrainStoppedByMaxPacketsCount{0};
    size_t rudpDrainStoppedBySocketErrorCount{0};
    size_t rudpBindingHelloReceivedCount{0};
    size_t rudpBindingBoundCount{0};
    size_t rudpBindingRefreshedCount{0};
    size_t rudpBindingUnknownSessionCount{0};
    size_t rudpBindingConflictsCount{0};
    size_t rudpBindingInvalidEndpointCount{0};
    size_t rudpBindingInvalidPayloadCount{0};
    size_t rudpBindingIgnoredNonHelloCount{0};
    size_t rudpBindingUnboundInputRejectedCount{0};
    size_t rudpBindingUnsupportedPacketIgnoredCount{0};
    size_t rudpBindingInputCandidatesCount{0};
    size_t rudpBindingInputDecodedCount{0};
    size_t rudpBindingInputDecodeFailedCount{0};
    size_t rudpReliableEventTrackedCount{0};
    std::array<size_t, kRudpReliableEventKindCount>
        rudpReliableEventTrackedByKindCounts{};
    size_t rudpReliableEventPendingCount{0};
    size_t rudpRetransmissionExpiredCount{0};
    std::array<size_t, kRudpReliableEventKindCount>
        rudpReliableEventExpiredByKindCounts{};
    size_t rudpRetransmissionDueCount{0};
    size_t rudpRetransmissionResentCount{0};
    size_t rudpRetransmissionSendErrorCount{0};
    size_t rudpRetransmissionDroppedPeerCount{0};
    size_t rudpInputSequenceAcceptedCount{0};
    size_t rudpInputSequenceDuplicateRejectedCount{0};
    size_t rudpInputSequenceStaleRejectedCount{0};
    size_t rudpInputSequenceAmbiguousRejectedCount{0};
    size_t rudpInputSequenceInvalidSessionRejectedCount{0};
    size_t rudpBindingInputNoRoomRejectedCount{0};
    std::array<size_t, kRudpInputCommandOpMetricCount>
        rudpBindingInputNoRoomRejectedByOpCounts{};
    size_t rudpMoveAcceptedCount{0};
    size_t rudpMoveDispatchedCount{0};
    size_t rudpMoveApplyRejectedCount{0};
    size_t rudpMoveInvalidReservedFlagsRejectedCount{0};
    size_t rudpMoveRateLimitedRejectedCount{0};
    size_t rudpMoveReceiveToApplyLatencySampleCount{0};
    uint64_t rudpMoveReceiveToApplyLatencyTotalUs{0};
    std::array<size_t, kRelease1PrimaryLatencyBucketCount>
        rudpMoveReceiveToApplyLatencyBucketCounts{};
    size_t rudpAttackAcceptedCount{0};
    size_t rudpAttackReceiveToApplyLatencySampleCount{0};
    uint64_t rudpAttackReceiveToApplyLatencyTotalUs{0};
    std::array<size_t, kRelease1PrimaryLatencyBucketCount>
        rudpAttackReceiveToApplyLatencyBucketCounts{};
    size_t rudpLootClaimAcceptedCount{0};
    size_t rudpLootClaimReceiveToApplyLatencySampleCount{0};
    uint64_t rudpLootClaimReceiveToApplyLatencyTotalUs{0};
    std::array<size_t, kRelease1PrimaryLatencyBucketCount>
        rudpLootClaimReceiveToApplyLatencyBucketCounts{};
    size_t rudpStateSnapshotSentCount{0};
    size_t tcpDisconnectTotalCount{0};
    size_t tcpDisconnectMarkedReadClosedCount{0};
    size_t tcpDisconnectMarkedReadErrorCount{0};
    std::array<size_t, kTcpReadErrorErrnoMetricCount>
        tcpDisconnectMarkedReadErrorByErrnoCounts{};
    size_t tcpDisconnectMarkedPacketReaderRejectedCount{0};
    size_t tcpDisconnectMarkedInvalidPacketCount{0};
    size_t tcpDisconnectMarkedNetworkEventCount{0};
    size_t tcpDisconnectMarkedSendFailureCount{0};
    size_t tcpDisconnectMarkedOutboundQueueFullCount{0};
    size_t tcpDisconnectMarkedEventLoopUpdateFailureCount{0};
    size_t tcpDisconnectMarkedMissingConnectionCount{0};
    std::array<size_t, kTcpSendFailurePacketTypeMetricCount>
        tcpSendFailureByPacketTypeCounts{};
    size_t tcpSendFailureUnknownPacketTypeCount{0};
    size_t tcpCreateRoomRequestReceivedCount{0};
    size_t tcpJoinRoomRequestReceivedCount{0};
    size_t tcpCreateRoomResponseSentCount{0};
    size_t tcpJoinRoomResponseSentCount{0};
    size_t tcpRoomListSnapshotDirectCount{0};
    size_t tcpRoomListSnapshotBroadcastCount{0};
    size_t tcpRoomListSnapshotBroadcastRecipientCount{0};
    size_t tcpRoomListSnapshotBytesCount{0};
    size_t activeConnectionCount{0};
    size_t activeSessionCount{0};
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
    Server(uint16_t port, IMetaSessionClaimClient* metaSessionClaimClient);
    Server(uint16_t port, std::chrono::milliseconds rudpPeerTimeout);
    Server(
        uint16_t port,
        std::chrono::milliseconds rudpPeerTimeout,
        IMetaSessionClaimClient* metaSessionClaimClient);
    ~Server();

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
    Release1RuntimeMetricsSnapshot release1RuntimeMetricsSnapshot() const;
    std::string renderRelease1RuntimeMetricsTextfile() const;
    bool configureRelease1MetricsTextfile(
        const std::string& targetPath,
        std::chrono::milliseconds interval);
    bool writeRelease1RuntimeMetricsTextfile(
        std::string* errorMessage = nullptr) const;
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
    void processRelease1MetricsTextfile(Util::TimePoint now);
    void processPendingMetaSessionClaims(Util::TimePoint now);
    void processMetaSessionLivenessReports(Util::TimePoint now);
    void releaseQueuedAcceptedMetaSessionClaimsAfterShutdown();
    void enqueueMetaSessionClaim(
        int clientFd,
        uint64_t connectionId,
        const std::string& token);
    void startMetaSessionClaimWorker();
    void stopMetaSessionClaimWorker();
    void runMetaSessionClaimWorker();
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
        Util::TimePoint now,
        std::vector<int>& disconnectedClients,
        bool& outRoomListChanged);
    void processRudpHelloDelivery(
        const Net::RudpPacketDelivery& delivery,
        Util::TimePoint now);
    void processRudpAdapterGate(
        const Net::RudpPacketDelivery& delivery,
        Util::TimePoint now,
        std::vector<int>& disconnectedClients,
        bool& outRoomListChanged);
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
    bool sendPendingRudpReliableEventsForSession(uint64_t sessionId);
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
    std::optional<uint64_t> authenticatedSessionIdForClientFd(int clientFd) const;
    Net::TcpRoomDetailState buildRoomDetailState(
        const Game::Room& room,
        uint64_t viewerSessionId) const;
    bool handleTcpPacket(
        ClientConnection& connection,
        const std::vector<uint8_t>& packet,
        Util::TimePoint now,
        std::vector<int>& disconnectedClients,
        bool& outRoomListChanged);
    bool handleAuthenticateGameSessionPacket(
        ClientConnection& connection,
        const std::vector<uint8_t>& packet,
        Util::TimePoint now,
        std::vector<int>& disconnectedClients);
    bool completeAcceptedGameSessionClaim(
        ClientConnection& connection,
        GameSessionAuthState& authState,
        const MetaSessionClaimResult& result,
        Util::TimePoint now,
        std::vector<int>& disconnectedClients);
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
    bool dispatchRudpRoomEvent(
        const Game::RoomEvent& event,
        Util::TimePoint now,
        std::vector<int>& disconnectedClients,
        bool& outRoomListChanged);
    bool drainInlineRoomEvents(std::vector<int>& disconnectedClients);
    bool drainInlineRudpRoomEvents(
        Util::TimePoint now,
        std::vector<int>& disconnectedClients,
        bool& outRoomListChanged);
    bool applyInlineRoomEvent(
        uint32_t roomId,
        const Game::RoomEvent& event,
        std::vector<int>& disconnectedClients);
    bool applyInlineRudpRoomEvent(
        uint32_t roomId,
        const Game::RoomEvent& event,
        Util::TimePoint now,
        std::vector<int>& disconnectedClients,
        bool& outRoomListChanged);
    void recordRelease1RudpMoveReceiveToApplyLatency(
        Util::TimePoint receivedAt,
        Util::TimePoint appliedAt);
    void recordRelease1RudpAttackReceiveToApplyLatency(
        Util::TimePoint receivedAt,
        Util::TimePoint appliedAt);
    void recordRelease1RudpLootClaimReceiveToApplyLatency(
        Util::TimePoint receivedAt,
        Util::TimePoint appliedAt);
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
    void recordTcpSendFailurePacketType(const uint8_t* data, size_t size);
    bool sendPacketToClient(
        int clientFd,
        const uint8_t* data,
        size_t size,
        std::vector<int>& disconnectedClients);
    bool sendCurrentRoomListSnapshotToClient(
        int clientFd,
        std::vector<int>& disconnectedClients);
    bool sendRoomListSnapshotToLobbySessions(std::vector<int>& disconnectedClients);
    void scheduleRoomListSnapshotBroadcast(Util::TimePoint now);
    bool flushPendingRoomListSnapshotBroadcast(
        Util::TimePoint now,
        std::vector<int>& disconnectedClients);
    void processPendingRoomListSnapshotBroadcast(Util::TimePoint now);
    bool sendRoomDetailStateToSession(
        uint64_t sessionId,
        uint32_t roomId,
        std::vector<int>& disconnectedClients);
    bool broadcastRoomDetailState(
        uint32_t roomId,
        const std::vector<uint64_t>& sessionIds,
        std::vector<int>& disconnectedClients);
    bool sendLobbyReturnVisibility(
        uint64_t sessionId,
        uint32_t previousRoomId,
        Net::TcpLobbyReturnReason reason,
        std::vector<int>& disconnectedClients);
    bool completeBattleResultIfReady(
        uint32_t roomId,
        std::vector<int>& disconnectedClients,
        bool& outRoomListChanged);
    bool completeBattleResult(
        const Game::BattleFinalRankingResult& ranking,
        std::vector<int>& disconnectedClients,
        bool& outRoomListChanged);
    bool broadcastBattleFinalRanking(
        const Game::BattleFinalRankingResult& ranking,
        std::vector<int>& disconnectedClients);
    bool sendBattleResultLobbyReturn(
        const std::vector<uint64_t>& sessionIds,
        uint32_t previousRoomId,
        Net::TcpLobbyReturnReason reason,
        std::vector<int>& disconnectedClients);
    void closeBattleResultRoom(
        uint32_t roomId,
        const std::vector<uint64_t>& sessionIds);
    std::optional<std::string> nicknameForSession(uint64_t sessionId) const;
    bool broadcastBattleStart(
        const Game::RoomCommandResult& result,
        std::vector<int>& disconnectedClients);
    bool broadcastBattleLoadEntry(
        const Game::RoomCommandResult& result,
        std::vector<int>& disconnectedClients);
    bool broadcastArenaGameplayStart(
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
    bool broadcastMonsterHealthSnapshot(
        const Game::RoomCommandResult& result,
        std::vector<int>& disconnectedClients);
    bool broadcastDropListSnapshotV2(
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
    void broadcastStateSnapshots(
        bool clientListChanged,
        bool roomListChanged,
        Util::TimePoint now);
    bool disconnectClient(int clientFd);
    void closeAllConnections();

    friend struct ServerTestAccess;

    struct PendingMetaSessionClaimCompletion {
        int clientFd{-1};
        uint64_t connectionId{0};
        std::string gameSessionToken;
        MetaSessionClaimResult result;
    };
    struct PendingMetaSessionClaimInvocation {
        int clientFd{-1};
        uint64_t connectionId{0};
        std::string gameSessionToken;
    };
    struct MetaSessionClaimCompletionSink {
        std::mutex mutex;
        std::vector<PendingMetaSessionClaimCompletion> completions;
        std::atomic<bool> accepting{true};
#if defined(__linux__)
        std::atomic<int> linuxWakeupFd{-1};
#endif
    };
    struct MetaSessionClaimReleaseSink {
        explicit MetaSessionClaimReleaseSink(IMetaSessionClaimClient* client);
        void releaseAcceptedClaimWithoutServerSession(
            const MetaSessionClaimResult& result,
            uint64_t connectionId) const;
        void release(const MetaSessionReleaseRequest& request) const;

        IMetaSessionClaimClient* client{nullptr};
    };

    Net::TcpListener listener_;
    Net::UdpSocket udpSocket_;
    IMetaSessionClaimClient* metaSessionClaimClient_;
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
    std::unordered_map<int, GameSessionAuthState> gameSessionAuthByClientFd_;
    std::unordered_map<uint64_t, int> authenticatedClientFdByAccountId_;
    std::unordered_map<uint64_t, Util::TimePoint> lastMetaSessionLivenessByConnectionId_;
    std::shared_ptr<MetaSessionClaimCompletionSink> metaSessionClaimCompletionSink_;
    std::shared_ptr<MetaSessionClaimReleaseSink> metaSessionClaimReleaseSink_;
    std::mutex metaSessionClaimWorkerMutex_;
    std::condition_variable metaSessionClaimWorkerCondition_;
    std::deque<PendingMetaSessionClaimInvocation> pendingMetaSessionClaimInvocations_;
    std::thread metaSessionClaimWorker_;
    bool metaSessionClaimWorkerStopping_{false};
    std::unordered_set<uint64_t> lobbySessionIds_;
    Net::NetworkEventLoop* networkEventLoop_;
    uint64_t nextTcpFdGeneration_;
    std::atomic<int> linuxWakeupFd_;
    std::atomic<size_t> activeConnectionCount_;
    std::atomic<size_t> sessionCountSnapshot_;
    std::atomic<size_t> release1RuntimeTickCount_;
    std::string release1MetricsTextfilePath_;
    std::chrono::milliseconds release1MetricsTextfileInterval_;
    Util::TimePoint release1MetricsTextfileLastAttemptAt_;
    bool release1MetricsTextfileEnabled_;
    bool release1MetricsTextfileHasAttempted_;
    std::atomic<size_t> rudpPeerCountSnapshot_;
    std::atomic<size_t> rudpDrainAttempted_;
    std::atomic<size_t> rudpDrainDelivered_;
    std::atomic<size_t> rudpDrainMalformed_;
    std::atomic<size_t> rudpDrainInvalidEndpoint_;
    std::atomic<size_t> rudpDrainAckOnly_;
    std::atomic<size_t> rudpDrainDuplicate_;
    std::atomic<size_t> rudpDrainTooOld_;
    std::atomic<size_t> rudpDrainSocketErrors_;
    std::atomic<size_t> rudpDrainStoppedByWouldBlock_;
    std::atomic<size_t> rudpDrainStoppedByMaxPackets_;
    std::atomic<size_t> rudpDrainStoppedBySocketError_;
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
    std::array<std::atomic<size_t>, kRudpInputCommandOpMetricCount>
        rudpBindingInputNoRoomRejectedByOp_{};
    std::atomic<size_t> rudpBindingMoveAccepted_;
    std::atomic<size_t> rudpBindingAttackAccepted_;
    std::atomic<size_t> rudpBindingLootClaimAccepted_;
    std::atomic<size_t> rudpMoveReceiveToApplyLatencySampleCount_;
    std::atomic<uint64_t> rudpMoveReceiveToApplyLatencyTotalUs_;
    std::array<std::atomic<size_t>, kRelease1PrimaryLatencyBucketCount>
        rudpMoveReceiveToApplyLatencyBuckets_{};
    std::atomic<size_t> rudpAttackReceiveToApplyLatencySampleCount_;
    std::atomic<uint64_t> rudpAttackReceiveToApplyLatencyTotalUs_;
    std::array<std::atomic<size_t>, kRelease1PrimaryLatencyBucketCount>
        rudpAttackReceiveToApplyLatencyBuckets_{};
    std::atomic<size_t> rudpLootClaimReceiveToApplyLatencySampleCount_;
    std::atomic<uint64_t> rudpLootClaimReceiveToApplyLatencyTotalUs_;
    std::array<std::atomic<size_t>, kRelease1PrimaryLatencyBucketCount>
        rudpLootClaimReceiveToApplyLatencyBuckets_{};
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
    std::array<std::atomic<size_t>, kRudpReliableEventKindCount>
        rudpReliableEventTrackedByKind_{};
    std::array<std::atomic<size_t>, kRudpReliableEventKindCount>
        rudpReliableEventExpiredByKind_{};
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
    std::atomic<size_t> tcpDisconnectTotal_;
    std::atomic<size_t> tcpDisconnectMarkedReadClosed_;
    std::atomic<size_t> tcpDisconnectMarkedReadError_;
    std::array<std::atomic<size_t>, kTcpReadErrorErrnoMetricCount>
        tcpDisconnectMarkedReadErrorByErrno_{};
    std::atomic<size_t> tcpDisconnectMarkedPacketReaderRejected_;
    std::atomic<size_t> tcpDisconnectMarkedInvalidPacket_;
    std::atomic<size_t> tcpDisconnectMarkedNetworkEvent_;
    std::atomic<size_t> tcpDisconnectMarkedSendFailure_;
    std::atomic<size_t> tcpDisconnectMarkedOutboundQueueFull_;
    std::atomic<size_t> tcpDisconnectMarkedEventLoopUpdateFailure_;
    std::atomic<size_t> tcpDisconnectMarkedMissingConnection_;
    std::array<std::atomic<size_t>, kTcpSendFailurePacketTypeMetricCount>
        tcpSendFailureByPacketType_{};
    std::atomic<size_t> tcpSendFailureUnknownPacketType_;
    std::atomic<size_t> tcpCreateRoomRequestReceived_;
    std::atomic<size_t> tcpJoinRoomRequestReceived_;
    std::atomic<size_t> tcpCreateRoomResponseSent_;
    std::atomic<size_t> tcpJoinRoomResponseSent_;
    std::atomic<size_t> tcpRoomListSnapshotDirect_;
    std::atomic<size_t> tcpRoomListSnapshotBroadcast_;
    std::atomic<size_t> tcpRoomListSnapshotBroadcastRecipients_;
    std::atomic<size_t> tcpRoomListSnapshotBytes_;
    bool pendingRoomListSnapshotBroadcast_;
    Util::TimePoint pendingRoomListSnapshotBroadcastDueAt_;
    std::atomic<bool> running_;
    uint16_t port_;
};
}  // namespace Core
