#include "Core/Server.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
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
#include "Net/RudpBattleStartRosterPayload.hpp"
#include "Net/RudpGameEventPayload.hpp"
#include "Net/RudpHelloPayload.hpp"
#include "Net/RudpInputCommandPayload.hpp"
#include "Net/RudpStateSnapshotPayload.hpp"
#include "Util/PrometheusTextfileWriter.hpp"
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
constexpr std::chrono::milliseconds kDefaultSessionTimeout(60000);
constexpr std::chrono::milliseconds kLoopSleep(1);
constexpr size_t kReceiveBufferSize = 512;
constexpr size_t kRudpMaxPacketsPerTick = 48;
constexpr size_t kServerRoomEventQueueCapacity = 128;
constexpr size_t kTcpOutboundPendingLimit = 256 * 1024;
constexpr std::chrono::milliseconds kRudpSnapshotInterval(100);
constexpr std::chrono::milliseconds kRoomListSnapshotBroadcastDebounce(50);
constexpr std::chrono::milliseconds kMetaSessionLivenessInterval(300000);
constexpr uint64_t kTcpListenerGeneration = 1;
#if defined(__linux__)
constexpr uint64_t kUdpSocketGeneration = 2;
constexpr uint64_t kRuntimeTimerGeneration = 3;
constexpr uint64_t kRuntimeWakeupGeneration = 4;
constexpr std::chrono::milliseconds kLinuxEpollWaitForever(-1);
#endif
constexpr std::array<Net::RudpReliableEventKind, Core::kRudpReliableEventKindCount>
    kRudpReliableEventKinds{
        Net::RudpReliableEventKind::kBattleStart,
        Net::RudpReliableEventKind::kBattleStartRoster,
        Net::RudpReliableEventKind::kMonsterDeath,
        Net::RudpReliableEventKind::kLootResolved,
        Net::RudpReliableEventKind::kMetaResponse};
constexpr std::array<Net::RudpInputCommandOp, Core::kRudpInputCommandOpMetricCount>
    kRudpInputCommandOpMetrics{
        Net::RudpInputCommandOp::kReady,
        Net::RudpInputCommandOp::kMonsterDeath,
        Net::RudpInputCommandOp::kClickLoot,
        Net::RudpInputCommandOp::kMove,
        Net::RudpInputCommandOp::kAttack,
        Net::RudpInputCommandOp::kSpaceLoot};
constexpr std::array<Net::TcpPacketType, Core::kTcpSendFailurePacketTypeMetricCount>
    kTcpSendFailurePacketTypeMetrics{
        Net::TcpPacketType::kWelcome,
        Net::TcpPacketType::kClientListSnapshot,
        Net::TcpPacketType::kSessionReplaced,
        Net::TcpPacketType::kCreateRoomResponse,
        Net::TcpPacketType::kJoinRoomResponse,
        Net::TcpPacketType::kLeaveRoomResponse,
        Net::TcpPacketType::kRoomListSnapshot,
        Net::TcpPacketType::kReadyRoomResponse,
        Net::TcpPacketType::kBattleStart,
        Net::TcpPacketType::kMonsterSpawn,
        Net::TcpPacketType::kMonsterDeath,
        Net::TcpPacketType::kDropListSnapshot,
        Net::TcpPacketType::kLootResolved,
        Net::TcpPacketType::kLootRejected,
        Net::TcpPacketType::kInventorySnapshot,
        Net::TcpPacketType::kSettlementResult,
        Net::TcpPacketType::kMetaResponse,
        Net::TcpPacketType::kBattleStartRoster,
        Net::TcpPacketType::kMonsterHealthSnapshot,
        Net::TcpPacketType::kDropListSnapshotV2,
        Net::TcpPacketType::kLobbyReturnVisibility,
        Net::TcpPacketType::kRoomDetailState,
        Net::TcpPacketType::kUnreadyRoomResponse,
        Net::TcpPacketType::kHostStartBattleResponse,
        Net::TcpPacketType::kHostKickResponse,
        Net::TcpPacketType::kArenaGameplayStart,
        Net::TcpPacketType::kBattleFinalRanking,
        Net::TcpPacketType::kBattleLoadEntry,
        Net::TcpPacketType::kError};

size_t rudpReliableEventKindIndex(Net::RudpReliableEventKind kind) {
    switch (kind) {
    case Net::RudpReliableEventKind::kBattleStart:
        return 0;
    case Net::RudpReliableEventKind::kBattleStartRoster:
        return 1;
    case Net::RudpReliableEventKind::kMonsterDeath:
        return 2;
    case Net::RudpReliableEventKind::kLootResolved:
        return 3;
    case Net::RudpReliableEventKind::kMetaResponse:
        return 4;
    }

    return 0;
}

const char* rudpReliableEventKindLabel(Net::RudpReliableEventKind kind) {
    switch (kind) {
    case Net::RudpReliableEventKind::kBattleStart:
        return "battle_start";
    case Net::RudpReliableEventKind::kBattleStartRoster:
        return "battle_start_roster";
    case Net::RudpReliableEventKind::kMonsterDeath:
        return "monster_death";
    case Net::RudpReliableEventKind::kLootResolved:
        return "loot_resolved";
    case Net::RudpReliableEventKind::kMetaResponse:
        return "meta_response";
    }

    return "unknown";
}

size_t rudpInputCommandOpMetricIndex(Net::RudpInputCommandOp op) {
    switch (op) {
    case Net::RudpInputCommandOp::kReady:
        return 0;
    case Net::RudpInputCommandOp::kMonsterDeath:
        return 1;
    case Net::RudpInputCommandOp::kClickLoot:
        return 2;
    case Net::RudpInputCommandOp::kMove:
        return 3;
    case Net::RudpInputCommandOp::kAttack:
        return 4;
    case Net::RudpInputCommandOp::kSpaceLoot:
        return 5;
    }

    return 0;
}

const char* rudpInputCommandOpMetricLabel(Net::RudpInputCommandOp op) {
    switch (op) {
    case Net::RudpInputCommandOp::kReady:
        return "ready";
    case Net::RudpInputCommandOp::kMonsterDeath:
        return "monster_death";
    case Net::RudpInputCommandOp::kClickLoot:
        return "click_loot";
    case Net::RudpInputCommandOp::kMove:
        return "move";
    case Net::RudpInputCommandOp::kAttack:
        return "attack";
    case Net::RudpInputCommandOp::kSpaceLoot:
        return "space_loot";
    }

    return "unknown";
}

size_t tcpReadErrorErrnoMetricIndex(int errorNumber) {
    switch (errorNumber) {
    case ECONNRESET:
        return 0;
    case ECONNABORTED:
        return 1;
    case ETIMEDOUT:
        return 2;
    default:
        return 3;
    }
}

const char* tcpReadErrorErrnoMetricLabel(size_t index) {
    switch (index) {
    case 0:
        return "econnreset";
    case 1:
        return "econnaborted";
    case 2:
        return "etimedout";
    default:
        return "other";
    }
}

std::optional<size_t> tcpSendFailurePacketTypeMetricIndex(Net::TcpPacketType type) {
    for (size_t index = 0; index < kTcpSendFailurePacketTypeMetrics.size(); ++index) {
        if (kTcpSendFailurePacketTypeMetrics[index] == type) {
            return index;
        }
    }
    return std::nullopt;
}

const char* tcpSendFailurePacketTypeMetricLabel(Net::TcpPacketType type) {
    switch (type) {
    case Net::TcpPacketType::kWelcome:
        return "welcome";
    case Net::TcpPacketType::kClientListSnapshot:
        return "client_list_snapshot";
    case Net::TcpPacketType::kSessionReplaced:
        return "session_replaced";
    case Net::TcpPacketType::kCreateRoomResponse:
        return "create_room_response";
    case Net::TcpPacketType::kJoinRoomResponse:
        return "join_room_response";
    case Net::TcpPacketType::kLeaveRoomResponse:
        return "leave_room_response";
    case Net::TcpPacketType::kRoomListSnapshot:
        return "room_list_snapshot";
    case Net::TcpPacketType::kReadyRoomResponse:
        return "ready_room_response";
    case Net::TcpPacketType::kBattleStart:
        return "battle_start";
    case Net::TcpPacketType::kMonsterSpawn:
        return "monster_spawn";
    case Net::TcpPacketType::kMonsterDeath:
        return "monster_death";
    case Net::TcpPacketType::kDropListSnapshot:
        return "drop_list_snapshot";
    case Net::TcpPacketType::kLootResolved:
        return "loot_resolved";
    case Net::TcpPacketType::kLootRejected:
        return "loot_rejected";
    case Net::TcpPacketType::kInventorySnapshot:
        return "inventory_snapshot";
    case Net::TcpPacketType::kSettlementResult:
        return "settlement_result";
    case Net::TcpPacketType::kMetaResponse:
        return "meta_response";
    case Net::TcpPacketType::kBattleStartRoster:
        return "battle_start_roster";
    case Net::TcpPacketType::kMonsterHealthSnapshot:
        return "monster_health_snapshot";
    case Net::TcpPacketType::kDropListSnapshotV2:
        return "drop_list_snapshot_v2";
    case Net::TcpPacketType::kLobbyReturnVisibility:
        return "lobby_return_visibility";
    case Net::TcpPacketType::kRoomDetailState:
        return "room_detail_state";
    case Net::TcpPacketType::kUnreadyRoomResponse:
        return "unready_room_response";
    case Net::TcpPacketType::kHostStartBattleResponse:
        return "host_start_battle_response";
    case Net::TcpPacketType::kHostKickResponse:
        return "host_kick_response";
    case Net::TcpPacketType::kArenaGameplayStart:
        return "arena_gameplay_start";
    case Net::TcpPacketType::kBattleFinalRanking:
        return "battle_final_ranking";
    case Net::TcpPacketType::kBattleLoadEntry:
        return "battle_load_entry";
    case Net::TcpPacketType::kError:
        return "error";
    default:
        return "unknown";
    }
}

class RejectingMetaSessionClaimClient final : public Core::IMetaSessionClaimClient {
public:
    void claimGameSessionAsync(
        const Core::MetaSessionClaimRequest& /*request*/,
        ClaimCallback callback) override {
        callback(Core::MetaSessionClaimResult{});
    }

    void releaseGameSessionAsync(
        const Core::MetaSessionReleaseRequest& /*request*/) override {}

    void renewGameSessionAsync(
        const Core::MetaSessionRenewRequest& /*request*/) override {}
};

Core::IMetaSessionClaimClient& defaultMetaSessionClaimClient() {
    static RejectingMetaSessionClaimClient client;
    return client;
}

uint64_t currentUnixTimeMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

bool acceptedClaimHasReleasableMetaSlot(
    const Core::MetaSessionClaimResult& result) {
    return result.accepted && result.profile.accountId != 0;
}

bool acceptedClaimIsUsable(
    const Core::MetaSessionClaimResult& result,
    uint64_t nowUnixMs) {
    return result.accepted &&
        result.profile.accountId != 0 &&
        !result.profile.nickname.empty() &&
        result.reservationExpiresAtUnixMs > nowUnixMs;
}

Core::MetaSessionReleaseRequest releaseRequestForAcceptedClaimWithoutSession(
    const Core::MetaSessionClaimResult& result,
    uint64_t connectionId) {
    return Core::MetaSessionReleaseRequest{
        result.profile.accountId,
        0,
        connectionId};
}

bool markClientForDisconnect(std::vector<int>& disconnectedClients, int clientFd) {
    if (std::find(disconnectedClients.begin(), disconnectedClients.end(), clientFd) ==
        disconnectedClients.end()) {
        disconnectedClients.push_back(clientFd);
        return true;
    }
    return false;
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

bool buildRudpBattleStartRosterPayload(
    const Game::RoomCommandResult& result,
    Net::RudpBattleStartRosterPayload& outPayload) {
    outPayload = Net::RudpBattleStartRosterPayload{};
    if (result.room.roomId == 0) {
        return false;
    }

    outPayload.roomId = result.room.roomId;
    outPayload.playerSessionIds = result.playerSessionIds;
    return true;
}

std::string battleStartLogicalKey(const Net::RudpBattleStartPayload& payload) {
    return "BattleStart:" + std::to_string(payload.roomId) + ":" +
        std::to_string(payload.playerASessionId) + ":" +
        std::to_string(payload.playerBSessionId);
}

std::string battleStartRosterLogicalKey(const Net::RudpBattleStartRosterPayload& payload) {
    std::string key = "BattleStartRoster:" + std::to_string(payload.roomId);
    for (const uint64_t sessionId : payload.playerSessionIds) {
        key += ":" + std::to_string(sessionId);
    }
    return key;
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

std::vector<Net::TcpDropEntryV2> toTcpDropEntryV2s(const std::vector<Game::Drop>& drops) {
    std::vector<Net::TcpDropEntryV2> entries;
    entries.reserve(drops.size());
    for (const Game::Drop& drop : drops) {
        entries.push_back(
            Net::TcpDropEntryV2{
                drop.dropId,
                drop.itemId,
                drop.quantity,
                drop.position.x,
                drop.position.y});
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

std::vector<Net::BattleFinalRankingEntry> toTcpBattleFinalRankingEntries(
    const std::vector<Game::BattleFinalRankingRow>& rows) {
    std::vector<Net::BattleFinalRankingEntry> tcpRows;
    tcpRows.reserve(rows.size());
    for (const Game::BattleFinalRankingRow& row : rows) {
        tcpRows.push_back(
            Net::BattleFinalRankingEntry{
                row.rank,
                row.sessionId,
                row.nickname,
                row.totalAssetValue});
    }
    return tcpRows;
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
    case Game::RoomCommandError::kAlreadyStarted:
        return Net::TcpErrorCode::kAlreadyStarted;
    case Game::RoomCommandError::kNotHost:
        return Net::TcpErrorCode::kNotHost;
    case Game::RoomCommandError::kNotAllReady:
        return Net::TcpErrorCode::kNotAllReady;
    case Game::RoomCommandError::kNotEnoughPlayers:
        return Net::TcpErrorCode::kNotEnoughPlayers;
    case Game::RoomCommandError::kInvalidTarget:
        return Net::TcpErrorCode::kInvalidTarget;
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
    case Game::RoomEventType::kAttack:
        return Net::TcpPacketType::kReadyRoomRequest;
    case Game::RoomEventType::kSpaceLoot:
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
    result.scatterSeed = envelope.scatterSeed;
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
std::chrono::milliseconds defaultSessionTimeout() {
    return kDefaultSessionTimeout;
}

Server::MetaSessionClaimReleaseSink::MetaSessionClaimReleaseSink(
    IMetaSessionClaimClient* client)
    : client(client) {}

void Server::MetaSessionClaimReleaseSink::releaseAcceptedClaimWithoutServerSession(
    const MetaSessionClaimResult& result,
    uint64_t connectionId) const {
    if (client == nullptr || !acceptedClaimHasReleasableMetaSlot(result)) {
        return;
    }

    client->releaseGameSessionAsync(
        releaseRequestForAcceptedClaimWithoutSession(result, connectionId));
}

void Server::MetaSessionClaimReleaseSink::release(
    const MetaSessionReleaseRequest& request) const {
    if (client == nullptr || request.accountId == 0) {
        return;
    }

    client->releaseGameSessionAsync(request);
}

Server::Server(uint16_t port)
    : Server(port, defaultSessionTimeout(), &defaultMetaSessionClaimClient()) {}

Server::Server(uint16_t port, IMetaSessionClaimClient* metaSessionClaimClient)
    : Server(port, defaultSessionTimeout(), metaSessionClaimClient) {}

Server::Server(uint16_t port, std::chrono::milliseconds rudpPeerTimeout)
    : Server(port, rudpPeerTimeout, &defaultMetaSessionClaimClient()) {}

Server::Server(
    uint16_t port,
    std::chrono::milliseconds rudpPeerTimeout,
    IMetaSessionClaimClient* metaSessionClaimClient)
    : metaSessionClaimClient_(
          metaSessionClaimClient != nullptr
              ? metaSessionClaimClient
              : &defaultMetaSessionClaimClient()),
      sessionManager_(defaultSessionTimeout()),
      roomManager_(),
      roomEventDispatcher_(kServerRoomEventQueueCapacity, &roomEventMetrics_),
      rudpPeerRegistry_(rudpPeerTimeout),
      activeConnectionCount_(0),
      sessionCountSnapshot_(0),
      release1RuntimeTickCount_(0),
      release1MetricsTextfileInterval_(std::chrono::milliseconds(1000)),
      release1MetricsTextfileLastAttemptAt_(),
      release1MetricsTextfileEnabled_(false),
      release1MetricsTextfileHasAttempted_(false),
      rudpPeerCountSnapshot_(0),
      rudpDrainAttempted_(0),
      rudpDrainDelivered_(0),
      rudpDrainMalformed_(0),
      rudpDrainInvalidEndpoint_(0),
      rudpDrainAckOnly_(0),
      rudpDrainDuplicate_(0),
      rudpDrainTooOld_(0),
      rudpDrainSocketErrors_(0),
      rudpDrainStoppedByWouldBlock_(0),
      rudpDrainStoppedByMaxPackets_(0),
      rudpDrainStoppedBySocketError_(0),
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
      rudpBindingAttackAccepted_(0),
      rudpBindingLootClaimAccepted_(0),
      rudpMoveReceiveToApplyLatencySampleCount_(0),
      rudpMoveReceiveToApplyLatencyTotalUs_(0),
      rudpAttackReceiveToApplyLatencySampleCount_(0),
      rudpAttackReceiveToApplyLatencyTotalUs_(0),
      rudpLootClaimReceiveToApplyLatencySampleCount_(0),
      rudpLootClaimReceiveToApplyLatencyTotalUs_(0),
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
      tcpDisconnectTotal_(0),
      tcpDisconnectMarkedReadClosed_(0),
      tcpDisconnectMarkedReadError_(0),
      tcpDisconnectMarkedPacketReaderRejected_(0),
      tcpDisconnectMarkedInvalidPacket_(0),
      tcpDisconnectMarkedNetworkEvent_(0),
      tcpDisconnectMarkedSendFailure_(0),
      tcpDisconnectMarkedOutboundQueueFull_(0),
      tcpDisconnectMarkedEventLoopUpdateFailure_(0),
      tcpDisconnectMarkedMissingConnection_(0),
      tcpSendFailureUnknownPacketType_(0),
      tcpCreateRoomRequestReceived_(0),
      tcpJoinRoomRequestReceived_(0),
      tcpCreateRoomResponseSent_(0),
      tcpJoinRoomResponseSent_(0),
      tcpRoomListSnapshotDirect_(0),
      tcpRoomListSnapshotBroadcast_(0),
      tcpRoomListSnapshotBroadcastRecipients_(0),
      tcpRoomListSnapshotBytes_(0),
      pendingRoomListSnapshotBroadcast_(false),
      pendingRoomListSnapshotBroadcastDueAt_(),
      networkEventLoop_(nullptr),
      nextTcpFdGeneration_(1),
      linuxWakeupFd_(-1),
      running_(false),
      port_(port) {
    for (std::atomic<size_t>& bucket : rudpMoveReceiveToApplyLatencyBuckets_) {
        bucket.store(0, std::memory_order_relaxed);
    }
    for (std::atomic<size_t>& bucket : rudpAttackReceiveToApplyLatencyBuckets_) {
        bucket.store(0, std::memory_order_relaxed);
    }
    for (std::atomic<size_t>& bucket : rudpLootClaimReceiveToApplyLatencyBuckets_) {
        bucket.store(0, std::memory_order_relaxed);
    }
    for (std::atomic<size_t>& counter : rudpReliableEventTrackedByKind_) {
        counter.store(0, std::memory_order_relaxed);
    }
    for (std::atomic<size_t>& counter : rudpReliableEventExpiredByKind_) {
        counter.store(0, std::memory_order_relaxed);
    }
    for (std::atomic<size_t>& counter : rudpBindingInputNoRoomRejectedByOp_) {
        counter.store(0, std::memory_order_relaxed);
    }
    for (std::atomic<size_t>& counter : tcpDisconnectMarkedReadErrorByErrno_) {
        counter.store(0, std::memory_order_relaxed);
    }
    for (std::atomic<size_t>& counter : tcpSendFailureByPacketType_) {
        counter.store(0, std::memory_order_relaxed);
    }
    metaSessionClaimCompletionSink_ = std::make_shared<MetaSessionClaimCompletionSink>();
    metaSessionClaimReleaseSink_ =
        std::make_shared<MetaSessionClaimReleaseSink>(metaSessionClaimClient_);
    metaSessionClaimWorkerStopping_ = false;
    startMetaSessionClaimWorker();
}

Server::~Server() {
    requestStop();
    if (metaSessionClaimCompletionSink_ != nullptr) {
        metaSessionClaimCompletionSink_->accepting.store(false, std::memory_order_release);
    }
    stopMetaSessionClaimWorker();
    releaseQueuedAcceptedMetaSessionClaimsAfterShutdown();
}

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
    if (metaSessionClaimCompletionSink_ != nullptr) {
        metaSessionClaimCompletionSink_->linuxWakeupFd.store(
            wakeupFd,
            std::memory_order_release);
    }

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
    if (metaSessionClaimCompletionSink_ != nullptr) {
        metaSessionClaimCompletionSink_->linuxWakeupFd.store(
            -1,
            std::memory_order_release);
    }
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
            if (markClientForDisconnect(disconnectedClients, connection.clientFd())) {
                tcpDisconnectMarkedNetworkEvent_.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
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
            roomListChanged || roomChangedByDisconnect,
            now);
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
    stats.stoppedByWouldBlock =
        rudpDrainStoppedByWouldBlock_.load(std::memory_order_relaxed);
    stats.stoppedByMaxPackets =
        rudpDrainStoppedByMaxPackets_.load(std::memory_order_relaxed);
    stats.stoppedBySocketError =
        rudpDrainStoppedBySocketError_.load(std::memory_order_relaxed);
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
    for (size_t index = 0; index < stats.inputNoRoomRejectedByOp.size(); ++index) {
        stats.inputNoRoomRejectedByOp[index] =
            rudpBindingInputNoRoomRejectedByOp_[index].load(
                std::memory_order_relaxed);
    }
    stats.moveAccepted =
        rudpBindingMoveAccepted_.load(std::memory_order_relaxed);
    stats.attackAccepted =
        rudpBindingAttackAccepted_.load(std::memory_order_relaxed);
    stats.lootClaimAccepted =
        rudpBindingLootClaimAccepted_.load(std::memory_order_relaxed);
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

Release1RuntimeMetricsSnapshot Server::release1RuntimeMetricsSnapshot() const {
    const RudpServerDrainStats drainStats = rudpDrainStats();
    const RudpServerRetransmissionStats retransmissionStats =
        rudpRetransmissionStats();
    const RudpServerBindingStats bindingStats = rudpBindingStats();
    const RudpServerReliableEventStats reliableEventStats =
        rudpReliableEventStats();
    const RudpServerSnapshotStats snapshotStats = rudpSnapshotStats();
    std::array<size_t, kRelease1PrimaryLatencyBucketCount> moveLatencyBuckets{};
    for (size_t index = 0; index < moveLatencyBuckets.size(); ++index) {
        moveLatencyBuckets[index] =
            rudpMoveReceiveToApplyLatencyBuckets_[index].load(
                std::memory_order_relaxed);
    }
    std::array<size_t, kRelease1PrimaryLatencyBucketCount> attackLatencyBuckets{};
    for (size_t index = 0; index < attackLatencyBuckets.size(); ++index) {
        attackLatencyBuckets[index] =
            rudpAttackReceiveToApplyLatencyBuckets_[index].load(
                std::memory_order_relaxed);
    }
    std::array<size_t, kRelease1PrimaryLatencyBucketCount> lootClaimLatencyBuckets{};
    for (size_t index = 0; index < lootClaimLatencyBuckets.size(); ++index) {
        lootClaimLatencyBuckets[index] =
            rudpLootClaimReceiveToApplyLatencyBuckets_[index].load(
                std::memory_order_relaxed);
    }
    std::array<size_t, kRudpReliableEventKindCount> trackedByKind{};
    std::array<size_t, kRudpReliableEventKindCount> expiredByKind{};
    for (size_t index = 0; index < trackedByKind.size(); ++index) {
        trackedByKind[index] =
            rudpReliableEventTrackedByKind_[index].load(std::memory_order_relaxed);
        expiredByKind[index] =
            rudpReliableEventExpiredByKind_[index].load(std::memory_order_relaxed);
    }
    std::array<size_t, kRudpInputCommandOpMetricCount> noRoomRejectedByOp{};
    for (size_t index = 0; index < noRoomRejectedByOp.size(); ++index) {
        noRoomRejectedByOp[index] = bindingStats.inputNoRoomRejectedByOp[index];
    }
    std::array<size_t, kTcpSendFailurePacketTypeMetricCount>
        tcpSendFailureByPacketType{};
    for (size_t index = 0; index < tcpSendFailureByPacketType.size(); ++index) {
        tcpSendFailureByPacketType[index] =
            tcpSendFailureByPacketType_[index].load(std::memory_order_relaxed);
    }
    std::array<size_t, kTcpReadErrorErrnoMetricCount> tcpReadErrorByErrno{};
    for (size_t index = 0; index < tcpReadErrorByErrno.size(); ++index) {
        tcpReadErrorByErrno[index] =
            tcpDisconnectMarkedReadErrorByErrno_[index].load(std::memory_order_relaxed);
    }
    return Release1RuntimeMetricsSnapshot{
        release1RuntimeTickCount_.load(std::memory_order_relaxed),
        drainStats.attempted,
        drainStats.delivered,
        drainStats.malformed,
        drainStats.invalidEndpoint,
        drainStats.ackOnly,
        drainStats.duplicate,
        drainStats.tooOld,
        drainStats.socketErrors,
        drainStats.stoppedByWouldBlock,
        drainStats.stoppedByMaxPackets,
        drainStats.stoppedBySocketError,
        bindingStats.helloReceived,
        bindingStats.bound,
        bindingStats.refreshed,
        bindingStats.unknownSession,
        bindingStats.conflicts,
        bindingStats.invalidEndpoint,
        bindingStats.invalidPayload,
        bindingStats.ignoredNonHello,
        bindingStats.unboundInputRejected,
        bindingStats.unsupportedPacketIgnored,
        bindingStats.inputCandidates,
        bindingStats.inputDecoded,
        bindingStats.inputDecodeFailed,
        reliableEventStats.tracked,
        trackedByKind,
        rudpReliableEventPendingCountSnapshot_.load(std::memory_order_relaxed),
        retransmissionStats.expired,
        expiredByKind,
        retransmissionStats.due,
        retransmissionStats.resent,
        retransmissionStats.sendErrors,
        retransmissionStats.droppedPeers,
        bindingStats.inputSequenceAccepted,
        bindingStats.inputSequenceDuplicateRejected,
        bindingStats.inputSequenceStaleRejected,
        bindingStats.inputSequenceAmbiguousRejected,
        bindingStats.inputSequenceInvalidSessionRejected,
        bindingStats.inputNoRoomRejected,
        noRoomRejectedByOp,
        bindingStats.moveAccepted,
        bindingStats.moveDispatched,
        bindingStats.moveApplyRejected,
        bindingStats.moveInvalidReservedFlagsRejected,
        bindingStats.moveRateLimitedRejected,
        rudpMoveReceiveToApplyLatencySampleCount_.load(std::memory_order_relaxed),
        rudpMoveReceiveToApplyLatencyTotalUs_.load(std::memory_order_relaxed),
        moveLatencyBuckets,
        bindingStats.attackAccepted,
        rudpAttackReceiveToApplyLatencySampleCount_.load(std::memory_order_relaxed),
        rudpAttackReceiveToApplyLatencyTotalUs_.load(std::memory_order_relaxed),
        attackLatencyBuckets,
        bindingStats.lootClaimAccepted,
        rudpLootClaimReceiveToApplyLatencySampleCount_.load(
            std::memory_order_relaxed),
        rudpLootClaimReceiveToApplyLatencyTotalUs_.load(std::memory_order_relaxed),
        lootClaimLatencyBuckets,
        snapshotStats.sent,
        tcpDisconnectTotal_.load(std::memory_order_relaxed),
        tcpDisconnectMarkedReadClosed_.load(std::memory_order_relaxed),
        tcpDisconnectMarkedReadError_.load(std::memory_order_relaxed),
        tcpReadErrorByErrno,
        tcpDisconnectMarkedPacketReaderRejected_.load(std::memory_order_relaxed),
        tcpDisconnectMarkedInvalidPacket_.load(std::memory_order_relaxed),
        tcpDisconnectMarkedNetworkEvent_.load(std::memory_order_relaxed),
        tcpDisconnectMarkedSendFailure_.load(std::memory_order_relaxed),
        tcpDisconnectMarkedOutboundQueueFull_.load(std::memory_order_relaxed),
        tcpDisconnectMarkedEventLoopUpdateFailure_.load(std::memory_order_relaxed),
        tcpDisconnectMarkedMissingConnection_.load(std::memory_order_relaxed),
        tcpSendFailureByPacketType,
        tcpSendFailureUnknownPacketType_.load(std::memory_order_relaxed),
        tcpCreateRoomRequestReceived_.load(std::memory_order_relaxed),
        tcpJoinRoomRequestReceived_.load(std::memory_order_relaxed),
        tcpCreateRoomResponseSent_.load(std::memory_order_relaxed),
        tcpJoinRoomResponseSent_.load(std::memory_order_relaxed),
        tcpRoomListSnapshotDirect_.load(std::memory_order_relaxed),
        tcpRoomListSnapshotBroadcast_.load(std::memory_order_relaxed),
        tcpRoomListSnapshotBroadcastRecipients_.load(std::memory_order_relaxed),
        tcpRoomListSnapshotBytes_.load(std::memory_order_relaxed),
        activeConnectionCount(),
        sessionCount()};
}

std::string Server::renderRelease1RuntimeMetricsTextfile() const {
    const Release1RuntimeMetricsSnapshot metrics = release1RuntimeMetricsSnapshot();
    std::vector<Util::PrometheusMetricSample> samples{
        {"lol_server_runtime_tick_total",
         static_cast<double>(metrics.runtimeTickCount),
         {}},
        {"lol_rudp_drain_delivered_total",
         static_cast<double>(metrics.rudpDrainDeliveredCount),
         {}},
        {"lol_rudp_drain_attempted_total",
         static_cast<double>(metrics.rudpDrainAttemptedCount),
         {}},
        {"lol_rudp_drain_malformed_total",
         static_cast<double>(metrics.rudpDrainMalformedCount),
         {}},
        {"lol_rudp_drain_invalid_endpoint_total",
         static_cast<double>(metrics.rudpDrainInvalidEndpointCount),
         {}},
        {"lol_rudp_drain_ack_only_total",
         static_cast<double>(metrics.rudpDrainAckOnlyCount),
         {}},
        {"lol_rudp_drain_duplicate_total",
         static_cast<double>(metrics.rudpDrainDuplicateCount),
         {}},
        {"lol_rudp_drain_too_old_total",
         static_cast<double>(metrics.rudpDrainTooOldCount),
         {}},
        {"lol_rudp_drain_socket_errors_total",
         static_cast<double>(metrics.rudpDrainSocketErrorCount),
         {}},
        {"lol_rudp_drain_stopped_by_would_block_total",
         static_cast<double>(metrics.rudpDrainStoppedByWouldBlockCount),
         {}},
        {"lol_rudp_drain_stopped_by_max_packets_total",
         static_cast<double>(metrics.rudpDrainStoppedByMaxPacketsCount),
         {}},
        {"lol_rudp_drain_stopped_by_socket_error_total",
         static_cast<double>(metrics.rudpDrainStoppedBySocketErrorCount),
         {}},
        {"lol_rudp_binding_hello_received_total",
         static_cast<double>(metrics.rudpBindingHelloReceivedCount),
         {}},
        {"lol_rudp_binding_bound_total",
         static_cast<double>(metrics.rudpBindingBoundCount),
         {}},
        {"lol_rudp_binding_refreshed_total",
         static_cast<double>(metrics.rudpBindingRefreshedCount),
         {}},
        {"lol_rudp_binding_unknown_session_total",
         static_cast<double>(metrics.rudpBindingUnknownSessionCount),
         {}},
        {"lol_rudp_binding_conflicts_total",
         static_cast<double>(metrics.rudpBindingConflictsCount),
         {}},
        {"lol_rudp_binding_invalid_endpoint_total",
         static_cast<double>(metrics.rudpBindingInvalidEndpointCount),
         {}},
        {"lol_rudp_binding_invalid_payload_total",
         static_cast<double>(metrics.rudpBindingInvalidPayloadCount),
         {}},
        {"lol_rudp_binding_ignored_non_hello_total",
         static_cast<double>(metrics.rudpBindingIgnoredNonHelloCount),
         {}},
        {"lol_rudp_binding_unbound_input_rejected_total",
         static_cast<double>(metrics.rudpBindingUnboundInputRejectedCount),
         {}},
        {"lol_rudp_binding_unsupported_packet_ignored_total",
         static_cast<double>(metrics.rudpBindingUnsupportedPacketIgnoredCount),
         {}},
        {"lol_rudp_binding_input_candidates_total",
         static_cast<double>(metrics.rudpBindingInputCandidatesCount),
         {}},
        {"lol_rudp_binding_input_decoded_total",
         static_cast<double>(metrics.rudpBindingInputDecodedCount),
         {}},
        {"lol_rudp_binding_input_decode_failed_total",
         static_cast<double>(metrics.rudpBindingInputDecodeFailedCount),
         {}},
        {"lol_rudp_reliable_event_tracked_total",
         static_cast<double>(metrics.rudpReliableEventTrackedCount),
         {}},
        {"lol_rudp_reliable_event_pending",
         static_cast<double>(metrics.rudpReliableEventPendingCount),
         {}},
        {"lol_rudp_retransmission_expired_total",
         static_cast<double>(metrics.rudpRetransmissionExpiredCount),
         {}},
        {"lol_rudp_retransmission_due_total",
         static_cast<double>(metrics.rudpRetransmissionDueCount),
         {}},
        {"lol_rudp_retransmission_resent_total",
         static_cast<double>(metrics.rudpRetransmissionResentCount),
         {}},
        {"lol_rudp_retransmission_send_errors_total",
         static_cast<double>(metrics.rudpRetransmissionSendErrorCount),
         {}},
        {"lol_rudp_retransmission_dropped_peers_total",
         static_cast<double>(metrics.rudpRetransmissionDroppedPeerCount),
         {}},
        {"lol_rudp_input_sequence_accepted_total",
         static_cast<double>(metrics.rudpInputSequenceAcceptedCount),
         {}},
        {"lol_rudp_input_sequence_duplicate_rejected_total",
         static_cast<double>(metrics.rudpInputSequenceDuplicateRejectedCount),
         {}},
        {"lol_rudp_input_sequence_stale_rejected_total",
         static_cast<double>(metrics.rudpInputSequenceStaleRejectedCount),
         {}},
        {"lol_rudp_input_sequence_ambiguous_rejected_total",
         static_cast<double>(metrics.rudpInputSequenceAmbiguousRejectedCount),
         {}},
        {"lol_rudp_input_sequence_invalid_session_rejected_total",
         static_cast<double>(
             metrics.rudpInputSequenceInvalidSessionRejectedCount),
         {}},
        {"lol_rudp_binding_input_no_room_rejected_total",
         static_cast<double>(metrics.rudpBindingInputNoRoomRejectedCount),
         {}},
        {"lol_rudp_move_accepted_total",
         static_cast<double>(metrics.rudpMoveAcceptedCount),
         {}},
        {"lol_rudp_move_dispatched_total",
         static_cast<double>(metrics.rudpMoveDispatchedCount),
         {}},
        {"lol_rudp_move_apply_rejected_total",
         static_cast<double>(metrics.rudpMoveApplyRejectedCount),
         {}},
        {"lol_rudp_move_invalid_reserved_flags_rejected_total",
         static_cast<double>(
             metrics.rudpMoveInvalidReservedFlagsRejectedCount),
         {}},
        {"lol_rudp_move_rate_limited_rejected_total",
         static_cast<double>(metrics.rudpMoveRateLimitedRejectedCount),
         {}},
        {"lol_rudp_attack_accepted_total",
         static_cast<double>(metrics.rudpAttackAcceptedCount),
         {}},
        {"lol_rudp_loot_claim_accepted_total",
         static_cast<double>(metrics.rudpLootClaimAcceptedCount),
         {}}};
    for (size_t index = 0; index < kRudpInputCommandOpMetrics.size(); ++index) {
        const Net::RudpInputCommandOp op = kRudpInputCommandOpMetrics[index];
        samples.push_back(
            {"lol_rudp_binding_input_no_room_rejected_by_op_total",
             static_cast<double>(
                 metrics.rudpBindingInputNoRoomRejectedByOpCounts[index]),
             {{"op", rudpInputCommandOpMetricLabel(op)}}});
    }
    for (size_t index = 0; index < kRudpReliableEventKinds.size(); ++index) {
        const Net::RudpReliableEventKind kind = kRudpReliableEventKinds[index];
        samples.push_back(
            {"lol_rudp_reliable_event_tracked_by_kind_total",
             static_cast<double>(metrics.rudpReliableEventTrackedByKindCounts[index]),
             {{"kind", rudpReliableEventKindLabel(kind)}}});
        samples.push_back(
            {"lol_rudp_retransmission_expired_by_kind_total",
             static_cast<double>(metrics.rudpReliableEventExpiredByKindCounts[index]),
             {{"kind", rudpReliableEventKindLabel(kind)}}});
    }
    for (size_t index = 0; index < kRelease1PrimaryLatencyBucketCount; ++index) {
        samples.push_back(
            {"lol_rudp_move_receive_to_apply_latency_ms_bucket",
             static_cast<double>(
                 metrics.rudpMoveReceiveToApplyLatencyBucketCounts[index]),
             {{"le", std::to_string(kRelease1PrimaryLatencyBucketMs[index])}}});
    }
    samples.push_back(
        {"lol_rudp_move_receive_to_apply_latency_ms_count",
         static_cast<double>(
             metrics.rudpMoveReceiveToApplyLatencySampleCount),
         {}});
    samples.push_back(
        {"lol_rudp_move_receive_to_apply_latency_ms_sum",
         static_cast<double>(
             metrics.rudpMoveReceiveToApplyLatencyTotalUs) / 1000.0,
         {}});
    for (size_t index = 0; index < kRelease1PrimaryLatencyBucketCount; ++index) {
        samples.push_back(
            {"lol_rudp_attack_receive_to_apply_latency_ms_bucket",
             static_cast<double>(
                 metrics.rudpAttackReceiveToApplyLatencyBucketCounts[index]),
             {{"le", std::to_string(kRelease1PrimaryLatencyBucketMs[index])}}});
    }
    samples.push_back(
        {"lol_rudp_attack_receive_to_apply_latency_ms_count",
         static_cast<double>(
             metrics.rudpAttackReceiveToApplyLatencySampleCount),
         {}});
    samples.push_back(
        {"lol_rudp_attack_receive_to_apply_latency_ms_sum",
         static_cast<double>(
             metrics.rudpAttackReceiveToApplyLatencyTotalUs) / 1000.0,
         {}});
    for (size_t index = 0; index < kRelease1PrimaryLatencyBucketCount; ++index) {
        samples.push_back(
            {"lol_rudp_loot_claim_receive_to_apply_latency_ms_bucket",
             static_cast<double>(
                 metrics.rudpLootClaimReceiveToApplyLatencyBucketCounts[index]),
             {{"le", std::to_string(kRelease1PrimaryLatencyBucketMs[index])}}});
    }
    samples.push_back(
        {"lol_rudp_loot_claim_receive_to_apply_latency_ms_count",
         static_cast<double>(
             metrics.rudpLootClaimReceiveToApplyLatencySampleCount),
         {}});
    samples.push_back(
        {"lol_rudp_loot_claim_receive_to_apply_latency_ms_sum",
         static_cast<double>(
             metrics.rudpLootClaimReceiveToApplyLatencyTotalUs) / 1000.0,
         {}});
    samples.push_back(
        {"lol_rudp_state_snapshot_sent_total",
         static_cast<double>(metrics.rudpStateSnapshotSentCount),
         {}});
    samples.push_back(
        {"lol_tcp_disconnect_total",
         static_cast<double>(metrics.tcpDisconnectTotalCount),
         {}});
    samples.push_back(
        {"lol_tcp_disconnect_marked_read_closed_total",
         static_cast<double>(metrics.tcpDisconnectMarkedReadClosedCount),
         {}});
    samples.push_back(
        {"lol_tcp_disconnect_marked_read_error_total",
         static_cast<double>(metrics.tcpDisconnectMarkedReadErrorCount),
         {}});
    for (size_t index = 0; index < metrics.tcpDisconnectMarkedReadErrorByErrnoCounts.size();
         ++index) {
        samples.push_back(
            {"lol_tcp_disconnect_marked_read_error_by_errno_total",
             static_cast<double>(
                 metrics.tcpDisconnectMarkedReadErrorByErrnoCounts[index]),
             {{"errno", tcpReadErrorErrnoMetricLabel(index)}}});
    }
    samples.push_back(
        {"lol_tcp_disconnect_marked_packet_reader_rejected_total",
         static_cast<double>(metrics.tcpDisconnectMarkedPacketReaderRejectedCount),
         {}});
    samples.push_back(
        {"lol_tcp_disconnect_marked_invalid_packet_total",
         static_cast<double>(metrics.tcpDisconnectMarkedInvalidPacketCount),
         {}});
    samples.push_back(
        {"lol_tcp_disconnect_marked_network_event_total",
         static_cast<double>(metrics.tcpDisconnectMarkedNetworkEventCount),
         {}});
    samples.push_back(
        {"lol_tcp_disconnect_marked_send_failure_total",
         static_cast<double>(metrics.tcpDisconnectMarkedSendFailureCount),
         {}});
    samples.push_back(
        {"lol_tcp_disconnect_marked_outbound_queue_full_total",
         static_cast<double>(metrics.tcpDisconnectMarkedOutboundQueueFullCount),
         {}});
    samples.push_back(
        {"lol_tcp_disconnect_marked_event_loop_update_failure_total",
         static_cast<double>(metrics.tcpDisconnectMarkedEventLoopUpdateFailureCount),
         {}});
    samples.push_back(
        {"lol_tcp_disconnect_marked_missing_connection_total",
         static_cast<double>(metrics.tcpDisconnectMarkedMissingConnectionCount),
         {}});
    for (size_t index = 0; index < kTcpSendFailurePacketTypeMetrics.size(); ++index) {
        samples.push_back(
            {"lol_tcp_send_failure_by_packet_type_total",
             static_cast<double>(metrics.tcpSendFailureByPacketTypeCounts[index]),
             {{"packet",
               tcpSendFailurePacketTypeMetricLabel(
                   kTcpSendFailurePacketTypeMetrics[index])}}});
    }
    samples.push_back(
        {"lol_tcp_send_failure_unknown_packet_type_total",
         static_cast<double>(metrics.tcpSendFailureUnknownPacketTypeCount),
         {}});
    samples.push_back(
        {"lol_tcp_room_command_request_received_total",
         static_cast<double>(metrics.tcpCreateRoomRequestReceivedCount),
         {{"command", "create_room"}}});
    samples.push_back(
        {"lol_tcp_room_command_request_received_total",
         static_cast<double>(metrics.tcpJoinRoomRequestReceivedCount),
         {{"command", "join_room"}}});
    samples.push_back(
        {"lol_tcp_room_command_response_sent_total",
         static_cast<double>(metrics.tcpCreateRoomResponseSentCount),
         {{"command", "create_room"}}});
    samples.push_back(
        {"lol_tcp_room_command_response_sent_total",
         static_cast<double>(metrics.tcpJoinRoomResponseSentCount),
         {{"command", "join_room"}}});
    samples.push_back(
        {"lol_tcp_room_list_snapshot_direct_total",
         static_cast<double>(metrics.tcpRoomListSnapshotDirectCount),
         {}});
    samples.push_back(
        {"lol_tcp_room_list_snapshot_broadcast_total",
         static_cast<double>(metrics.tcpRoomListSnapshotBroadcastCount),
         {}});
    samples.push_back(
        {"lol_tcp_room_list_snapshot_broadcast_recipients_total",
         static_cast<double>(
             metrics.tcpRoomListSnapshotBroadcastRecipientCount),
         {}});
    samples.push_back(
        {"lol_tcp_room_list_snapshot_bytes_total",
         static_cast<double>(metrics.tcpRoomListSnapshotBytesCount),
         {}});
    samples.push_back(
        {"lol_active_connections",
         static_cast<double>(metrics.activeConnectionCount),
         {}});
    samples.push_back(
        {"lol_active_sessions",
         static_cast<double>(metrics.activeSessionCount),
         {}});
    return Util::renderPrometheusTextfile(samples);
}

bool Server::configureRelease1MetricsTextfile(
    const std::string& targetPath,
    std::chrono::milliseconds interval) {
    if (targetPath.empty() || interval.count() <= 0) {
        return false;
    }

    release1MetricsTextfilePath_ = targetPath;
    release1MetricsTextfileInterval_ = interval;
    release1MetricsTextfileLastAttemptAt_ = Util::TimePoint{};
    release1MetricsTextfileHasAttempted_ = false;
    release1MetricsTextfileEnabled_ = true;
    return true;
}

bool Server::writeRelease1RuntimeMetricsTextfile(
    std::string* errorMessage) const {
    if (!release1MetricsTextfileEnabled_) {
        if (errorMessage != nullptr) {
            *errorMessage = "release1 metrics textfile is not configured";
        }
        return false;
    }

    return Util::writePrometheusTextfileAtomically(
        release1MetricsTextfilePath_,
        renderRelease1RuntimeMetricsTextfile(),
        errorMessage);
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
    processPendingMetaSessionClaims(now);
    processMetaSessionLivenessReports(now);
    processRudpPeerLifecycle(now);
    processRudpRetransmissions(now);
    processRudpReliableEventRetransmissions(now);
    processRudpMovementSnapshots(now);
    std::vector<int> disconnectedClients;
    bool roomListChanged = false;
    const std::vector<Game::BattleFinalRankingResult> completedRankings =
        roomManager_.processBattleDropTimeouts(
            currentUnixTimeMs(),
            [this](uint64_t sessionId) { return nicknameForSession(sessionId); });
    for (const Game::BattleFinalRankingResult& ranking : completedRankings) {
        completeBattleResult(ranking, disconnectedClients, roomListChanged);
    }

    bool roomChangedByDisconnect = false;
    for (int clientFd : disconnectedClients) {
        roomChangedByDisconnect = disconnectClient(clientFd) || roomChangedByDisconnect;
    }
    if (!disconnectedClients.empty() || roomListChanged || roomChangedByDisconnect) {
        broadcastStateSnapshots(
            !disconnectedClients.empty(),
            roomListChanged || roomChangedByDisconnect,
            now);
    }
    sessionManager_.tick(now);
    sessionCountSnapshot_.store(sessionManager_.size(), std::memory_order_relaxed);
    release1RuntimeTickCount_.fetch_add(1, std::memory_order_relaxed);
    processPendingRoomListSnapshotBroadcast(now);
    processRelease1MetricsTextfile(now);
}

void Server::processRelease1MetricsTextfile(Util::TimePoint now) {
    if (!release1MetricsTextfileEnabled_) {
        return;
    }
    if (release1MetricsTextfileHasAttempted_ &&
        now - release1MetricsTextfileLastAttemptAt_ <
            release1MetricsTextfileInterval_) {
        return;
    }

    release1MetricsTextfileHasAttempted_ = true;
    release1MetricsTextfileLastAttemptAt_ = now;

    std::string errorMessage;
    // ponytail: timer-path write is one tiny textfile; move to a side thread
    // if it shows in tick budget.
    writeRelease1RuntimeMetricsTextfile(&errorMessage);
}

void Server::enqueueMetaSessionClaim(
    int clientFd,
    uint64_t connectionId,
    const std::string& token) {
    {
        std::lock_guard<std::mutex> lock(metaSessionClaimWorkerMutex_);
        if (metaSessionClaimWorkerStopping_) {
            return;
        }
        pendingMetaSessionClaimInvocations_.push_back(
            PendingMetaSessionClaimInvocation{
                clientFd,
                connectionId,
                token});
    }
    metaSessionClaimWorkerCondition_.notify_one();
}

void Server::startMetaSessionClaimWorker() {
    if (metaSessionClaimWorker_.joinable()) {
        return;
    }
    metaSessionClaimWorker_ =
        std::thread([this]() { runMetaSessionClaimWorker(); });
}

void Server::stopMetaSessionClaimWorker() {
    {
        std::lock_guard<std::mutex> lock(metaSessionClaimWorkerMutex_);
        metaSessionClaimWorkerStopping_ = true;
        pendingMetaSessionClaimInvocations_.clear();
    }
    metaSessionClaimWorkerCondition_.notify_all();
    if (metaSessionClaimWorker_.joinable()) {
        metaSessionClaimWorker_.join();
    }
}

void Server::runMetaSessionClaimWorker() {
    while (true) {
        PendingMetaSessionClaimInvocation invocation;
        {
            std::unique_lock<std::mutex> lock(metaSessionClaimWorkerMutex_);
            metaSessionClaimWorkerCondition_.wait(
                lock,
                [this]() {
                    return metaSessionClaimWorkerStopping_ ||
                        !pendingMetaSessionClaimInvocations_.empty();
                });
            if (metaSessionClaimWorkerStopping_) {
                return;
            }

            invocation = std::move(pendingMetaSessionClaimInvocations_.front());
            pendingMetaSessionClaimInvocations_.pop_front();
        }

        const MetaSessionClaimRequest request{
            invocation.connectionId,
            invocation.gameSessionToken};
        const std::weak_ptr<MetaSessionClaimCompletionSink> completionSink =
            metaSessionClaimCompletionSink_;
        const std::shared_ptr<MetaSessionClaimReleaseSink> releaseSink =
            metaSessionClaimReleaseSink_;
        metaSessionClaimClient_->claimGameSessionAsync(
            request,
            [completionSink,
             releaseSink,
             clientFd = invocation.clientFd,
             connectionId = invocation.connectionId,
             token = invocation.gameSessionToken](MetaSessionClaimResult result) {
                const std::shared_ptr<MetaSessionClaimCompletionSink> sink =
                    completionSink.lock();
                if (sink == nullptr ||
                    !sink->accepting.load(std::memory_order_acquire)) {
                    if (releaseSink != nullptr) {
                        releaseSink->releaseAcceptedClaimWithoutServerSession(
                            result,
                            connectionId);
                    }
                    return;
                }

                bool releaseWithoutServer = false;
                {
                    std::lock_guard<std::mutex> lock(sink->mutex);
                    if (!sink->accepting.load(std::memory_order_acquire)) {
                        releaseWithoutServer = true;
                    } else {
                        sink->completions.push_back(
                            PendingMetaSessionClaimCompletion{
                                clientFd,
                                connectionId,
                                token,
                                std::move(result)});
                    }
                }

                if (releaseWithoutServer) {
                    if (releaseSink != nullptr) {
                        releaseSink->releaseAcceptedClaimWithoutServerSession(
                            result,
                            connectionId);
                    }
                    return;
                }
#if defined(__linux__)
                writeCounterFd(sink->linuxWakeupFd.load(std::memory_order_acquire));
#endif
            });
    }
}

void Server::acceptNewClients(Util::TimePoint now) {
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
        auto connection = std::make_unique<ClientConnection>(
            clientFd,
            0,
            remoteKey,
            now);
        connection->setFdGeneration(allocateTcpFdGeneration());
        ClientConnection& storedConnection = *connection;
        GameSessionAuthState authState;
        authState.phase = GameSessionAuthPhase::kUnauthenticated;
        authState.connectionId = storedConnection.fdGeneration();
        connections_.emplace(clientFd, std::move(connection));
        gameSessionAuthByClientFd_.emplace(clientFd, std::move(authState));
        activeConnectionCount_.store(connections_.size(), std::memory_order_relaxed);
        sessionCountSnapshot_.store(sessionManager_.size(), std::memory_order_relaxed);

        if (!registerTcpClientWithEventLoop(storedConnection)) {
            if (markClientForDisconnect(disconnectedClients, clientFd)) {
                tcpDisconnectMarkedEventLoopUpdateFailure_.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
            continue;
        }
    }

    for (int clientFd : disconnectedClients) {
        disconnectClient(clientFd);
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
            roomListChanged || roomChangedByDisconnect,
            now);
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
            if (markClientForDisconnect(disconnectedClients, connection.clientFd())) {
                if (status == Net::ReceiveStatus::kClosed) {
                    tcpDisconnectMarkedReadClosed_.fetch_add(
                        1,
                        std::memory_order_relaxed);
                } else {
                    tcpDisconnectMarkedReadError_.fetch_add(
                        1,
                        std::memory_order_relaxed);
                    tcpDisconnectMarkedReadErrorByErrno_[
                        tcpReadErrorErrnoMetricIndex(errno)]
                        .fetch_add(1, std::memory_order_relaxed);
                }
            }
            return;
        }

        connection.updateLastHeard(now);
        auto session = sessionManager_.find(connection.remoteKey());
        if (session) {
            session->updateLastHeard(now);
        }

        if (!connection.packetReader().appendBytes(buffer.data(), received)) {
            if (markClientForDisconnect(disconnectedClients, connection.clientFd())) {
                tcpDisconnectMarkedPacketReaderRejected_.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
            return;
        }

        while (true) {
            Net::TcpPacketReadResult readResult =
                connection.packetReader().tryReadPacket(framedPacket);
            if (readResult == Net::TcpPacketReadResult::kNeedMoreData) {
                break;
            }

            if (readResult == Net::TcpPacketReadResult::kInvalidPacket) {
                if (markClientForDisconnect(disconnectedClients, connection.clientFd())) {
                    tcpDisconnectMarkedInvalidPacket_.fetch_add(
                        1,
                        std::memory_order_relaxed);
                }
                return;
            }

            if (!handleTcpPacket(
                    connection,
                    framedPacket,
                    now,
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
    std::vector<int> disconnectedClients;
    bool roomListChanged = false;
    processRudpDeliveries(summary.deliveries, now, disconnectedClients, roomListChanged);

    bool roomChangedByDisconnect = false;
    for (int clientFd : disconnectedClients) {
        roomChangedByDisconnect = disconnectClient(clientFd) || roomChangedByDisconnect;
    }

    if (!disconnectedClients.empty() || roomListChanged || roomChangedByDisconnect) {
        broadcastStateSnapshots(
            !disconnectedClients.empty(),
            roomListChanged || roomChangedByDisconnect,
            now);
    }
}

void Server::processRudpDeliveries(
    const std::vector<Net::RudpPacketDelivery>& deliveries,
    Util::TimePoint now,
    std::vector<int>& disconnectedClients,
    bool& outRoomListChanged) {
    for (const Net::RudpPacketDelivery& delivery : deliveries) {
        if (delivery.header.packetType != static_cast<uint16_t>(Net::RudpPacketType::kHello)) {
            rudpBindingIgnoredNonHello_.fetch_add(1, std::memory_order_relaxed);
            processRudpAdapterGate(
                delivery,
                now,
                disconnectedClients,
                outRoomListChanged);
            continue;
        }
        processRudpHelloDelivery(delivery, now);
    }
}

void Server::processRudpAdapterGate(
    const Net::RudpPacketDelivery& delivery,
    Util::TimePoint now,
    std::vector<int>& disconnectedClients,
    bool& outRoomListChanged) {
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
        rudpBindingInputNoRoomRejectedByOp_[rudpInputCommandOpMetricIndex(input.op)]
            .fetch_add(1, std::memory_order_relaxed);
        return;
    }

    auto session = sessionManager_.findBySessionId(*sessionId);
    if (session) {
        session->updateLastHeard(now);
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

    if (input.op == Net::RudpInputCommandOp::kAttack) {
        rudpBindingAttackAccepted_.fetch_add(1, std::memory_order_relaxed);
    }
    if (input.op == Net::RudpInputCommandOp::kClickLoot ||
        input.op == Net::RudpInputCommandOp::kSpaceLoot) {
        rudpBindingLootClaimAccepted_.fetch_add(1, std::memory_order_relaxed);
    }
    dispatchRudpRoomEvent(
        *translateResult.event,
        now,
        disconnectedClients,
        outRoomListChanged);
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

        recordRelease1RudpMoveReceiveToApplyLatency(now, Util::now());
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
    recordRelease1RudpMoveReceiveToApplyLatency(now, Util::now());
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
            rudpReliableEventTrackedByKind_[rudpReliableEventKindIndex(descriptor.kind)]
                .fetch_add(1, std::memory_order_relaxed);
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
        rudpReliableEventTrackedByKind_[rudpReliableEventKindIndex(descriptor.kind)]
            .fetch_add(1, std::memory_order_relaxed);
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

    const std::optional<Net::UdpEndpoint> endpoint =
        findBoundRudpEndpointForSession(sessionId);
    if (!endpoint.has_value()) {
        return RudpServerReliableEventTrackResult::kNoBoundEndpoint;
    }

    udpSocket_.sendTo(packetBytes.data(), packetBytes.size(), *endpoint);
    return RudpServerReliableEventTrackResult::kTracked;
}

bool Server::sendPendingRudpReliableEventsForSession(uint64_t sessionId) {
    const std::optional<Net::UdpEndpoint> endpoint =
        findBoundRudpEndpointForSession(sessionId);
    if (!endpoint.has_value()) {
        return false;
    }

    const auto queueIt = rudpReliableEventQueues_.find(sessionId);
    if (queueIt == rudpReliableEventQueues_.end()) {
        return false;
    }

    bool sentAny = false;
    for (uint32_t sequence : queueIt->second.pendingSequences()) {
        const std::vector<uint8_t>* packetBytes =
            queueIt->second.packetBytes(sequence);
        if (packetBytes == nullptr || packetBytes->empty()) {
            continue;
        }
        if (udpSocket_.sendTo(packetBytes->data(), packetBytes->size(), *endpoint)) {
            sentAny = true;
        }
    }
    return sentAny;
}

void Server::enqueueRudpBattleStartEvent(
    const Game::RoomCommandResult& result,
    Util::TimePoint now) {
    if (result.playerSessionIds.size() > 2u) {
        Net::RudpBattleStartRosterPayload rosterPayload;
        if (!buildRudpBattleStartRosterPayload(result, rosterPayload)) {
            return;
        }

        std::vector<uint8_t> rosterPayloadBytes;
        if (!Net::serializeRudpBattleStartRosterPayload(rosterPayload, rosterPayloadBytes)) {
            return;
        }

        const Net::RudpReliableEventDescriptor descriptor{
            Net::RudpReliableEventKind::kBattleStartRoster,
            battleStartRosterLogicalKey(rosterPayload),
            static_cast<uint16_t>(Net::RudpPacketType::kBattleStartRoster),
            static_cast<uint8_t>(Net::RudpChannelId::kEvent)};

        for (uint64_t sessionId : result.playerSessionIds) {
            const uint32_t sequence = nextRudpOutboundSequenceForSession(sessionId);
            std::vector<uint8_t> packetBytes;
            if (!buildRudpReliableEventPacket(
                    descriptor,
                    sequence,
                    rosterPayloadBytes,
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
        return;
    }

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
        sendPendingRudpReliableEventsForSession(hello.sessionId);
        break;
    case Net::RudpSessionBindResult::kRefreshed:
        rudpBindingRefreshed_.fetch_add(1, std::memory_order_relaxed);
        session->updateLastHeard(now);
        sendPendingRudpReliableEventsForSession(hello.sessionId);
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
            bool hasKind = false;
            size_t kindIndex = 0;
            if (const Net::RudpReliableEventPendingEntry* entry =
                    queue.pendingEntry(sequence)) {
                kindIndex = rudpReliableEventKindIndex(entry->descriptor.kind);
                hasKind = true;
            }
            if (queue.remove(sequence)) {
                rudpRetransmissionExpired_.fetch_add(1, std::memory_order_relaxed);
                if (hasKind) {
                    rudpReliableEventExpiredByKind_[kindIndex].fetch_add(
                        1,
                        std::memory_order_relaxed);
                }
            }
        }

        const std::optional<Net::UdpEndpoint> endpoint =
            findBoundRudpEndpointForSession(sessionId);
        const std::vector<uint32_t> dueSequences =
            queue.dueForRetransmission(now);
        for (uint32_t sequence : dueSequences) {
            rudpRetransmissionDue_.fetch_add(1, std::memory_order_relaxed);
            const std::vector<uint8_t>* packetBytes = queue.packetBytes(sequence);
            if (!endpoint.has_value()) {
                continue;
            }
            if (packetBytes == nullptr || packetBytes->empty()) {
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
    if (summary.stoppedByWouldBlock) {
        rudpDrainStoppedByWouldBlock_.fetch_add(1, std::memory_order_relaxed);
    }
    if (summary.stoppedByMaxPackets) {
        rudpDrainStoppedByMaxPackets_.fetch_add(1, std::memory_order_relaxed);
    }
    if (summary.stoppedBySocketError) {
        rudpDrainStoppedBySocketError_.fetch_add(1, std::memory_order_relaxed);
    }
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

void Server::processPendingMetaSessionClaims(Util::TimePoint now) {
    std::vector<PendingMetaSessionClaimCompletion> completions;
    if (metaSessionClaimCompletionSink_ != nullptr) {
        std::lock_guard<std::mutex> lock(metaSessionClaimCompletionSink_->mutex);
        completions.swap(metaSessionClaimCompletionSink_->completions);
    }
    if (completions.empty()) {
        return;
    }

    std::vector<int> disconnectedClients;
    bool authenticatedMembershipChanged = false;
    const uint64_t nowUnixMs = currentUnixTimeMs();

    for (const PendingMetaSessionClaimCompletion& completion : completions) {
        auto connectionIt = connections_.find(completion.clientFd);
        if (connectionIt == connections_.end() ||
            connectionIt->second->fdGeneration() != completion.connectionId) {
            if (acceptedClaimHasReleasableMetaSlot(completion.result)) {
                metaSessionClaimClient_->releaseGameSessionAsync(
                    releaseRequestForAcceptedClaimWithoutSession(
                        completion.result,
                        completion.connectionId));
            }
            continue;
        }

        auto authIt = gameSessionAuthByClientFd_.find(completion.clientFd);
        if (authIt == gameSessionAuthByClientFd_.end() ||
            authIt->second.phase != GameSessionAuthPhase::kClaimPending ||
            authIt->second.connectionId != completion.connectionId ||
            authIt->second.pendingToken != completion.gameSessionToken) {
            if (acceptedClaimHasReleasableMetaSlot(completion.result)) {
                metaSessionClaimClient_->releaseGameSessionAsync(
                    releaseRequestForAcceptedClaimWithoutSession(
                        completion.result,
                        completion.connectionId));
            }
            continue;
        }

        if (!acceptedClaimIsUsable(completion.result, nowUnixMs)) {
            if (acceptedClaimHasReleasableMetaSlot(completion.result)) {
                metaSessionClaimClient_->releaseGameSessionAsync(
                    releaseRequestForAcceptedClaimWithoutSession(
                        completion.result,
                        completion.connectionId));
            }
            authIt->second.phase = GameSessionAuthPhase::kClosing;
            markClientForDisconnect(disconnectedClients, completion.clientFd);
            continue;
        }

        if (completeAcceptedGameSessionClaim(
                *connectionIt->second,
                authIt->second,
                completion.result,
                now,
                disconnectedClients)) {
            authenticatedMembershipChanged = true;
        }
    }

    bool roomChangedByDisconnect = false;
    for (int clientFd : disconnectedClients) {
        roomChangedByDisconnect = disconnectClient(clientFd) || roomChangedByDisconnect;
    }

    if (authenticatedMembershipChanged || roomChangedByDisconnect) {
        sessionCountSnapshot_.store(sessionManager_.size(), std::memory_order_relaxed);
        broadcastStateSnapshots(
            authenticatedMembershipChanged || !disconnectedClients.empty(),
            roomChangedByDisconnect,
            now);
    }
}

void Server::processMetaSessionLivenessReports(Util::TimePoint now) {
    for (const auto& entry : gameSessionAuthByClientFd_) {
        const GameSessionAuthState& authState = entry.second;
        if (authState.phase != GameSessionAuthPhase::kAuthenticated ||
            authState.profile.accountId == 0 ||
            authState.connectionId == 0) {
            continue;
        }

        auto lastIt =
            lastMetaSessionLivenessByConnectionId_.find(authState.connectionId);
        if (lastIt == lastMetaSessionLivenessByConnectionId_.end()) {
            lastMetaSessionLivenessByConnectionId_[authState.connectionId] = now;
            continue;
        }
        if (now - lastIt->second < kMetaSessionLivenessInterval) {
            continue;
        }

        lastIt->second = now;
        metaSessionClaimClient_->renewGameSessionAsync(
            MetaSessionRenewRequest{
                authState.profile.accountId,
                authState.connectionId});
    }
}

void Server::releaseQueuedAcceptedMetaSessionClaimsAfterShutdown() {
    if (metaSessionClaimCompletionSink_ == nullptr ||
        metaSessionClaimReleaseSink_ == nullptr) {
        return;
    }

    std::vector<PendingMetaSessionClaimCompletion> completions;
    {
        std::lock_guard<std::mutex> lock(metaSessionClaimCompletionSink_->mutex);
        completions.swap(metaSessionClaimCompletionSink_->completions);
    }

    for (const PendingMetaSessionClaimCompletion& completion : completions) {
        metaSessionClaimReleaseSink_->releaseAcceptedClaimWithoutServerSession(
            completion.result,
            completion.connectionId);
    }
}

std::vector<uint64_t> Server::collectActiveSessionIds() const {
    std::vector<uint64_t> sessionIds;
    sessionIds.reserve(gameSessionAuthByClientFd_.size());
    for (const auto& entry : gameSessionAuthByClientFd_) {
        const GameSessionAuthState& authState = entry.second;
        if (authState.phase == GameSessionAuthPhase::kAuthenticated &&
            authState.serverSessionId != 0) {
            sessionIds.push_back(authState.serverSessionId);
        }
    }

    std::sort(sessionIds.begin(), sessionIds.end());
    return sessionIds;
}

std::vector<Net::TcpRoomEntry> Server::collectRoomEntries() const {
    const std::vector<Game::RoomSummary> rooms = roomManager_.roomList();
    std::vector<Net::TcpRoomEntry> entries;
    entries.reserve(rooms.size());
    for (const Game::RoomSummary& room : rooms) {
        entries.push_back(
            Net::TcpRoomEntry{
                room.roomId,
                room.playerCount,
                room.maxPlayers,
                room.battleStarted
                    ? Net::TcpRoomStatus::kInProgress
                    : Net::TcpRoomStatus::kOpen,
                room.title});
    }
    std::sort(
        entries.begin(),
        entries.end(),
        [](const Net::TcpRoomEntry& lhs, const Net::TcpRoomEntry& rhs) {
            const auto group = [](const Net::TcpRoomEntry& entry) {
                if (entry.roomStatus == Net::TcpRoomStatus::kInProgress) {
                    return 2;
                }
                return entry.playerCount >= entry.maxPlayers ? 1 : 0;
            };
            const int lhsGroup = group(lhs);
            const int rhsGroup = group(rhs);
            if (lhsGroup != rhsGroup) {
                return lhsGroup < rhsGroup;
            }
            return lhs.roomId < rhs.roomId;
        });
    return entries;
}

std::optional<uint64_t> Server::authenticatedSessionIdForClientFd(int clientFd) const {
    const auto authIt = gameSessionAuthByClientFd_.find(clientFd);
    if (authIt == gameSessionAuthByClientFd_.end() ||
        authIt->second.phase != GameSessionAuthPhase::kAuthenticated ||
        authIt->second.serverSessionId == 0) {
        return std::nullopt;
    }
    return authIt->second.serverSessionId;
}

Net::TcpRoomDetailState Server::buildRoomDetailState(
    const Game::Room& room,
    uint64_t viewerSessionId) const {
    Net::TcpRoomDetailState detail;
    detail.roomId = room.roomId();
    detail.roomStatus =
        room.battleStarted() ? Net::TcpRoomStatus::kInProgress : Net::TcpRoomStatus::kOpen;
    detail.roomTitle = room.title();
    detail.maxPlayers = static_cast<uint8_t>(room.maxPlayers());
    detail.members.reserve(room.playerSessionIds().size());

    for (uint64_t sessionId : room.playerSessionIds()) {
        const std::string fallbackNickname = "Player" + std::to_string(sessionId);
        const std::optional<std::string> authenticatedNickname = nicknameForSession(sessionId);
        detail.members.push_back(
            Net::TcpRoomMemberEntry{
                sessionId,
                authenticatedNickname.has_value() && !authenticatedNickname->empty()
                    ? *authenticatedNickname
                    : fallbackNickname,
                room.isReady(sessionId)});
    }

    if (!room.battleStarted() && room.contains(viewerSessionId)) {
        detail.selfActionMask |= Net::kTcpRoomActionLeaveRoom;
        detail.selfActionMask |= room.isReady(viewerSessionId)
            ? Net::kTcpRoomActionUnready
            : Net::kTcpRoomActionReady;
        if (room.isHost(viewerSessionId) && room.canStartBattle()) {
            detail.selfActionMask |= Net::kTcpRoomActionHostStartBattle;
        }

        if (room.isHost(viewerSessionId)) {
            for (uint64_t targetSessionId : room.playerSessionIds()) {
                if (targetSessionId == viewerSessionId) {
                    continue;
                }
                detail.targetActions.push_back(
                    Net::TcpTargetActionEntry{
                        targetSessionId,
                        Net::kTcpTargetActionHostKick});
            }
        }
    }

    return detail;
}

bool Server::handleTcpPacket(
    ClientConnection& connection,
    const std::vector<uint8_t>& packet,
    Util::TimePoint now,
    std::vector<int>& disconnectedClients,
    bool& outRoomListChanged) {
    auto authIt = gameSessionAuthByClientFd_.find(connection.clientFd());
    if (authIt == gameSessionAuthByClientFd_.end()) {
        markClientForDisconnect(disconnectedClients, connection.clientFd());
        return false;
    }

    Net::TcpPacketHeader header;
    if (!Net::peekTcpPacketHeader(packet.data(), packet.size(), header)) {
        markClientForDisconnect(disconnectedClients, connection.clientFd());
        return false;
    }

    GameSessionAuthState& authState = authIt->second;
    switch (authState.phase) {
    case GameSessionAuthPhase::kUnauthenticated:
        if (header.type != Net::TcpPacketType::kAuthenticateGameSession) {
            return true;
        }
        return handleAuthenticateGameSessionPacket(
            connection,
            packet,
            now,
            disconnectedClients);
    case GameSessionAuthPhase::kClaimPending:
        return true;
    case GameSessionAuthPhase::kAuthenticated:
        if (header.type == Net::TcpPacketType::kAuthenticateGameSession) {
            return true;
        }
        if (header.type == Net::TcpPacketType::kHeartbeatRequest) {
            if (!Net::parseHeartbeatRequestPacket(packet.data(), packet.size(), header)) {
                markClientForDisconnect(disconnectedClients, connection.clientFd());
                return false;
            }
            return true;
        }
        return handleRoomPacket(
            connection,
            packet,
            disconnectedClients,
            outRoomListChanged);
    case GameSessionAuthPhase::kClosing:
        return true;
    }

    return true;
}

bool Server::handleAuthenticateGameSessionPacket(
    ClientConnection& connection,
    const std::vector<uint8_t>& packet,
    Util::TimePoint /*now*/,
    std::vector<int>& disconnectedClients) {
    auto authIt = gameSessionAuthByClientFd_.find(connection.clientFd());
    if (authIt == gameSessionAuthByClientFd_.end() ||
        authIt->second.phase != GameSessionAuthPhase::kUnauthenticated) {
        markClientForDisconnect(disconnectedClients, connection.clientFd());
        return false;
    }

    Net::TcpPacketHeader header;
    std::string token;
    if (!Net::parseAuthenticateGameSessionPacket(
            packet.data(),
            packet.size(),
            header,
            token)) {
        markClientForDisconnect(disconnectedClients, connection.clientFd());
        return false;
    }

    GameSessionAuthState& authState = authIt->second;
    authState.phase = GameSessionAuthPhase::kClaimPending;
    authState.pendingToken = token;
    const int clientFd = connection.clientFd();
    const uint64_t connectionId = authState.connectionId;
    enqueueMetaSessionClaim(clientFd, connectionId, token);
    return true;
}

bool Server::completeAcceptedGameSessionClaim(
    ClientConnection& connection,
    GameSessionAuthState& authState,
    const MetaSessionClaimResult& result,
    Util::TimePoint now,
    std::vector<int>& disconnectedClients) {
    auto oldFdIt = authenticatedClientFdByAccountId_.find(result.profile.accountId);
    if (oldFdIt != authenticatedClientFdByAccountId_.end() &&
        oldFdIt->second != connection.clientFd()) {
        const int oldClientFd = oldFdIt->second;
        auto oldAuthIt = gameSessionAuthByClientFd_.find(oldClientFd);
        if (oldAuthIt != gameSessionAuthByClientFd_.end()) {
            const GameSessionAuthState& oldAuthState = oldAuthIt->second;
            if (oldAuthState.phase == GameSessionAuthPhase::kAuthenticated &&
                oldAuthState.profile.accountId != 0 &&
                oldAuthState.serverSessionId != 0 &&
                metaSessionClaimReleaseSink_ != nullptr) {
                lastMetaSessionLivenessByConnectionId_.erase(
                    oldAuthState.connectionId);
                metaSessionClaimReleaseSink_->release(
                    MetaSessionReleaseRequest{
                        oldAuthState.profile.accountId,
                        oldAuthState.serverSessionId,
                        oldAuthState.connectionId});
            }

            std::array<uint8_t, Net::kTcpHeaderSize> replacedPacket{};
            Net::serializeSessionReplacedPacket(replacedPacket);
            oldAuthIt->second.phase = GameSessionAuthPhase::kClosing;
            oldAuthIt->second.releaseOnDisconnect = false;
            sendPacketToClient(
                oldClientFd,
                replacedPacket.data(),
                replacedPacket.size(),
                disconnectedClients);
        }
        markClientForDisconnect(disconnectedClients, oldClientFd);
        authenticatedClientFdByAccountId_.erase(oldFdIt);
    }

    auto session = sessionManager_.findOrCreate(connection.remoteKey(), now);
    if (!session || session->isBlocked()) {
        if (acceptedClaimHasReleasableMetaSlot(result)) {
            metaSessionClaimClient_->releaseGameSessionAsync(
                releaseRequestForAcceptedClaimWithoutSession(
                    result,
                    authState.connectionId));
        }
        authState.phase = GameSessionAuthPhase::kClosing;
        markClientForDisconnect(disconnectedClients, connection.clientFd());
        return false;
    }

    session->updateLastHeard(now);
    authState.phase = GameSessionAuthPhase::kAuthenticated;
    authState.pendingToken.clear();
    authState.profile = result.profile;
    authState.serverSessionId = session->sessionId();
    authState.releaseOnDisconnect = true;
    lastMetaSessionLivenessByConnectionId_[authState.connectionId] = now;
    authenticatedClientFdByAccountId_[result.profile.accountId] = connection.clientFd();
    lobbySessionIds_.insert(session->sessionId());

    std::array<uint8_t, Net::kWelcomePacketSize> welcomePacket{};
    Net::serializeWelcomePacket(session->sessionId(), welcomePacket);
    if (!sendPacketToClient(
            connection.clientFd(),
            welcomePacket.data(),
            welcomePacket.size(),
            disconnectedClients)) {
        return false;
    }

    return sendCurrentRoomListSnapshotToClient(connection.clientFd(), disconnectedClients);
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
        std::string roomTitle;
        uint8_t maxPlayers = 0;
        if (!Net::parseCreateRoomRequestPacket(
                packet.data(),
                packet.size(),
                header,
                roomTitle,
                maxPlayers)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return false;
        }
        tcpCreateRoomRequestReceived_.fetch_add(1, std::memory_order_relaxed);
        // 서버가 sessionId 기준으로 룸을 생성. 서버 권한 구조에 맞음.
        const Game::RoomCommandResult result =
            roomManager_.createRoom(session->sessionId(), roomTitle, maxPlayers);
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
        tcpCreateRoomResponseSent_.fetch_add(1, std::memory_order_relaxed);

        roomEventDispatcher_.registerRoom(result.room.roomId);
        lobbySessionIds_.erase(session->sessionId());
        if (!sendRoomDetailStateToSession(
                session->sessionId(),
                result.room.roomId,
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
        tcpJoinRoomRequestReceived_.fetch_add(1, std::memory_order_relaxed);

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
        tcpJoinRoomResponseSent_.fetch_add(1, std::memory_order_relaxed);

        lobbySessionIds_.erase(session->sessionId());
        if (!broadcastRoomDetailState(
                result.room.roomId,
                result.playerSessionIds,
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
        lobbySessionIds_.insert(session->sessionId());

        std::array<uint8_t, Net::kRoomIdPacketSize> responsePacket{};
        Net::serializeLeaveRoomResponsePacket(result.room.roomId, responsePacket);
        if (!sendPacketToClient(
                connection.clientFd(),
                responsePacket.data(),
                responsePacket.size(),
                disconnectedClients)) {
            return false;
        }

        if (!broadcastRoomDetailState(
                result.room.roomId,
                result.playerSessionIds,
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
        if (!result.ok) {
            return sendTcpError(
                connection.clientFd(),
                Net::TcpPacketType::kReadyRoomRequest,
                toTcpErrorCode(result.error),
                disconnectedClients);
        }

        std::array<uint8_t, Net::kReadyRoomStatusPacketSize> responsePacket{};
        if (!Net::serializeReadyRoomResponsePacket(
                result.room.roomId,
                result.room.readyPlayerCount,
                result.room.playerCount,
                responsePacket)) {
            return false;
        }
        if (!sendPacketToClient(
                connection.clientFd(),
                responsePacket.data(),
                responsePacket.size(),
                disconnectedClients)) {
            return false;
        }

        return broadcastRoomDetailState(
            result.room.roomId,
            result.playerSessionIds,
            disconnectedClients);
    }

    case Net::TcpPacketType::kUnreadyRoomRequest: {
        if (!Net::parseUnreadyRoomRequestPacket(packet.data(), packet.size(), header)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return false;
        }

        const Game::RoomCommandResult result = roomManager_.markUnready(session->sessionId());
        if (!result.ok) {
            return sendTcpError(
                connection.clientFd(),
                Net::TcpPacketType::kUnreadyRoomRequest,
                toTcpErrorCode(result.error),
                disconnectedClients);
        }

        std::array<uint8_t, Net::kRoomIdPacketSize> responsePacket{};
        if (!Net::serializeUnreadyRoomResponsePacket(result.room.roomId, responsePacket)) {
            return false;
        }
        if (!sendPacketToClient(
                connection.clientFd(),
                responsePacket.data(),
                responsePacket.size(),
                disconnectedClients)) {
            return false;
        }

        return broadcastRoomDetailState(
            result.room.roomId,
            result.playerSessionIds,
            disconnectedClients);
    }

    case Net::TcpPacketType::kHostStartBattleRequest: {
        if (!Net::parseHostStartBattleRequestPacket(packet.data(), packet.size(), header)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return false;
        }

        const Game::RoomCommandResult result = roomManager_.hostStartBattle(session->sessionId());
        if (!result.ok) {
            return sendTcpError(
                connection.clientFd(),
                Net::TcpPacketType::kHostStartBattleRequest,
                toTcpErrorCode(result.error),
                disconnectedClients);
        }

        std::array<uint8_t, Net::kRoomIdPacketSize> responsePacket{};
        if (!Net::serializeHostStartBattleResponsePacket(result.room.roomId, responsePacket)) {
            return false;
        }
        if (!sendPacketToClient(
                connection.clientFd(),
                responsePacket.data(),
                responsePacket.size(),
                disconnectedClients)) {
            return false;
        }
        if (!broadcastBattleStart(result, disconnectedClients)) {
            return false;
        }
        if (!broadcastBattleLoadEntry(result, disconnectedClients)) {
            return false;
        }

        outRoomListChanged = true;
        return true;
    }

    case Net::TcpPacketType::kArenaLoadComplete: {
        uint32_t roomId = 0;
        uint64_t battleInstanceId = 0;
        if (!Net::parseArenaLoadCompletePacket(
                packet.data(),
                packet.size(),
                header,
                roomId,
                battleInstanceId)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return false;
        }

        const Game::RoomCommandResult result = roomManager_.markArenaLoadComplete(
            session->sessionId(),
            roomId,
            battleInstanceId);
        if (!result.ok) {
            return true;
        }

        if (!result.arenaGameplayJustStarted) {
            return true;
        }

        if (!broadcastArenaGameplayStart(result, disconnectedClients)) {
            return false;
        }

        const Game::RoomCommandResult spawnResult =
            roomManager_.spawnMonster(result.room.roomId);
        if (spawnResult.ok && spawnResult.monsterJustSpawned &&
            !broadcastMonsterSpawn(spawnResult, disconnectedClients)) {
            return false;
        }

        return true;
    }

    case Net::TcpPacketType::kHostKickRequest: {
        uint32_t targetSessionId = 0;
        if (!Net::parseHostKickRequestPacket(
                packet.data(),
                packet.size(),
                header,
                targetSessionId)) {
            markClientForDisconnect(disconnectedClients, connection.clientFd());
            return false;
        }

        const Game::RoomCommandResult result =
            roomManager_.hostKick(session->sessionId(), targetSessionId);
        if (!result.ok) {
            return sendTcpError(
                connection.clientFd(),
                Net::TcpPacketType::kHostKickRequest,
                toTcpErrorCode(result.error),
                disconnectedClients);
        }

        std::array<uint8_t, Net::kHostKickResponsePacketSize> responsePacket{};
        if (!Net::serializeHostKickResponsePacket(
                result.room.roomId,
                static_cast<uint32_t>(result.kickedSessionId),
                responsePacket)) {
            return false;
        }
        if (!sendPacketToClient(
                connection.clientFd(),
                responsePacket.data(),
                responsePacket.size(),
                disconnectedClients)) {
            return false;
        }

        lobbySessionIds_.insert(result.kickedSessionId);
        if (!sendLobbyReturnVisibility(
                result.kickedSessionId,
                result.room.roomId,
                Net::TcpLobbyReturnReason::kHostKick,
                disconnectedClients)) {
            return false;
        }
        if (!broadcastRoomDetailState(
                result.room.roomId,
                result.playerSessionIds,
                disconnectedClients)) {
            return false;
        }

        outRoomListChanged = true;
        return true;
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
    std::vector<int> disconnectedClients;
    bool roomListChanged = false;
    const bool succeeded = dispatchRudpRoomEvent(
        event,
        now,
        disconnectedClients,
        roomListChanged);

    bool roomChangedByDisconnect = false;
    for (int clientFd : disconnectedClients) {
        roomChangedByDisconnect = disconnectClient(clientFd) || roomChangedByDisconnect;
    }

    if (!disconnectedClients.empty() || roomListChanged || roomChangedByDisconnect) {
        broadcastStateSnapshots(
            !disconnectedClients.empty(),
            roomListChanged || roomChangedByDisconnect,
            now);
    }
    return succeeded;
}

bool Server::dispatchRudpRoomEvent(
    const Game::RoomEvent& event,
    Util::TimePoint now,
    std::vector<int>& disconnectedClients,
    bool& outRoomListChanged) {
    const Game::RoomEventDispatcherEnqueueResult enqueueResult =
        roomEventDispatcher_.enqueue(event);
    if (enqueueResult.status != Game::RoomEventDispatcherEnqueueStatus::kEnqueued) {
        return false;
    }

    return drainInlineRudpRoomEvents(now, disconnectedClients, outRoomListChanged);
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

bool Server::drainInlineRudpRoomEvents(
    Util::TimePoint now,
    std::vector<int>& disconnectedClients,
    bool& outRoomListChanged) {
    bool allSucceeded = true;
    uint32_t roomId = 0;
    while (roomEventDispatcher_.tryPopActiveRoom(roomId)) {
        Game::RoomEvent event;
        while (roomEventDispatcher_.tryDequeueRoomEvent(roomId, event)) {
            if (!applyInlineRudpRoomEvent(
                    roomId,
                    event,
                    now,
                    disconnectedClients,
                    outRoomListChanged)) {
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

    bool roomListChanged = false;
    bool allSucceeded = flushOutboundQueue(disconnectedClients);
    if (result.status == Game::RoomEventApplyStatus::kApplied &&
        (event.type == Game::RoomEventType::kClickLoot ||
         event.type == Game::RoomEventType::kSpaceLoot) &&
        result.commandResult.lootJustClaimed) {
        allSucceeded =
            completeBattleResultIfReady(
                result.commandResult.room.roomId,
                disconnectedClients,
                roomListChanged) &&
            allSucceeded;
    }
    if (roomListChanged) {
        allSucceeded =
            sendRoomListSnapshotToLobbySessions(disconnectedClients) &&
            allSucceeded;
    }

    return allSucceeded;
}

bool Server::applyInlineRudpRoomEvent(
    uint32_t roomId,
    const Game::RoomEvent& event,
    Util::TimePoint now,
    std::vector<int>& disconnectedClients,
    bool& outRoomListChanged) {
    Game::RoomActor actor(roomId);
    const Game::RoomEventApplyResult result = actor.apply(roomManager_, event);
    if (result.status == Game::RoomEventApplyStatus::kApplied &&
        event.type == Game::RoomEventType::kAttack) {
        recordRelease1RudpAttackReceiveToApplyLatency(now, Util::now());
    }
    if (result.status == Game::RoomEventApplyStatus::kApplied &&
        (event.type == Game::RoomEventType::kClickLoot ||
         event.type == Game::RoomEventType::kSpaceLoot) &&
        result.commandResult.lootJustClaimed) {
        recordRelease1RudpLootClaimReceiveToApplyLatency(now, Util::now());
    }
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

    if (event.type == Game::RoomEventType::kClickLoot) {
        bool allSucceeded = flushRudpOutboundQueue(now);
        if (result.status == Game::RoomEventApplyStatus::kApplied &&
            result.commandResult.lootJustClaimed) {
            allSucceeded =
                completeBattleResultIfReady(
                    result.commandResult.room.roomId,
                    disconnectedClients,
                    outRoomListChanged) &&
                allSucceeded;
        }

        return allSucceeded;
    }

    if (event.type == Game::RoomEventType::kAttack ||
        event.type == Game::RoomEventType::kSpaceLoot) {
        bool allSucceeded = flushOutboundQueue(disconnectedClients);
        if (result.status == Game::RoomEventApplyStatus::kApplied &&
            event.type == Game::RoomEventType::kSpaceLoot &&
            result.commandResult.lootJustClaimed) {
            allSucceeded =
                completeBattleResultIfReady(
                    result.commandResult.room.roomId,
                    disconnectedClients,
                    outRoomListChanged) &&
                allSucceeded;
        }

        return allSucceeded;
    }

    return flushRudpOutboundQueue(now);
}

void Server::recordRelease1RudpMoveReceiveToApplyLatency(
    Util::TimePoint receivedAt,
    Util::TimePoint appliedAt) {
    const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
        appliedAt - receivedAt).count();
    const uint64_t latencyUs =
        elapsedUs < 0 ? 0 : static_cast<uint64_t>(elapsedUs);

    rudpMoveReceiveToApplyLatencySampleCount_.fetch_add(
        1,
        std::memory_order_relaxed);
    rudpMoveReceiveToApplyLatencyTotalUs_.fetch_add(
        latencyUs,
        std::memory_order_relaxed);
    for (size_t index = 0; index < kRelease1PrimaryLatencyBucketCount; ++index) {
        const uint64_t bucketUs =
            static_cast<uint64_t>(kRelease1PrimaryLatencyBucketMs[index]) * 1000U;
        if (latencyUs <= bucketUs) {
            rudpMoveReceiveToApplyLatencyBuckets_[index].fetch_add(
                1,
                std::memory_order_relaxed);
        }
    }
}

void Server::recordRelease1RudpAttackReceiveToApplyLatency(
    Util::TimePoint receivedAt,
    Util::TimePoint appliedAt) {
    const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
        appliedAt - receivedAt).count();
    const uint64_t latencyUs =
        elapsedUs < 0 ? 0 : static_cast<uint64_t>(elapsedUs);

    rudpAttackReceiveToApplyLatencySampleCount_.fetch_add(
        1,
        std::memory_order_relaxed);
    rudpAttackReceiveToApplyLatencyTotalUs_.fetch_add(
        latencyUs,
        std::memory_order_relaxed);
    for (size_t index = 0; index < kRelease1PrimaryLatencyBucketCount; ++index) {
        const uint64_t bucketUs =
            static_cast<uint64_t>(kRelease1PrimaryLatencyBucketMs[index]) * 1000U;
        if (latencyUs <= bucketUs) {
            rudpAttackReceiveToApplyLatencyBuckets_[index].fetch_add(
                1,
                std::memory_order_relaxed);
        }
    }
}

void Server::recordRelease1RudpLootClaimReceiveToApplyLatency(
    Util::TimePoint receivedAt,
    Util::TimePoint appliedAt) {
    const auto elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
        appliedAt - receivedAt).count();
    const uint64_t latencyUs =
        elapsedUs < 0 ? 0 : static_cast<uint64_t>(elapsedUs);

    rudpLootClaimReceiveToApplyLatencySampleCount_.fetch_add(
        1,
        std::memory_order_relaxed);
    rudpLootClaimReceiveToApplyLatencyTotalUs_.fetch_add(
        latencyUs,
        std::memory_order_relaxed);
    for (size_t index = 0; index < kRelease1PrimaryLatencyBucketCount; ++index) {
        const uint64_t bucketUs =
            static_cast<uint64_t>(kRelease1PrimaryLatencyBucketMs[index]) * 1000U;
        if (latencyUs <= bucketUs) {
            rudpLootClaimReceiveToApplyLatencyBuckets_[index].fetch_add(
                1,
                std::memory_order_relaxed);
        }
    }
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
    case Game::OutboundMessageType::kMonsterHealthSnapshot:
        return broadcastMonsterHealthSnapshot(result, disconnectedClients);
    case Game::OutboundMessageType::kDropListSnapshotV2:
        return broadcastDropListSnapshotV2(result, disconnectedClients);
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
    case Game::OutboundMessageType::kMonsterHealthSnapshot:
    case Game::OutboundMessageType::kDropListSnapshotV2:
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

void Server::recordTcpSendFailurePacketType(const uint8_t* data, size_t size) {
    Net::TcpPacketHeader header;
    if (!Net::peekTcpPacketHeader(data, size, header) ||
        header.size < Net::kTcpHeaderSize ||
        header.size > Net::kMaxTcpPacketSize ||
        header.size > size) {
        tcpSendFailureUnknownPacketType_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const std::optional<size_t> metricIndex =
        tcpSendFailurePacketTypeMetricIndex(header.type);
    if (!metricIndex.has_value()) {
        tcpSendFailureUnknownPacketType_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    tcpSendFailureByPacketType_[metricIndex.value()].fetch_add(
        1,
        std::memory_order_relaxed);
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
                if (markClientForDisconnect(disconnectedClients, connection.clientFd())) {
                    tcpDisconnectMarkedEventLoopUpdateFailure_.fetch_add(
                        1,
                        std::memory_order_relaxed);
                }
                return false;
            }
            return true;
        }

        if (markClientForDisconnect(disconnectedClients, connection.clientFd())) {
            recordTcpSendFailurePacketType(
                connection.pendingOutboundData(),
                connection.pendingOutboundSize());
            tcpDisconnectMarkedSendFailure_.fetch_add(1, std::memory_order_relaxed);
        }
        return false;
    }

    if (!setTcpClientWriteInterest(connection, false)) {
        if (markClientForDisconnect(disconnectedClients, connection.clientFd())) {
            tcpDisconnectMarkedEventLoopUpdateFailure_.fetch_add(
                1,
                std::memory_order_relaxed);
        }
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
            if (markClientForDisconnect(disconnectedClients, connection.clientFd())) {
                recordTcpSendFailurePacketType(data, size);
                tcpDisconnectMarkedSendFailure_.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
            return false;
        }
    }

    if (!connection.enqueueOutbound(
            pendingData,
            pendingSize,
            kTcpOutboundPendingLimit)) {
        if (markClientForDisconnect(disconnectedClients, connection.clientFd())) {
            tcpDisconnectMarkedOutboundQueueFull_.fetch_add(
                1,
                std::memory_order_relaxed);
        }
        return false;
    }

    if (!setTcpClientWriteInterest(connection, true)) {
        if (markClientForDisconnect(disconnectedClients, connection.clientFd())) {
            tcpDisconnectMarkedEventLoopUpdateFailure_.fetch_add(
                1,
                std::memory_order_relaxed);
        }
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
            if (markClientForDisconnect(disconnectedClients, clientFd)) {
                tcpDisconnectMarkedMissingConnection_.fetch_add(
                    1,
                    std::memory_order_relaxed);
            }
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

    if (markClientForDisconnect(disconnectedClients, clientFd)) {
        recordTcpSendFailurePacketType(data, size);
        tcpDisconnectMarkedSendFailure_.fetch_add(1, std::memory_order_relaxed);
    }
    return false;
}

bool Server::sendCurrentRoomListSnapshotToClient(
    int clientFd,
    std::vector<int>& disconnectedClients) {
    const std::vector<Net::TcpRoomEntry> rooms = collectRoomEntries();

    std::vector<uint8_t> packet;
    if (!Net::serializeRoomListSnapshotPacket(rooms, packet)) {
        return false;
    }

    tcpRoomListSnapshotDirect_.fetch_add(1, std::memory_order_relaxed);
    tcpRoomListSnapshotBytes_.fetch_add(packet.size(), std::memory_order_relaxed);
    return sendPacketToClient(clientFd, packet.data(), packet.size(), disconnectedClients);
}

bool Server::sendRoomListSnapshotToLobbySessions(std::vector<int>& disconnectedClients) {
    const std::vector<Net::TcpRoomEntry> rooms = collectRoomEntries();
    std::vector<uint8_t> packet;
    if (!Net::serializeRoomListSnapshotPacket(rooms, packet)) {
        return false;
    }

    tcpRoomListSnapshotBroadcast_.fetch_add(1, std::memory_order_relaxed);
    bool allSucceeded = true;
    for (const auto& entry : connections_) {
        const std::optional<uint64_t> sessionId =
            authenticatedSessionIdForClientFd(entry.first);
        if (!sessionId.has_value() ||
            lobbySessionIds_.find(*sessionId) == lobbySessionIds_.end()) {
            continue;
        }
        tcpRoomListSnapshotBroadcastRecipients_.fetch_add(
            1,
            std::memory_order_relaxed);
        tcpRoomListSnapshotBytes_.fetch_add(packet.size(), std::memory_order_relaxed);
        if (!sendPacketToClient(entry.first, packet.data(), packet.size(), disconnectedClients)) {
            allSucceeded = false;
        }
    }

    return allSucceeded;
}

bool Server::sendRoomDetailStateToSession(
    uint64_t sessionId,
    uint32_t roomId,
    std::vector<int>& disconnectedClients) {
    const Game::Room* room = roomManager_.findRoom(roomId);
    if (room == nullptr || !room->contains(sessionId)) {
        return true;
    }

    const int clientFd = findClientFdForSession(sessionId);
    if (clientFd < 0) {
        return true;
    }

    std::vector<uint8_t> packet;
    const Net::TcpRoomDetailState detail = buildRoomDetailState(*room, sessionId);
    if (!Net::serializeRoomDetailStatePacket(detail, packet)) {
        return false;
    }

    return sendPacketToClient(clientFd, packet.data(), packet.size(), disconnectedClients);
}

bool Server::broadcastRoomDetailState(
    uint32_t roomId,
    const std::vector<uint64_t>& sessionIds,
    std::vector<int>& disconnectedClients) {
    bool allSucceeded = true;
    for (uint64_t sessionId : sessionIds) {
        if (!sendRoomDetailStateToSession(sessionId, roomId, disconnectedClients)) {
            allSucceeded = false;
        }
    }
    return allSucceeded;
}

bool Server::sendLobbyReturnVisibility(
    uint64_t sessionId,
    uint32_t previousRoomId,
    Net::TcpLobbyReturnReason reason,
    std::vector<int>& disconnectedClients) {
    const int clientFd = findClientFdForSession(sessionId);
    if (clientFd < 0) {
        return true;
    }

    std::array<uint8_t, Net::kLobbyReturnVisibilityPacketSize> packet{};
    if (!Net::serializeLobbyReturnVisibilityPacket(previousRoomId, reason, packet)) {
        return false;
    }

    return sendPacketToClient(clientFd, packet.data(), packet.size(), disconnectedClients);
}

bool Server::completeBattleResultIfReady(
    uint32_t roomId,
    std::vector<int>& disconnectedClients,
    bool& outRoomListChanged) {
    if (roomId == 0) {
        return true;
    }

    const Game::BattleFinalRankingResult ranking = roomManager_.buildBattleFinalRanking(
        roomId,
        [this](uint64_t sessionId) { return nicknameForSession(sessionId); });

    switch (ranking.status) {
    case Game::BattleFinalRankingStatus::kOk:
    case Game::BattleFinalRankingStatus::kResultGenerationFailure:
        return completeBattleResult(ranking, disconnectedClients, outRoomListChanged);
    case Game::BattleFinalRankingStatus::kRoomNotFound:
    case Game::BattleFinalRankingStatus::kNotInGameplay:
    case Game::BattleFinalRankingStatus::kNotComplete:
        return true;
    }

    return true;
}

bool Server::completeBattleResult(
    const Game::BattleFinalRankingResult& ranking,
    std::vector<int>& disconnectedClients,
    bool& outRoomListChanged) {
    bool allSucceeded = true;
    if (ranking.status == Game::BattleFinalRankingStatus::kOk) {
        allSucceeded =
            broadcastBattleFinalRanking(ranking, disconnectedClients) && allSucceeded;
        allSucceeded = sendBattleResultLobbyReturn(
            ranking.participantSessionIds,
            ranking.roomId,
            Net::TcpLobbyReturnReason::kNone,
            disconnectedClients) && allSucceeded;
    } else if (ranking.status == Game::BattleFinalRankingStatus::kResultGenerationFailure) {
        allSucceeded = sendBattleResultLobbyReturn(
            ranking.participantSessionIds,
            ranking.roomId,
            Net::TcpLobbyReturnReason::kResultGenerationFailure,
            disconnectedClients) && allSucceeded;
    } else {
        return true;
    }

    closeBattleResultRoom(ranking.roomId, ranking.participantSessionIds);
    outRoomListChanged = true;
    return allSucceeded;
}

bool Server::broadcastBattleFinalRanking(
    const Game::BattleFinalRankingResult& ranking,
    std::vector<int>& disconnectedClients) {
    std::vector<uint8_t> packet;
    const std::vector<Net::BattleFinalRankingEntry> rows =
        toTcpBattleFinalRankingEntries(ranking.rows);
    if (!Net::serializeBattleFinalRankingPacket(
            ranking.roomId,
            ranking.battleInstanceId,
            rows,
            packet)) {
        return false;
    }

    bool allSucceeded = true;
    for (uint64_t sessionId : ranking.participantSessionIds) {
        const int clientFd = findClientFdForSession(sessionId);
        if (clientFd < 0) {
            continue;
        }
        if (!sendPacketToClient(clientFd, packet.data(), packet.size(), disconnectedClients)) {
            allSucceeded = false;
        }
    }
    return allSucceeded;
}

bool Server::sendBattleResultLobbyReturn(
    const std::vector<uint64_t>& sessionIds,
    uint32_t previousRoomId,
    Net::TcpLobbyReturnReason reason,
    std::vector<int>& disconnectedClients) {
    bool allSucceeded = true;
    for (uint64_t sessionId : sessionIds) {
        if (findClientFdForSession(sessionId) >= 0) {
            lobbySessionIds_.insert(sessionId);
        }
        if (!sendLobbyReturnVisibility(
                sessionId,
                previousRoomId,
                reason,
                disconnectedClients)) {
            allSucceeded = false;
        }
    }
    return allSucceeded;
}

void Server::closeBattleResultRoom(
    uint32_t roomId,
    const std::vector<uint64_t>& sessionIds) {
    roomEventDispatcher_.beginRoomShutdown(roomId);
    const std::vector<uint64_t> closedSessionIds = roomManager_.closeRoom(roomId);

    for (uint64_t sessionId : sessionIds) {
        rudpMoveDispatchStateBySession_.erase(sessionId);
        clearRudpReliableEventsForSession(sessionId);
        clearRudpOutboundSequenceForSession(sessionId);
    }
    for (uint64_t sessionId : closedSessionIds) {
        rudpMoveDispatchStateBySession_.erase(sessionId);
        clearRudpReliableEventsForSession(sessionId);
        clearRudpOutboundSequenceForSession(sessionId);
    }
}

std::optional<std::string> Server::nicknameForSession(uint64_t sessionId) const {
    for (const auto& entry : gameSessionAuthByClientFd_) {
        const GameSessionAuthState& authState = entry.second;
        if (authState.phase == GameSessionAuthPhase::kAuthenticated &&
            authState.serverSessionId == sessionId) {
            return authState.profile.nickname;
        }
    }
    return std::nullopt;
}

bool Server::broadcastBattleStart(
    const Game::RoomCommandResult& result,
    std::vector<int>& disconnectedClients) {
    if (result.playerSessionIds.size() < 2u) {
        return true;
    }
    enqueueRudpBattleStartEvent(result, Util::now());

    // 패킷을 client마다 따로 만들지 않고 한 번만 만들어 fanout한다.
    std::vector<uint8_t> packet;
    if (result.playerSessionIds.size() == 2u) {
        std::array<uint8_t, Net::kBattleStartPacketSize> legacyPacket{};
        Net::serializeBattleStartPacket(
            result.room.roomId,
            result.playerSessionIds[0],
            result.playerSessionIds[1],
            legacyPacket);
        packet.assign(legacyPacket.begin(), legacyPacket.end());
    } else if (!Net::serializeBattleStartRosterPacket(
                   result.room.roomId,
                   result.playerSessionIds,
                   packet)) {
        return true;
    }

    bool allSucceeded = true;   // 전체 성공 여부 추적 변수. 하나라도 실패 시 false로 바꾼다.
    for (const auto& entry : connections_) {    // 서버에 연결된 모든 clients 순회
        const std::optional<uint64_t> sessionId =
            authenticatedSessionIdForClientFd(entry.first);
        if (!sessionId.has_value() || !isRoomMember(result, *sessionId)) {
            continue;
        }

        if (!sendPacketToClient(entry.first, packet.data(), packet.size(), disconnectedClients)) {
            allSucceeded = false;
        }
    }

    return allSucceeded;
}

bool Server::broadcastBattleLoadEntry(
    const Game::RoomCommandResult& result,
    std::vector<int>& disconnectedClients) {
    if (result.playerSessionIds.size() < 2u || result.battleInstanceId == 0) {
        return true;
    }

    std::vector<uint8_t> packet;
    if (!Net::serializeBattleLoadEntryPacket(
            result.room.roomId,
            result.battleInstanceId,
            result.playerSessionIds,
            packet)) {
        return false;
    }

    bool allSucceeded = true;
    for (const auto& entry : connections_) {
        const std::optional<uint64_t> sessionId =
            authenticatedSessionIdForClientFd(entry.first);
        if (!sessionId.has_value() || !isRoomMember(result, *sessionId)) {
            continue;
        }

        if (!sendPacketToClient(entry.first, packet.data(), packet.size(), disconnectedClients)) {
            allSucceeded = false;
        }
    }

    return allSucceeded;
}

bool Server::broadcastArenaGameplayStart(
    const Game::RoomCommandResult& result,
    std::vector<int>& disconnectedClients) {
    if (result.playerSessionIds.size() < 2u || result.battleInstanceId == 0) {
        return true;
    }

    std::array<uint8_t, Net::kArenaGameplayStartPacketSize> packet{};
    if (!Net::serializeArenaGameplayStartPacket(
            result.room.roomId,
            result.battleInstanceId,
            packet)) {
        return false;
    }

    bool allSucceeded = true;
    for (const auto& entry : connections_) {
        const std::optional<uint64_t> sessionId =
            authenticatedSessionIdForClientFd(entry.first);
        if (!sessionId.has_value() || !isRoomMember(result, *sessionId)) {
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
        const std::optional<uint64_t> sessionId =
            authenticatedSessionIdForClientFd(entry.first);
        if (!sessionId.has_value() || !isRoomMember(result, *sessionId)) {
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
        const std::optional<uint64_t> sessionId =
            authenticatedSessionIdForClientFd(entry.first);
        if (!sessionId.has_value() || !isRoomMember(result, *sessionId)) {
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
        const std::optional<uint64_t> sessionId =
            authenticatedSessionIdForClientFd(entry.first);
        if (!sessionId.has_value() || !isRoomMember(result, *sessionId)) {
            continue;
        }

        if (!sendPacketToClient(entry.first, packet.data(), packet.size(), disconnectedClients)) {
            allSucceeded = false;
        }
    }

    return allSucceeded;
}

bool Server::broadcastMonsterHealthSnapshot(
    const Game::RoomCommandResult& result,
    std::vector<int>& disconnectedClients) {
    std::array<uint8_t, Net::kMonsterHealthSnapshotPacketSize> packet{};
    if (!Net::serializeMonsterHealthSnapshotPacket(
            result.room.roomId,
            result.monster.monsterId,
            result.monster.currentHp,
            result.monster.maxHp,
            packet)) {
        return false;
    }

    bool allSucceeded = true;
    for (const auto& entry : connections_) {
        const std::optional<uint64_t> sessionId =
            authenticatedSessionIdForClientFd(entry.first);
        if (!sessionId.has_value() || !isRoomMember(result, *sessionId)) {
            continue;
        }

        if (!sendPacketToClient(entry.first, packet.data(), packet.size(), disconnectedClients)) {
            allSucceeded = false;
        }
    }

    return allSucceeded;
}

bool Server::broadcastDropListSnapshotV2(
    const Game::RoomCommandResult& result,
    std::vector<int>& disconnectedClients) {
    std::vector<uint8_t> packet;
    const std::vector<Net::TcpDropEntryV2> drops = toTcpDropEntryV2s(result.drops);
    if (!Net::serializeDropListSnapshotV2Packet(
            result.room.roomId,
            result.scatterSeed,
            drops,
            packet)) {
        return false;
    }

    bool allSucceeded = true;
    for (const auto& entry : connections_) {
        const std::optional<uint64_t> sessionId =
            authenticatedSessionIdForClientFd(entry.first);
        if (!sessionId.has_value() || !isRoomMember(result, *sessionId)) {
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
        const std::optional<uint64_t> sessionId =
            authenticatedSessionIdForClientFd(entry.first);
        if (!sessionId.has_value() || !isRoomMember(result, *sessionId)) {
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
    for (const auto& entry : gameSessionAuthByClientFd_) {
        if (entry.second.phase == GameSessionAuthPhase::kAuthenticated &&
            entry.second.serverSessionId == sessionId) {
            return entry.first;
        }
    }

    return -1;
}

void Server::scheduleRoomListSnapshotBroadcast(Util::TimePoint now) {
    if (pendingRoomListSnapshotBroadcast_) {
        return;
    }

    pendingRoomListSnapshotBroadcast_ = true;
    pendingRoomListSnapshotBroadcastDueAt_ =
        now + kRoomListSnapshotBroadcastDebounce;
}

bool Server::flushPendingRoomListSnapshotBroadcast(
    Util::TimePoint now,
    std::vector<int>& disconnectedClients) {
    if (!pendingRoomListSnapshotBroadcast_) {
        return true;
    }
    if (now < pendingRoomListSnapshotBroadcastDueAt_) {
        return true;
    }

    pendingRoomListSnapshotBroadcast_ = false;
    pendingRoomListSnapshotBroadcastDueAt_ = Util::TimePoint{};
    return sendRoomListSnapshotToLobbySessions(disconnectedClients);
}

void Server::processPendingRoomListSnapshotBroadcast(Util::TimePoint now) {
    std::vector<int> disconnectedClients;
    flushPendingRoomListSnapshotBroadcast(now, disconnectedClients);

    bool roomChangedByDisconnect = false;
    for (int clientFd : disconnectedClients) {
        roomChangedByDisconnect = disconnectClient(clientFd) || roomChangedByDisconnect;
    }

    if (!disconnectedClients.empty() || roomChangedByDisconnect) {
        broadcastStateSnapshots(
            !disconnectedClients.empty(),
            roomChangedByDisconnect,
            now);
    }
}

void Server::broadcastStateSnapshots(bool clientListChanged, bool roomListChanged) {
    broadcastStateSnapshots(clientListChanged, roomListChanged, Util::now());
}

void Server::broadcastStateSnapshots(
    bool clientListChanged,
    bool roomListChanged,
    Util::TimePoint now) {
    if (roomListChanged) {
        scheduleRoomListSnapshotBroadcast(now);
    }

    while (!connections_.empty() && clientListChanged) {
        std::vector<uint8_t> clientSnapshotPacket;

        if (clientListChanged) {
            const std::vector<uint64_t> sessionIds = collectActiveSessionIds();
            if (!Net::serializeClientListSnapshotPacket(sessionIds, clientSnapshotPacket)) {
                return;
            }
        }

        std::vector<int> failedClients;
        for (const auto& entry : connections_) {
            const auto authIt = gameSessionAuthByClientFd_.find(entry.first);
            const bool isAuthenticatedRecipient =
                authIt != gameSessionAuthByClientFd_.end() &&
                authIt->second.phase == GameSessionAuthPhase::kAuthenticated &&
                authIt->second.serverSessionId != 0;
            if (clientListChanged &&
                isAuthenticatedRecipient &&
                !sendPacketToClient(
                    entry.first,
                    clientSnapshotPacket.data(),
                    clientSnapshotPacket.size(),
                    failedClients)) {
                continue;
            }
        }

        if (failedClients.empty()) {
            return;
        }

        clientListChanged = true;
        for (int clientFd : failedClients) {
            if (disconnectClient(clientFd)) {
                scheduleRoomListSnapshotBroadcast(now);
            }
        }
    }
}

bool Server::disconnectClient(int clientFd) {
    auto it = connections_.find(clientFd);
    if (it == connections_.end()) {
        return false;
    }

    tcpDisconnectTotal_.fetch_add(1, std::memory_order_relaxed);

    uint64_t sessionId = 0;
    bool releaseOnDisconnect = false;
    MetaSessionReleaseRequest releaseRequest;
    auto authIt = gameSessionAuthByClientFd_.find(clientFd);
    if (authIt != gameSessionAuthByClientFd_.end()) {
        GameSessionAuthState& authState = authIt->second;
        sessionId = authState.serverSessionId;
        releaseOnDisconnect =
            authState.phase == GameSessionAuthPhase::kAuthenticated &&
            authState.releaseOnDisconnect &&
            authState.profile.accountId != 0 &&
            authState.serverSessionId != 0;
        releaseRequest = MetaSessionReleaseRequest{
            authState.profile.accountId,
            authState.serverSessionId,
            authState.connectionId};
        lastMetaSessionLivenessByConnectionId_.erase(authState.connectionId);
        if (authState.profile.accountId != 0) {
            auto accountIt =
                authenticatedClientFdByAccountId_.find(authState.profile.accountId);
            if (accountIt != authenticatedClientFdByAccountId_.end() &&
                accountIt->second == clientFd) {
                authenticatedClientFdByAccountId_.erase(accountIt);
            }
        }
    }

    const bool roomChanged =
        sessionId != 0 && roomManager_.leaveRoom(sessionId).ok;
    if (sessionId != 0) {
        lobbySessionIds_.erase(sessionId);
    }
    if (networkEventLoop_ != nullptr) {
        networkEventLoop_->unregisterFd(
            Net::NetworkFdToken{clientFd, it->second->fdGeneration()});
    }
    if (sessionId != 0) {
        rudpSessionBinder_.removeBySessionId(sessionId);
        rudpInputCommandSequenceTracker_.removeSession(sessionId);
        rudpMoveInputGuard_.removeSession(sessionId);
        rudpMoveDispatchStateBySession_.erase(sessionId);
        clearRudpReliableEventsForSession(sessionId);
        clearRudpOutboundSequenceForSession(sessionId);
        sessionManager_.remove(it->second->remoteKey());
    }
    rudpBindingCountSnapshot_.store(
        rudpSessionBinder_.size(),
        std::memory_order_relaxed);
    listener_.closeClient(clientFd);
    connections_.erase(it);
    gameSessionAuthByClientFd_.erase(clientFd);
    activeConnectionCount_.store(connections_.size(), std::memory_order_relaxed);
    sessionCountSnapshot_.store(sessionManager_.size(), std::memory_order_relaxed);
    if (releaseOnDisconnect) {
        metaSessionClaimClient_->releaseGameSessionAsync(releaseRequest);
    }
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
