#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Net {
constexpr size_t kTcpHeaderSize = 4;
constexpr uint16_t kMaxTcpPacketSize = 1024;
constexpr size_t kWelcomePacketSize = 12;
constexpr size_t kSessionIdFieldSize = 8;
constexpr size_t kRoomIdFieldSize = 4;
constexpr size_t kPlayerCountFieldSize = 2;
constexpr size_t kClientListCountFieldSize = 2;
constexpr size_t kClientListEntrySize = kSessionIdFieldSize;
constexpr size_t kRoomStatusPacketSize = kTcpHeaderSize + kRoomIdFieldSize + kPlayerCountFieldSize;
constexpr size_t kReadyRoomStatusPacketSize =
    kTcpHeaderSize + kRoomIdFieldSize + (2 * kPlayerCountFieldSize);    // 준비한 사람 수 + 전체 멤버 수
constexpr size_t kRoomIdPacketSize = kTcpHeaderSize + kRoomIdFieldSize;
constexpr size_t kRoomListCountFieldSize = 2;
constexpr size_t kRoomEntrySize = 8;
constexpr size_t kBattleStartPacketSize = kTcpHeaderSize + kRoomIdFieldSize + (2 * kSessionIdFieldSize);
constexpr size_t kMonsterIdFieldSize = 4;
constexpr size_t kMonsterTypeIdFieldSize = 4;
constexpr size_t kMonsterHpFieldSize = 2;
constexpr size_t kMonsterSpawnPacketSize =
    kTcpHeaderSize + kRoomIdFieldSize + kMonsterIdFieldSize + kMonsterTypeIdFieldSize + kMonsterHpFieldSize;
constexpr size_t kMonsterDeathRequestPacketSize = kTcpHeaderSize + kMonsterIdFieldSize;
constexpr size_t kMonsterDeathPacketSize = kTcpHeaderSize + kRoomIdFieldSize + kMonsterIdFieldSize;
constexpr size_t kDropListCountFieldSize = 2;
constexpr size_t kDropIdFieldSize = 4;
constexpr size_t kItemIdFieldSize = 4;
constexpr size_t kQuantityFieldSize = 2;
constexpr size_t kDropEntrySize = kDropIdFieldSize + kItemIdFieldSize + kQuantityFieldSize;
constexpr size_t kWeightFieldSize = 2;
constexpr size_t kLootRejectReasonFieldSize = 2;
constexpr size_t kInventoryCountFieldSize = 2;
constexpr size_t kInventoryEntrySize = kItemIdFieldSize + kQuantityFieldSize;
constexpr size_t kClickLootRequestPacketSize = kTcpHeaderSize + kDropIdFieldSize;
constexpr size_t kLootResolvedPacketSize =
    kTcpHeaderSize + kRoomIdFieldSize + kDropIdFieldSize + kSessionIdFieldSize + kItemIdFieldSize +
    kQuantityFieldSize;
constexpr size_t kLootRejectedPacketSize =
    kTcpHeaderSize + kRoomIdFieldSize + kDropIdFieldSize + kLootRejectReasonFieldSize;
constexpr size_t kFinishSessionRequestPacketSize = kTcpHeaderSize;
constexpr size_t kSmokeCreateCenterDropRequestPacketSize = kTcpHeaderSize;
constexpr size_t kSmokePlacePlayersAroundCenterDropRequestPacketSize = kTcpHeaderSize;
constexpr size_t kSettlementIdLengthFieldSize = 2;
constexpr size_t kSettlementIdMaxLength = 64;
constexpr size_t kTimestampFieldSize = 8;
constexpr size_t kGoldDeltaFieldSize = 8;
constexpr size_t kSettlementReasonFieldSize = 2;
constexpr size_t kSettlementInventoryDeltaCountFieldSize = 2;
constexpr size_t kQuantityDeltaFieldSize = 4;
constexpr size_t kSettlementInventoryDeltaEntrySize =
    kItemIdFieldSize + kQuantityDeltaFieldSize + kDropIdFieldSize;
constexpr size_t kSettlementResultFixedPayloadSize =
    kSettlementIdLengthFieldSize + (2 * kSessionIdFieldSize) + kRoomIdFieldSize +
    (2 * kTimestampFieldSize) + kGoldDeltaFieldSize + kSettlementReasonFieldSize +
    kSettlementInventoryDeltaCountFieldSize;
constexpr size_t kMetaResponseOpFieldSize = 2;
constexpr size_t kMetaResponseStatusFieldSize = 2;
constexpr size_t kRetryAfterMsFieldSize = 4;
constexpr size_t kMetaResponseFixedPayloadSize =
    kMetaResponseOpFieldSize + kSettlementIdLengthFieldSize + kMetaResponseStatusFieldSize +
    kRetryAfterMsFieldSize;
constexpr size_t kErrorPacketSize = 8;

enum class TcpPacketType : uint16_t {
    kWelcome = 0x0001,
    kClientListSnapshot = 0x0002,
    kCreateRoomRequest = 0x0101,
    kCreateRoomResponse = 0x0102,
    kJoinRoomRequest = 0x0103,
    kJoinRoomResponse = 0x0104,
    kLeaveRoomRequest = 0x0105,
    kLeaveRoomResponse = 0x0106,
    kRoomListSnapshot = 0x0107,
    kReadyRoomRequest = 0x0108,     // payload 없는 header-only 요청
    kReadyRoomResponse = 0x0109,    // 요청자 관점의 상태 응답
    kBattleStart = 0x010A,          // 룸 멤버 전체에게 가는 상태 전이 이벤트
    kMonsterSpawn = 0x010B,
    kMonsterDeathRequest = 0x010C,
    kMonsterDeath = 0x010D,
    kDropListSnapshot = 0x010E,
    kClickLootRequest = 0x010F,
    kLootResolved = 0x0110,
    kLootRejected = 0x0111,
    kInventorySnapshot = 0x0112,
    kFinishSessionRequest = 0x0113,
    kSettlementResult = 0x0114,
    kMetaResponse = 0x0115,
    kSmokeCreateCenterDropRequest = 0x0116,
    kSmokePlacePlayersAroundCenterDropRequest = 0x0117,
    kError = 0x01FF,
};

enum class TcpErrorCode : uint16_t {
    kNone = 0,
    kFull = 1,
    kNotFound = 2,
    kAlreadyInRoom = 3,
    kNotInRoom = 4,
};

struct TcpPacketHeader {
    uint16_t size{0};
    TcpPacketType type{TcpPacketType::kWelcome};
};

struct TcpRoomEntry {
    uint32_t roomId{0};
    uint16_t playerCount{0};
    uint16_t maxPlayers{0};
};

struct TcpDropEntry {
    uint32_t dropId{0};
    uint32_t itemId{0};
    uint16_t quantity{0};
};

enum class TcpLootRejectReason : uint16_t {
    kNone = 0,
    kAlreadyClaimed = 1,
    kOverweight = 2,
};

struct TcpInventoryEntry {
    uint32_t itemId{0};
    uint16_t quantity{0};
};

enum class TcpSettlementReason : uint16_t {
    kNormal = 0,    // 정상 종료
    kDisconnect = 1,    // 연결 끊김으로 인한 종료
    kServerShutdown = 2,    // 서버 종료로 인한 정산
    kForcedClose = 3,   // 강제 종료
};

struct TcpSettlementInventoryDelta {
    uint32_t itemId{0};
    int32_t quantityDelta{0};   // 감소량 또한 표시해야 하니 signed.
    uint32_t sourceDropId{0};
};

struct TcpSettlementResult {
    std::string settlementId;
    uint64_t sessionId{0};
    uint64_t accountId{0};
    uint32_t roomId{0};
    uint64_t startedAtUnixMs{0};
    uint64_t finishedAtUnixMs{0};
    int64_t goldDelta{0};
    TcpSettlementReason reason{TcpSettlementReason::kNormal};
    std::vector<TcpSettlementInventoryDelta> inventoryDeltas;
};

enum class TcpMetaResponseOp : uint16_t {
    kSettlementApplied = 1,
    kSettlementDuplicate = 2,
    kSettlementRejected = 3,
    kSettlementRetryLater = 4,
};

enum class TcpMetaResponseStatus : uint16_t {
    kApplied = 1,
    kDuplicate = 2,
    kRejected = 3,
    kRetryLater = 4,
};

struct TcpMetaResponse {
    TcpMetaResponseOp op{TcpMetaResponseOp::kSettlementApplied};
    std::string settlementId;
    TcpMetaResponseStatus status{TcpMetaResponseStatus::kApplied};
    uint32_t retryAfterMs{0};
};

bool serializeWelcomePacket(uint64_t sessionId, std::array<uint8_t, kWelcomePacketSize>& outPacket);
bool serializeCreateRoomRequestPacket(std::array<uint8_t, kTcpHeaderSize>& outPacket);
bool serializeCreateRoomResponsePacket(
    uint32_t roomId,
    uint16_t playerCount,
    std::array<uint8_t, kRoomStatusPacketSize>& outPacket);
bool serializeJoinRoomRequestPacket(
    uint32_t roomId,
    std::array<uint8_t, kRoomIdPacketSize>& outPacket);
bool serializeJoinRoomResponsePacket(
    uint32_t roomId,
    uint16_t playerCount,
    std::array<uint8_t, kRoomStatusPacketSize>& outPacket);
bool serializeLeaveRoomRequestPacket(std::array<uint8_t, kTcpHeaderSize>& outPacket);
bool serializeReadyRoomRequestPacket(std::array<uint8_t, kTcpHeaderSize>& outPacket);
bool serializeLeaveRoomResponsePacket(
    uint32_t roomId,
    std::array<uint8_t, kRoomIdPacketSize>& outPacket);
bool serializeReadyRoomResponsePacket(
    uint32_t roomId,
    uint16_t readyPlayerCount,
    uint16_t totalPlayerCount,
    std::array<uint8_t, kReadyRoomStatusPacketSize>& outPacket);
size_t clientListSnapshotPacketSize(size_t sessionCount);
bool serializeClientListSnapshotPacket(
    const std::vector<uint64_t>& sessionIds,
    std::vector<uint8_t>& outPacket);
size_t roomListSnapshotPacketSize(size_t roomCount);
bool serializeRoomListSnapshotPacket(
    const std::vector<TcpRoomEntry>& rooms,
    std::vector<uint8_t>& outPacket);
bool serializeBattleStartPacket(
    uint32_t roomId,
    uint64_t playerASessionId,
    uint64_t playerBSessionId,
    std::array<uint8_t, kBattleStartPacketSize>& outPacket);
bool serializeMonsterSpawnPacket(
    uint32_t roomId,
    uint32_t monsterId,
    uint32_t monsterTypeId,
    uint16_t maxHp,
    std::array<uint8_t, kMonsterSpawnPacketSize>& outPacket);
bool serializeMonsterDeathRequestPacket(
    uint32_t monsterId,
    std::array<uint8_t, kMonsterDeathRequestPacketSize>& outPacket);
bool serializeMonsterDeathPacket(
    uint32_t roomId,
    uint32_t monsterId,
    std::array<uint8_t, kMonsterDeathPacketSize>& outPacket);
size_t dropListSnapshotPacketSize(size_t dropCount);
bool serializeDropListSnapshotPacket(
    uint32_t roomId,
    const std::vector<TcpDropEntry>& drops,
    std::vector<uint8_t>& outPacket);
bool serializeClickLootRequestPacket(
    uint32_t dropId,
    std::array<uint8_t, kClickLootRequestPacketSize>& outPacket);
bool serializeLootResolvedPacket(
    uint32_t roomId,
    uint32_t dropId,
    uint64_t winnerSessionId,
    uint32_t itemId,
    uint16_t quantity,
    std::array<uint8_t, kLootResolvedPacketSize>& outPacket);
bool serializeLootRejectedPacket(
    uint32_t roomId,
    uint32_t dropId,
    TcpLootRejectReason reason,
    std::array<uint8_t, kLootRejectedPacketSize>& outPacket);
size_t inventorySnapshotPacketSize(size_t entryCount);
bool serializeInventorySnapshotPacket(
    uint64_t sessionId,
    uint16_t currentWeight,
    uint16_t maxWeight,
    const std::vector<TcpInventoryEntry>& entries,
    std::vector<uint8_t>& outPacket);
bool serializeFinishSessionRequestPacket(std::array<uint8_t, kFinishSessionRequestPacketSize>& outPacket);
bool serializeSmokeCreateCenterDropRequestPacket(
    std::array<uint8_t, kSmokeCreateCenterDropRequestPacketSize>& outPacket);
bool serializeSmokePlacePlayersAroundCenterDropRequestPacket(
    std::array<uint8_t, kSmokePlacePlayersAroundCenterDropRequestPacketSize>& outPacket);
size_t settlementResultPacketSize(size_t settlementIdLength, size_t inventoryDeltaCount);
bool serializeSettlementResultPacket(
    const TcpSettlementResult& settlement,
    std::vector<uint8_t>& outPacket);
size_t metaResponsePacketSize(size_t settlementIdLength);
bool serializeMetaResponsePacket(
    const TcpMetaResponse& response,
    std::vector<uint8_t>& outPacket);
bool serializeErrorPacket(
    TcpPacketType failedType,
    TcpErrorCode errorCode,
    std::array<uint8_t, kErrorPacketSize>& outPacket);
bool peekTcpPacketHeader(const uint8_t* data, size_t size, TcpPacketHeader& outHeader);
bool parseTcpPacketHeader(const uint8_t* data, size_t size, TcpPacketHeader& outHeader);
bool parseWelcomePacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint64_t& outSessionId);
bool parseCreateRoomRequestPacket(const uint8_t* data, size_t size, TcpPacketHeader& outHeader);
bool parseCreateRoomResponsePacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint16_t& outPlayerCount);
bool parseJoinRoomRequestPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId);
bool parseJoinRoomResponsePacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint16_t& outPlayerCount);
bool parseLeaveRoomRequestPacket(const uint8_t* data, size_t size, TcpPacketHeader& outHeader);
bool parseReadyRoomRequestPacket(const uint8_t* data, size_t size, TcpPacketHeader& outHeader);
bool parseLeaveRoomResponsePacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId);
bool parseReadyRoomResponsePacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint16_t& outReadyPlayerCount,
    uint16_t& outTotalPlayerCount);
bool parseClientListSnapshotPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    std::vector<uint64_t>& outSessionIds);
bool parseRoomListSnapshotPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    std::vector<TcpRoomEntry>& outRooms);
bool parseBattleStartPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint64_t& outPlayerASessionId,
    uint64_t& outPlayerBSessionId);
bool parseMonsterSpawnPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint32_t& outMonsterId,
    uint32_t& outMonsterTypeId,
    uint16_t& outMaxHp);
bool parseMonsterDeathRequestPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outMonsterId);
bool parseMonsterDeathPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint32_t& outMonsterId);
bool parseDropListSnapshotPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    std::vector<TcpDropEntry>& outDrops);
bool parseClickLootRequestPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outDropId);
bool parseLootResolvedPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint32_t& outDropId,
    uint64_t& outWinnerSessionId,
    uint32_t& outItemId,
    uint16_t& outQuantity);
bool parseLootRejectedPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint32_t& outDropId,
    TcpLootRejectReason& outReason);
bool parseInventorySnapshotPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint64_t& outSessionId,
    uint16_t& outCurrentWeight,
    uint16_t& outMaxWeight,
    std::vector<TcpInventoryEntry>& outEntries);
bool parseFinishSessionRequestPacket(const uint8_t* data, size_t size, TcpPacketHeader& outHeader);
bool parseSmokeCreateCenterDropRequestPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader);
bool parseSmokePlacePlayersAroundCenterDropRequestPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader);
bool parseSettlementResultPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    TcpSettlementResult& outSettlement);
bool parseMetaResponsePacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    TcpMetaResponse& outResponse);
bool parseErrorPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    TcpPacketType& outFailedType,
    TcpErrorCode& outErrorCode);
}  // namespace Net
