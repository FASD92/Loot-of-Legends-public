#include "Net/TcpPacket.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace {
void writeU16BE(uint16_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[1] = static_cast<uint8_t>(value & 0xFF);
}

void writeU64BE(uint64_t value, uint8_t* out) {
    for (size_t i = 0; i < 8; ++i) {
        out[i] = static_cast<uint8_t>((value >> ((7 - i) * 8)) & 0xFF);
    }
}

void writeI64BE(int64_t value, uint8_t* out) {
    writeU64BE(static_cast<uint64_t>(value), out);
}

uint16_t readU16BE(const uint8_t* data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) |
                                 static_cast<uint16_t>(data[1]));
}

void writeU32BE(uint32_t value, uint8_t* out) {
    out[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    out[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(value & 0xFF);
}

void writeI32BE(int32_t value, uint8_t* out) {
    writeU32BE(static_cast<uint32_t>(value), out);
}

uint64_t readU64BE(const uint8_t* data) {
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i) {
        value = (value << 8) | static_cast<uint64_t>(data[i]);
    }
    return value;
}

int64_t readI64BE(const uint8_t* data) {
    return static_cast<int64_t>(readU64BE(data));
}

uint32_t readU32BE(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

int32_t readI32BE(const uint8_t* data) {
    return static_cast<int32_t>(readU32BE(data));
}

template <size_t PacketSize>
void writePacketHeader(Net::TcpPacketType type, std::array<uint8_t, PacketSize>& outPacket) {
    writeU16BE(static_cast<uint16_t>(PacketSize), outPacket.data());
    writeU16BE(static_cast<uint16_t>(type), outPacket.data() + Net::kTcpHeaderSize - 2);
}

bool parseExactPacketType(
    const uint8_t* data,
    size_t size,
    Net::TcpPacketType expectedType,
    Net::TcpPacketHeader& outHeader) {
    if (!Net::parseTcpPacketHeader(data, size, outHeader)) {
        return false;
    }

    return outHeader.type == expectedType;
}

bool parseRoomIdPacket(
    const uint8_t* data,
    size_t size,
    Net::TcpPacketType expectedType,
    Net::TcpPacketHeader& outHeader,
    uint32_t& outRoomId) {
    if (!parseExactPacketType(data, size, expectedType, outHeader) ||
        size != Net::kRoomIdPacketSize) {
        return false;
    }

    outRoomId = readU32BE(data + Net::kTcpHeaderSize);
    return true;
}

bool parseRoomStatusPacket(
    const uint8_t* data,
    size_t size,
    Net::TcpPacketType expectedType,
    Net::TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint16_t& outPlayerCount) {
    if (!parseExactPacketType(data, size, expectedType, outHeader) ||
        size != Net::kRoomStatusPacketSize) {
        return false;
    }

    outRoomId = readU32BE(data + Net::kTcpHeaderSize);
    outPlayerCount = readU16BE(data + Net::kTcpHeaderSize + Net::kRoomIdFieldSize);
    return true;
}

bool parseReadyRoomStatusPacket(
    const uint8_t* data,    // packet의 시작 주소.
    size_t size,            // 실제로 받은 byte 수. 포인터만 있으면 길이를 모르기 때문에 data와 size는 단짝.
    Net::TcpPacketType expectedType,
    Net::TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint16_t& outReadyPlayerCount,
    uint16_t& outTotalPlayerCount) {
    // 헤더는 정상인지, type은 맞는지, exact size가 맞는지 확인
    if (!parseExactPacketType(data, size, expectedType, outHeader) ||
        size != Net::kReadyRoomStatusPacketSize) {
        return false;
    }

    outRoomId = readU32BE(data + Net::kTcpHeaderSize);
    outReadyPlayerCount = readU16BE(data + Net::kTcpHeaderSize + Net::kRoomIdFieldSize);
    outTotalPlayerCount = readU16BE(
        data + Net::kTcpHeaderSize + Net::kRoomIdFieldSize + Net::kPlayerCountFieldSize);
    return true;
}

bool isValidSettlementId(const std::string& settlementId) {
    if (settlementId.empty() || settlementId.size() > Net::kSettlementIdMaxLength) {
        return false;
    }

    for (const unsigned char ch : settlementId) {
        if (ch < 0x20 || ch > 0x7E) {   // printable ASCII
            return false;
        }
    }

    return true;
}
}  // namespace

namespace Net {
bool serializeWelcomePacket(uint64_t sessionId, std::array<uint8_t, kWelcomePacketSize>& outPacket) {
    writePacketHeader(TcpPacketType::kWelcome, outPacket);
    writeU64BE(sessionId, outPacket.data() + kTcpHeaderSize);
    return true;
}

bool serializeCreateRoomRequestPacket(std::array<uint8_t, kTcpHeaderSize>& outPacket) {
    writePacketHeader(TcpPacketType::kCreateRoomRequest, outPacket);
    return true;
}

bool serializeCreateRoomResponsePacket(
    uint32_t roomId,
    uint16_t playerCount,
    std::array<uint8_t, kRoomStatusPacketSize>& outPacket) {
    writePacketHeader(TcpPacketType::kCreateRoomResponse, outPacket);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    writeU16BE(playerCount, outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize);
    return true;
}

bool serializeJoinRoomRequestPacket(
    uint32_t roomId,
    std::array<uint8_t, kRoomIdPacketSize>& outPacket) {
    writePacketHeader(TcpPacketType::kJoinRoomRequest, outPacket);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    return true;
}

bool serializeJoinRoomResponsePacket(
    uint32_t roomId,
    uint16_t playerCount,
    std::array<uint8_t, kRoomStatusPacketSize>& outPacket) {
    writePacketHeader(TcpPacketType::kJoinRoomResponse, outPacket);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    writeU16BE(playerCount, outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize);
    return true;
}

bool serializeLeaveRoomRequestPacket(std::array<uint8_t, kTcpHeaderSize>& outPacket) {
    writePacketHeader(TcpPacketType::kLeaveRoomRequest, outPacket);
    return true;
}

    // heady-only 함수. 클라이언트는 ready 하겠다는 의도만 보낸다.
bool serializeReadyRoomRequestPacket(std::array<uint8_t, kTcpHeaderSize>& outPacket) {
    writePacketHeader(TcpPacketType::kReadyRoomRequest, outPacket);
    return true;
}

bool serializeLeaveRoomResponsePacket(
    uint32_t roomId,
    std::array<uint8_t, kRoomIdPacketSize>& outPacket) {
    writePacketHeader(TcpPacketType::kLeaveRoomResponse, outPacket);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    return true;
}

bool serializeReadyRoomResponsePacket(
    uint32_t roomId,
    uint16_t readyPlayerCount,
    uint16_t totalPlayerCount,
    std::array<uint8_t, kReadyRoomStatusPacketSize>& outPacket) {
    writePacketHeader(TcpPacketType::kReadyRoomResponse, outPacket);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    writeU16BE(readyPlayerCount, outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize);
    writeU16BE(
        totalPlayerCount,
        outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize + kPlayerCountFieldSize);
    return true;
}

size_t clientListSnapshotPacketSize(size_t sessionCount) {
    return kTcpHeaderSize + kClientListCountFieldSize + (sessionCount * kClientListEntrySize);
}

bool serializeClientListSnapshotPacket(
    const std::vector<uint64_t>& sessionIds,
    std::vector<uint8_t>& outPacket) {
    if (sessionIds.size() > std::numeric_limits<uint16_t>::max()) {
        return false;
    }

    const size_t packetSize = clientListSnapshotPacketSize(sessionIds.size());
    if (packetSize > kMaxTcpPacketSize) {
        return false;
    }

    outPacket.assign(packetSize, 0);
    writeU16BE(static_cast<uint16_t>(packetSize), outPacket.data());
    writeU16BE(static_cast<uint16_t>(TcpPacketType::kClientListSnapshot), outPacket.data() + 2);
    writeU16BE(static_cast<uint16_t>(sessionIds.size()), outPacket.data() + kTcpHeaderSize);

    uint8_t* payload = outPacket.data() + kTcpHeaderSize + kClientListCountFieldSize;
    for (size_t i = 0; i < sessionIds.size(); ++i) {
        writeU64BE(sessionIds[i], payload + (i * kClientListEntrySize));
    }

    return true;
}

size_t roomListSnapshotPacketSize(size_t roomCount) {
    return kTcpHeaderSize + kRoomListCountFieldSize + (roomCount * kRoomEntrySize);
}

bool serializeRoomListSnapshotPacket(const std::vector<TcpRoomEntry>& rooms, std::vector<uint8_t>& outPacket) {
    if (rooms.size() > std::numeric_limits<uint16_t>::max()) {
        return false;
    }

    const size_t packetSize = roomListSnapshotPacketSize(rooms.size());
    if (packetSize > kMaxTcpPacketSize) {
        return false;
    }

    outPacket.assign(packetSize, 0);
    writeU16BE(static_cast<uint16_t>(packetSize), outPacket.data());
    writeU16BE(static_cast<uint16_t>(TcpPacketType::kRoomListSnapshot), outPacket.data() + 2);
    writeU16BE(static_cast<uint16_t>(rooms.size()), outPacket.data() + kTcpHeaderSize);

    uint8_t* payload = outPacket.data() + kTcpHeaderSize + kRoomListCountFieldSize;
    for (size_t i = 0; i < rooms.size(); ++i) {
        uint8_t* entry = payload + (i * kRoomEntrySize);
        writeU32BE(rooms[i].roomId, entry);
        writeU16BE(rooms[i].playerCount, entry + kRoomIdFieldSize);
        writeU16BE(rooms[i].maxPlayers, entry + kRoomIdFieldSize + kPlayerCountFieldSize);
    }

    return true;
}

bool serializeBattleStartPacket(
    uint32_t roomId,
    uint64_t playerASessionId,
    uint64_t playerBSessionId,
    std::array<uint8_t, kBattleStartPacketSize>& outPacket) {
    writePacketHeader(TcpPacketType::kBattleStart, outPacket);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    writeU64BE(playerASessionId, outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize);
    writeU64BE(
        playerBSessionId,
        outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize + kSessionIdFieldSize);
    return true;
}

bool serializeMonsterSpawnPacket(
    uint32_t roomId,
    uint32_t monsterId,
    uint32_t monsterTypeId,
    uint16_t maxHp,
    std::array<uint8_t, kMonsterSpawnPacketSize>& outPacket) {
    writePacketHeader(TcpPacketType::kMonsterSpawn, outPacket);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    writeU32BE(monsterId, outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize);
    writeU32BE(
        monsterTypeId,
        outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize + kMonsterIdFieldSize);
    writeU16BE(
        maxHp,
        outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize + kMonsterIdFieldSize +
            kMonsterTypeIdFieldSize);
    return true;
}

bool serializeMonsterDeathRequestPacket(
    uint32_t monsterId,
    std::array<uint8_t, kMonsterDeathRequestPacketSize>& outPacket) {
    writePacketHeader(TcpPacketType::kMonsterDeathRequest, outPacket);
    writeU32BE(monsterId, outPacket.data() + kTcpHeaderSize);
    return true;
}

bool serializeMonsterDeathPacket(
    uint32_t roomId,
    uint32_t monsterId,
    std::array<uint8_t, kMonsterDeathPacketSize>& outPacket) {
    writePacketHeader(TcpPacketType::kMonsterDeath, outPacket);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    writeU32BE(monsterId, outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize);
    return true;
}

size_t dropListSnapshotPacketSize(size_t dropCount) {
    return kTcpHeaderSize + kRoomIdFieldSize + kDropListCountFieldSize + (dropCount * kDropEntrySize);
}
// 가변 길이 스냅샷 serializer
bool serializeDropListSnapshotPacket(
    uint32_t roomId,
    const std::vector<TcpDropEntry>& drops,
    std::vector<uint8_t>& outPacket) {
    if (drops.size() > std::numeric_limits<uint16_t>::max()) {
        return false;
    }
    // drops.size()에 따라 매번 패킷 길이가 달라진다.
    const size_t packetSize = dropListSnapshotPacketSize(drops.size());
    if (packetSize > kMaxTcpPacketSize) {
        return false;
    }

    outPacket.assign(packetSize, 0);    // 정확한 크기의 버퍼를 다시 준비
    writeU16BE(static_cast<uint16_t>(packetSize), outPacket.data());
    writeU16BE(static_cast<uint16_t>(TcpPacketType::kDropListSnapshot), outPacket.data() + 2);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    writeU16BE(
        static_cast<uint16_t>(drops.size()),
        outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize);

    uint8_t* payload = outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize + kDropListCountFieldSize;
    for (size_t i = 0; i < drops.size(); ++i) {
        uint8_t* entry = payload + (i * kDropEntrySize);
        writeU32BE(drops[i].dropId, entry);
        writeU32BE(drops[i].itemId, entry + kDropIdFieldSize);
        writeU16BE(drops[i].quantity, entry + kDropIdFieldSize + kItemIdFieldSize);
    }

    return true;
}

bool serializeClickLootRequestPacket(
    uint32_t dropId,
    std::array<uint8_t, kClickLootRequestPacketSize>& outPacket) {
    writePacketHeader(TcpPacketType::kClickLootRequest, outPacket);
    writeU32BE(dropId, outPacket.data() + kTcpHeaderSize);
    return true;
}

bool serializeLootResolvedPacket(
    uint32_t roomId,
    uint32_t dropId,
    uint64_t winnerSessionId,
    uint32_t itemId,
    uint16_t quantity,
    std::array<uint8_t, kLootResolvedPacketSize>& outPacket) {
    writePacketHeader(TcpPacketType::kLootResolved, outPacket);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    writeU32BE(dropId, outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize);
    writeU64BE(
        winnerSessionId,
        outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize + kDropIdFieldSize);
    writeU32BE(
        itemId,
        outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize + kDropIdFieldSize +
            kSessionIdFieldSize);
    writeU16BE(
        quantity,
        outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize + kDropIdFieldSize +
            kSessionIdFieldSize + kItemIdFieldSize);
    return true;
}

bool serializeLootRejectedPacket(
    uint32_t roomId,
    uint32_t dropId,
    TcpLootRejectReason reason,
    std::array<uint8_t, kLootRejectedPacketSize>& outPacket) {
    writePacketHeader(TcpPacketType::kLootRejected, outPacket);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    writeU32BE(dropId, outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize);
    writeU16BE(
        static_cast<uint16_t>(reason),
        outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize + kDropIdFieldSize);
    return true;
}

size_t inventorySnapshotPacketSize(size_t entryCount) {
    return kTcpHeaderSize + kSessionIdFieldSize + (2 * kWeightFieldSize) +
           kInventoryCountFieldSize + (entryCount * kInventoryEntrySize);
}

bool serializeInventorySnapshotPacket(
    uint64_t sessionId,
    uint16_t currentWeight,
    uint16_t maxWeight,
    const std::vector<TcpInventoryEntry>& entries,
    std::vector<uint8_t>& outPacket) {
    if (entries.size() > std::numeric_limits<uint16_t>::max()) {
        return false;
    }

    const size_t packetSize = inventorySnapshotPacketSize(entries.size());
    if (packetSize > kMaxTcpPacketSize) {
        return false;
    }

    outPacket.assign(packetSize, 0);
    writeU16BE(static_cast<uint16_t>(packetSize), outPacket.data());
    writeU16BE(static_cast<uint16_t>(TcpPacketType::kInventorySnapshot), outPacket.data() + 2);
    writeU64BE(sessionId, outPacket.data() + kTcpHeaderSize);
    writeU16BE(currentWeight, outPacket.data() + kTcpHeaderSize + kSessionIdFieldSize);
    writeU16BE(
        maxWeight,
        outPacket.data() + kTcpHeaderSize + kSessionIdFieldSize + kWeightFieldSize);
    writeU16BE(
        static_cast<uint16_t>(entries.size()),
        outPacket.data() + kTcpHeaderSize + kSessionIdFieldSize + (2 * kWeightFieldSize));

    uint8_t* payload =
        outPacket.data() + kTcpHeaderSize + kSessionIdFieldSize + (2 * kWeightFieldSize) +
        kInventoryCountFieldSize;
    for (size_t i = 0; i < entries.size(); ++i) {
        uint8_t* entry = payload + (i * kInventoryEntrySize);
        writeU32BE(entries[i].itemId, entry);
        writeU16BE(entries[i].quantity, entry + kItemIdFieldSize);
    }

    return true;
}
// payload 없는 header-only
bool serializeFinishSessionRequestPacket(std::array<uint8_t, kFinishSessionRequestPacketSize>& outPacket) {
    writePacketHeader(TcpPacketType::kFinishSessionRequest, outPacket);
    return true;
}

bool serializeSmokeCreateCenterDropRequestPacket(
    std::array<uint8_t, kSmokeCreateCenterDropRequestPacketSize>& outPacket) {
    writePacketHeader(TcpPacketType::kSmokeCreateCenterDropRequest, outPacket);
    return true;
}

bool serializeSmokePlacePlayersAroundCenterDropRequestPacket(
    std::array<uint8_t, kSmokePlacePlayersAroundCenterDropRequestPacketSize>& outPacket) {
    writePacketHeader(TcpPacketType::kSmokePlacePlayersAroundCenterDropRequest, outPacket);
    return true;
}

// settlementResult는 가변 길이 패킷이라 실제 직렬화 전에 크기를 계산해야만 한다.
// settlementIdLength와 inventoryDeltaCount가 가변.
size_t settlementResultPacketSize(size_t settlementIdLength, size_t inventoryDeltaCount) {
    return kTcpHeaderSize + kSettlementResultFixedPayloadSize + settlementIdLength +
           (inventoryDeltaCount * kSettlementInventoryDeltaEntrySize);
}

bool serializeSettlementResultPacket(
    const TcpSettlementResult& settlement,
    std::vector<uint8_t>& outPacket) {
    if (!isValidSettlementId(settlement.settlementId) ||
        settlement.inventoryDeltas.size() > std::numeric_limits<uint16_t>::max()) {
        return false;
    }

    const size_t packetSize =
        settlementResultPacketSize(settlement.settlementId.size(), settlement.inventoryDeltas.size());
    if (packetSize > kMaxTcpPacketSize) {
        return false;
    }

    outPacket.assign(packetSize, 0);
    writeU16BE(static_cast<uint16_t>(packetSize), outPacket.data());
    writeU16BE(static_cast<uint16_t>(TcpPacketType::kSettlementResult), outPacket.data() + 2);

    size_t offset = kTcpHeaderSize;
    writeU16BE(static_cast<uint16_t>(settlement.settlementId.size()), outPacket.data() + offset);
    offset += kSettlementIdLengthFieldSize;
    std::copy(
        settlement.settlementId.begin(),
        settlement.settlementId.end(),
        outPacket.begin() + static_cast<std::vector<uint8_t>::difference_type>(offset));
    offset += settlement.settlementId.size();
    writeU64BE(settlement.sessionId, outPacket.data() + offset);
    offset += kSessionIdFieldSize;
    writeU64BE(settlement.accountId, outPacket.data() + offset);
    offset += kSessionIdFieldSize;
    writeU32BE(settlement.roomId, outPacket.data() + offset);
    offset += kRoomIdFieldSize;
    writeU64BE(settlement.startedAtUnixMs, outPacket.data() + offset);
    offset += kTimestampFieldSize;
    writeU64BE(settlement.finishedAtUnixMs, outPacket.data() + offset);
    offset += kTimestampFieldSize;
    writeI64BE(settlement.goldDelta, outPacket.data() + offset);
    offset += kGoldDeltaFieldSize;
    writeU16BE(static_cast<uint16_t>(settlement.reason), outPacket.data() + offset);
    offset += kSettlementReasonFieldSize;
    writeU16BE(static_cast<uint16_t>(settlement.inventoryDeltas.size()), outPacket.data() + offset);
    offset += kSettlementInventoryDeltaCountFieldSize;

    for (const TcpSettlementInventoryDelta& delta : settlement.inventoryDeltas) {
        writeU32BE(delta.itemId, outPacket.data() + offset);
        offset += kItemIdFieldSize;
        writeI32BE(delta.quantityDelta, outPacket.data() + offset);
        offset += kQuantityDeltaFieldSize;
        writeU32BE(delta.sourceDropId, outPacket.data() + offset);
        offset += kDropIdFieldSize;
    }

    return true;
}
// metaResponse 또한 settlementIdLength 때문에 가변 길이다.
size_t metaResponsePacketSize(size_t settlementIdLength) {
    return kTcpHeaderSize + kMetaResponseFixedPayloadSize + settlementIdLength;
}

bool serializeMetaResponsePacket(
    const TcpMetaResponse& response,
    std::vector<uint8_t>& outPacket) {
    if (!isValidSettlementId(response.settlementId)) {
        return false;
    }

    const size_t packetSize = metaResponsePacketSize(response.settlementId.size());
    if (packetSize > kMaxTcpPacketSize) {
        return false;
    }

    outPacket.assign(packetSize, 0);
    writeU16BE(static_cast<uint16_t>(packetSize), outPacket.data());
    writeU16BE(static_cast<uint16_t>(TcpPacketType::kMetaResponse), outPacket.data() + 2);

    size_t offset = kTcpHeaderSize;
    writeU16BE(static_cast<uint16_t>(response.op), outPacket.data() + offset);
    offset += kMetaResponseOpFieldSize;
    writeU16BE(static_cast<uint16_t>(response.settlementId.size()), outPacket.data() + offset);
    offset += kSettlementIdLengthFieldSize;
    std::copy(
        response.settlementId.begin(),
        response.settlementId.end(),
        outPacket.begin() + static_cast<std::vector<uint8_t>::difference_type>(offset));
    offset += response.settlementId.size();
    writeU16BE(static_cast<uint16_t>(response.status), outPacket.data() + offset);
    offset += kMetaResponseStatusFieldSize;
    writeU32BE(response.retryAfterMs, outPacket.data() + offset);
    return true;
}

bool serializeErrorPacket(
    TcpPacketType failedType,
    TcpErrorCode errorCode,
    std::array<uint8_t, kErrorPacketSize>& outPacket) {
    writePacketHeader(TcpPacketType::kError, outPacket);
    writeU16BE(static_cast<uint16_t>(failedType), outPacket.data() + kTcpHeaderSize);
    writeU16BE(static_cast<uint16_t>(errorCode), outPacket.data() + kTcpHeaderSize + 2);
    return true;
}

bool peekTcpPacketHeader(const uint8_t* data, size_t size, TcpPacketHeader& outHeader) {
    if (data == nullptr || size < kTcpHeaderSize) {
        return false;
    }

    outHeader.size = readU16BE(data);
    outHeader.type = static_cast<TcpPacketType>(readU16BE(data + 2));
    return true;
}

bool parseTcpPacketHeader(const uint8_t* data, size_t size, TcpPacketHeader& outHeader) {
    if (!peekTcpPacketHeader(data, size, outHeader)) {
        return false;
    }

    if (outHeader.size < kTcpHeaderSize || outHeader.size > kMaxTcpPacketSize) {
        return false;
    }

    return outHeader.size == size;
}

bool parseWelcomePacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint64_t& outSessionId) {
    if (!parseTcpPacketHeader(data, size, outHeader)) {
        return false;
    }

    if (outHeader.type != TcpPacketType::kWelcome || size != kWelcomePacketSize) {
        return false;
    }

    outSessionId = readU64BE(data + kTcpHeaderSize);
    return true;
}

bool parseCreateRoomRequestPacket(const uint8_t* data, size_t size, TcpPacketHeader& outHeader) {
    return parseExactPacketType(data, size, TcpPacketType::kCreateRoomRequest, outHeader) &&
           size == kTcpHeaderSize;
}

bool parseCreateRoomResponsePacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint16_t& outPlayerCount) {
    return parseRoomStatusPacket(
        data,
        size,
        TcpPacketType::kCreateRoomResponse,
        outHeader,
        outRoomId,
        outPlayerCount);
}

bool parseJoinRoomRequestPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId) {
    return parseRoomIdPacket(data, size, TcpPacketType::kJoinRoomRequest, outHeader, outRoomId);
}

bool parseJoinRoomResponsePacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint16_t& outPlayerCount) {
    return parseRoomStatusPacket(
        data,
        size,
        TcpPacketType::kJoinRoomResponse,
        outHeader,
        outRoomId,
        outPlayerCount);
}

bool parseLeaveRoomRequestPacket(const uint8_t* data, size_t size, TcpPacketHeader& outHeader) {
    return parseExactPacketType(data, size, TcpPacketType::kLeaveRoomRequest, outHeader) &&
           size == kTcpHeaderSize;
}

bool parseReadyRoomRequestPacket(const uint8_t* data, size_t size, TcpPacketHeader& outHeader) {
    return parseExactPacketType(data, size, TcpPacketType::kReadyRoomRequest, outHeader) &&
           size == kTcpHeaderSize;
}

bool parseLeaveRoomResponsePacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId) {
    return parseRoomIdPacket(data, size, TcpPacketType::kLeaveRoomResponse, outHeader, outRoomId);
}

bool parseReadyRoomResponsePacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint16_t& outReadyPlayerCount,
    uint16_t& outTotalPlayerCount) {
    return parseReadyRoomStatusPacket(
        data,
        size,
        TcpPacketType::kReadyRoomResponse,
        outHeader,
        outRoomId,
        outReadyPlayerCount,
        outTotalPlayerCount);
}

bool parseClientListSnapshotPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    std::vector<uint64_t>& outSessionIds) {
    outSessionIds.clear();
    if (!parseTcpPacketHeader(data, size, outHeader)) {
        return false;
    }

    if (outHeader.type != TcpPacketType::kClientListSnapshot ||
        size < kTcpHeaderSize + kClientListCountFieldSize) {
        return false;
    }

    const uint16_t count = readU16BE(data + kTcpHeaderSize);
    const size_t expectedSize = clientListSnapshotPacketSize(count);
    if (expectedSize != size) {
        return false;
    }

    outSessionIds.reserve(count);
    const uint8_t* payload = data + kTcpHeaderSize + kClientListCountFieldSize;
    for (uint16_t i = 0; i < count; ++i) {
        outSessionIds.push_back(readU64BE(payload + (static_cast<size_t>(i) * kClientListEntrySize)));
    }

    return true;
}

bool parseRoomListSnapshotPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    std::vector<TcpRoomEntry>& outRooms) {
    outRooms.clear();
    if (!parseTcpPacketHeader(data, size, outHeader)) {
        return false;
    }

    if (outHeader.type != TcpPacketType::kRoomListSnapshot ||
        size < kTcpHeaderSize + kRoomListCountFieldSize) {
        return false;
    }

    const uint16_t count = readU16BE(data + kTcpHeaderSize);
    const size_t expectedSize = roomListSnapshotPacketSize(count);
    if (expectedSize != size) {
        return false;
    }

    outRooms.reserve(count);
    const uint8_t* payload = data + kTcpHeaderSize + kRoomListCountFieldSize;
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* entry = payload + (static_cast<size_t>(i) * kRoomEntrySize);
        outRooms.push_back(TcpRoomEntry{
            readU32BE(entry),
            readU16BE(entry + kRoomIdFieldSize),
            readU16BE(entry + kRoomIdFieldSize + kPlayerCountFieldSize),
        });
    }

    return true;
}

bool parseBattleStartPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint64_t& outPlayerASessionId,
    uint64_t& outPlayerBSessionId) {
    // type과 size를 체크하는 exact parser
    if (!parseExactPacketType(data, size, TcpPacketType::kBattleStart, outHeader) ||
        size != kBattleStartPacketSize) {
        return false;
    }

    outRoomId = readU32BE(data + kTcpHeaderSize);
    outPlayerASessionId = readU64BE(data + kTcpHeaderSize + kRoomIdFieldSize);
    outPlayerBSessionId = readU64BE(
        data + kTcpHeaderSize + kRoomIdFieldSize + kSessionIdFieldSize);
    return true;
}

bool parseMonsterSpawnPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint32_t& outMonsterId,
    uint32_t& outMonsterTypeId,
    uint16_t& outMaxHp) {
    if (!parseExactPacketType(data, size, TcpPacketType::kMonsterSpawn, outHeader) ||
        size != kMonsterSpawnPacketSize) {
        return false;
    }

    outRoomId = readU32BE(data + kTcpHeaderSize);
    outMonsterId = readU32BE(data + kTcpHeaderSize + kRoomIdFieldSize);
    outMonsterTypeId = readU32BE(data + kTcpHeaderSize + kRoomIdFieldSize + kMonsterIdFieldSize);
    outMaxHp = readU16BE(
        data + kTcpHeaderSize + kRoomIdFieldSize + kMonsterIdFieldSize + kMonsterTypeIdFieldSize);
    return true;
}

bool parseMonsterDeathRequestPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outMonsterId) {
    if (!parseExactPacketType(data, size, TcpPacketType::kMonsterDeathRequest, outHeader) ||
        size != kMonsterDeathRequestPacketSize) {
        return false;
    }

    outMonsterId = readU32BE(data + kTcpHeaderSize);
    return true;
}

bool parseMonsterDeathPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint32_t& outMonsterId) {
    if (!parseExactPacketType(data, size, TcpPacketType::kMonsterDeath, outHeader) ||
        size != kMonsterDeathPacketSize) {
        return false;
    }

    outRoomId = readU32BE(data + kTcpHeaderSize);
    outMonsterId = readU32BE(data + kTcpHeaderSize + kRoomIdFieldSize);
    return true;
}

bool parseDropListSnapshotPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    std::vector<TcpDropEntry>& outDrops) {
    outDrops.clear();
    if (!parseTcpPacketHeader(data, size, outHeader)) {
        return false;
    }

    if (outHeader.type != TcpPacketType::kDropListSnapshot ||
        size < kTcpHeaderSize + kRoomIdFieldSize + kDropListCountFieldSize) {
        return false;
    }

    outRoomId = readU32BE(data + kTcpHeaderSize);
    const uint16_t count = readU16BE(data + kTcpHeaderSize + kRoomIdFieldSize);
    const size_t expectedSize = dropListSnapshotPacketSize(count);
    if (expectedSize != size) {
        return false;
    }

    outDrops.reserve(count);
    const uint8_t* payload = data + kTcpHeaderSize + kRoomIdFieldSize + kDropListCountFieldSize;
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* entry = payload + (static_cast<size_t>(i) * kDropEntrySize);
        outDrops.push_back(TcpDropEntry{
            readU32BE(entry),
            readU32BE(entry + kDropIdFieldSize),
            readU16BE(entry + kDropIdFieldSize + kItemIdFieldSize),
        });
    }

    return true;
}

bool parseClickLootRequestPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outDropId) {
    if (!parseExactPacketType(data, size, TcpPacketType::kClickLootRequest, outHeader) ||
        size != kClickLootRequestPacketSize) {
        return false;
    }

    outDropId = readU32BE(data + kTcpHeaderSize);
    return true;
}

bool parseLootResolvedPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint32_t& outDropId,
    uint64_t& outWinnerSessionId,
    uint32_t& outItemId,
    uint16_t& outQuantity) {
    if (!parseExactPacketType(data, size, TcpPacketType::kLootResolved, outHeader) ||
        size != kLootResolvedPacketSize) {
        return false;
    }

    outRoomId = readU32BE(data + kTcpHeaderSize);
    outDropId = readU32BE(data + kTcpHeaderSize + kRoomIdFieldSize);
    outWinnerSessionId =
        readU64BE(data + kTcpHeaderSize + kRoomIdFieldSize + kDropIdFieldSize);
    outItemId = readU32BE(
        data + kTcpHeaderSize + kRoomIdFieldSize + kDropIdFieldSize + kSessionIdFieldSize);
    outQuantity = readU16BE(
        data + kTcpHeaderSize + kRoomIdFieldSize + kDropIdFieldSize + kSessionIdFieldSize +
        kItemIdFieldSize);
    return true;
}

bool parseLootRejectedPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint32_t& outDropId,
    TcpLootRejectReason& outReason) {
    if (!parseExactPacketType(data, size, TcpPacketType::kLootRejected, outHeader) ||
        size != kLootRejectedPacketSize) {
        return false;
    }

    outRoomId = readU32BE(data + kTcpHeaderSize);
    outDropId = readU32BE(data + kTcpHeaderSize + kRoomIdFieldSize);
    outReason = static_cast<TcpLootRejectReason>(
        readU16BE(data + kTcpHeaderSize + kRoomIdFieldSize + kDropIdFieldSize));
    return true;
}

bool parseInventorySnapshotPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint64_t& outSessionId,
    uint16_t& outCurrentWeight,
    uint16_t& outMaxWeight,
    std::vector<TcpInventoryEntry>& outEntries) {
    outEntries.clear();
    if (!parseTcpPacketHeader(data, size, outHeader)) {
        return false;
    }

    if (outHeader.type != TcpPacketType::kInventorySnapshot ||
        size < kTcpHeaderSize + kSessionIdFieldSize + (2 * kWeightFieldSize) +
                   kInventoryCountFieldSize) {
        return false;
    }

    outSessionId = readU64BE(data + kTcpHeaderSize);
    outCurrentWeight = readU16BE(data + kTcpHeaderSize + kSessionIdFieldSize);
    outMaxWeight = readU16BE(data + kTcpHeaderSize + kSessionIdFieldSize + kWeightFieldSize);
    const uint16_t count =
        readU16BE(data + kTcpHeaderSize + kSessionIdFieldSize + (2 * kWeightFieldSize));
    const size_t expectedSize = inventorySnapshotPacketSize(count);
    if (expectedSize != size) {
        return false;
    }

    outEntries.reserve(count);
    const uint8_t* payload =
        data + kTcpHeaderSize + kSessionIdFieldSize + (2 * kWeightFieldSize) +
        kInventoryCountFieldSize;
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* entry = payload + (static_cast<size_t>(i) * kInventoryEntrySize);
        outEntries.push_back(TcpInventoryEntry{
            readU32BE(entry),
            readU16BE(entry + kItemIdFieldSize),
        });
    }

    return true;
}

bool parseFinishSessionRequestPacket(const uint8_t* data, size_t size, TcpPacketHeader& outHeader) {
    return parseExactPacketType(data, size, TcpPacketType::kFinishSessionRequest, outHeader) &&
           size == kFinishSessionRequestPacketSize;
}

bool parseSmokeCreateCenterDropRequestPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader) {
    return parseExactPacketType(data, size, TcpPacketType::kSmokeCreateCenterDropRequest, outHeader) &&
           size == kSmokeCreateCenterDropRequestPacketSize;
}

bool parseSmokePlacePlayersAroundCenterDropRequestPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader) {
    return parseExactPacketType(
               data,
               size,
               TcpPacketType::kSmokePlacePlayersAroundCenterDropRequest,
               outHeader) &&
           size == kSmokePlacePlayersAroundCenterDropRequestPacketSize;
}

bool parseSettlementResultPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    TcpSettlementResult& outSettlement) {
    outSettlement = TcpSettlementResult{};  // 출력 구조체를 초기화. 파싱 실패 시 이전 값이 남지 않도록.
    if (!parseTcpPacketHeader(data, size, outHeader)) {
        return false;
    }

    if (outHeader.type != TcpPacketType::kSettlementResult ||
        size < kTcpHeaderSize + kSettlementResultFixedPayloadSize) {
        return false;
    }

    size_t offset = kTcpHeaderSize;
    const uint16_t settlementIdLength = readU16BE(data + offset);
    offset += kSettlementIdLengthFieldSize;
    if (offset + settlementIdLength > size) {
        return false;
    }

    std::string settlementId(
        reinterpret_cast<const char*>(data + offset),
        reinterpret_cast<const char*>(data + offset + settlementIdLength));
    if (!isValidSettlementId(settlementId)) {
        return false;
    }
    offset += settlementIdLength;
    // settlementId 가변 길이 필드를 읽은 뒤, 남은 고정 길이 필드들이 패킷 안에 온전히 들어있는지 확인하는 방어 코드.
    const size_t fixedRemainderSize = kSettlementResultFixedPayloadSize - kSettlementIdLengthFieldSize;
    if (offset + fixedRemainderSize > size) {
        return false;
    }

    outSettlement.settlementId = std::move(settlementId);
    outSettlement.sessionId = readU64BE(data + offset);
    offset += kSessionIdFieldSize;
    outSettlement.accountId = readU64BE(data + offset);
    offset += kSessionIdFieldSize;
    outSettlement.roomId = readU32BE(data + offset);
    offset += kRoomIdFieldSize;
    outSettlement.startedAtUnixMs = readU64BE(data + offset);
    offset += kTimestampFieldSize;
    outSettlement.finishedAtUnixMs = readU64BE(data + offset);
    offset += kTimestampFieldSize;
    outSettlement.goldDelta = readI64BE(data + offset);
    offset += kGoldDeltaFieldSize;
    outSettlement.reason = static_cast<TcpSettlementReason>(readU16BE(data + offset));
    offset += kSettlementReasonFieldSize;
    const uint16_t inventoryDeltaCount = readU16BE(data + offset);
    offset += kSettlementInventoryDeltaCountFieldSize;

    const size_t expectedSize = settlementResultPacketSize(settlementIdLength, inventoryDeltaCount);
    if (expectedSize != size) {
        return false;
    }

    outSettlement.inventoryDeltas.reserve(inventoryDeltaCount);
    for (uint16_t i = 0; i < inventoryDeltaCount; ++i) {
        TcpSettlementInventoryDelta delta;
        delta.itemId = readU32BE(data + offset);
        offset += kItemIdFieldSize;
        delta.quantityDelta = readI32BE(data + offset);
        offset += kQuantityDeltaFieldSize;
        delta.sourceDropId = readU32BE(data + offset);
        offset += kDropIdFieldSize;
        outSettlement.inventoryDeltas.push_back(delta);
    }

    return true;
}
// inventory delta count가 없어서 settlementResult 보다는 단순하다.
bool parseMetaResponsePacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    TcpMetaResponse& outResponse) {
    outResponse = TcpMetaResponse{};
    if (!parseTcpPacketHeader(data, size, outHeader)) {
        return false;
    }

    if (outHeader.type != TcpPacketType::kMetaResponse ||
        size < kTcpHeaderSize + kMetaResponseFixedPayloadSize) {
        return false;
    }

    size_t offset = kTcpHeaderSize;
    outResponse.op = static_cast<TcpMetaResponseOp>(readU16BE(data + offset));
    offset += kMetaResponseOpFieldSize;
    const uint16_t settlementIdLength = readU16BE(data + offset);
    offset += kSettlementIdLengthFieldSize;
    const size_t expectedSize = metaResponsePacketSize(settlementIdLength);
    if (expectedSize != size || offset + settlementIdLength > size) {
        return false;
    }

    std::string settlementId(
        reinterpret_cast<const char*>(data + offset),
        reinterpret_cast<const char*>(data + offset + settlementIdLength));
    if (!isValidSettlementId(settlementId)) {
        return false;
    }
    outResponse.settlementId = std::move(settlementId);
    offset += settlementIdLength;
    outResponse.status = static_cast<TcpMetaResponseStatus>(readU16BE(data + offset));
    offset += kMetaResponseStatusFieldSize;
    outResponse.retryAfterMs = readU32BE(data + offset);
    return true;
}

bool parseErrorPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    TcpPacketType& outFailedType,
    TcpErrorCode& outErrorCode) {
    if (!parseExactPacketType(data, size, TcpPacketType::kError, outHeader) ||
        size != kErrorPacketSize) {
        return false;
    }

    outFailedType = static_cast<TcpPacketType>(readU16BE(data + kTcpHeaderSize));
    outErrorCode = static_cast<TcpErrorCode>(readU16BE(data + kTcpHeaderSize + 2));
    return true;
}
}  // namespace Net
