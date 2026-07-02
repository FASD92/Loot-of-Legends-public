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

bool isValidBattleStartRoster(uint32_t roomId, const std::vector<uint64_t>& playerSessionIds) {
    if (roomId == 0 ||
        playerSessionIds.size() < Net::kBattleStartRosterMinPlayers ||
        playerSessionIds.size() > Net::kBattleStartRosterMaxPlayers) {
        return false;
    }

    for (size_t i = 0; i < playerSessionIds.size(); ++i) {
        if (playerSessionIds[i] == 0) {
            return false;
        }
        for (size_t j = i + 1; j < playerSessionIds.size(); ++j) {
            if (playerSessionIds[i] == playerSessionIds[j]) {
                return false;
            }
        }
    }

    return true;
}

bool isValidBattleLoadEntry(
    uint32_t roomId,
    uint64_t battleInstanceId,
    const std::vector<uint64_t>& playerSessionIds) {
    if (roomId == 0 ||
        battleInstanceId == 0 ||
        playerSessionIds.size() < Net::kBattleLoadEntryMinPlayers ||
        playerSessionIds.size() > Net::kBattleLoadEntryMaxPlayers) {
        return false;
    }

    for (size_t i = 0; i < playerSessionIds.size(); ++i) {
        if (playerSessionIds[i] == 0) {
            return false;
        }
        for (size_t j = i + 1; j < playerSessionIds.size(); ++j) {
            if (playerSessionIds[i] == playerSessionIds[j]) {
                return false;
            }
        }
    }

    return true;
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

bool parseRoomBattleInstancePacket(
    const uint8_t* data,
    size_t size,
    Net::TcpPacketType expectedType,
    size_t expectedSize,
    Net::TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint64_t& outBattleInstanceId) {
    outRoomId = 0;
    outBattleInstanceId = 0;
    if (!parseExactPacketType(data, size, expectedType, outHeader) ||
        size != expectedSize) {
        return false;
    }

    const uint32_t roomId = readU32BE(data + Net::kTcpHeaderSize);
    const uint64_t battleInstanceId =
        readU64BE(data + Net::kTcpHeaderSize + Net::kRoomIdFieldSize);
    if (roomId == 0 || battleInstanceId == 0) {
        return false;
    }

    outRoomId = roomId;
    outBattleInstanceId = battleInstanceId;
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

bool isPrintableAsciiText(const std::string& value) {
    for (const unsigned char ch : value) {
        if (ch < 0x20 || ch > 0x7E) {
            return false;
        }
    }

    return true;
}

bool decodeNextUtf8CodePoint(std::string_view value, size_t& offset, uint32_t& outCodePoint) {
    const unsigned char first = static_cast<unsigned char>(value[offset]);
    if (first <= 0x7F) {
        outCodePoint = first;
        ++offset;
        return true;
    }

    size_t length = 0;
    uint32_t codePoint = 0;
    if (first >= 0xC2 && first <= 0xDF) {
        length = 2;
        codePoint = first & 0x1F;
    } else if (first >= 0xE0 && first <= 0xEF) {
        length = 3;
        codePoint = first & 0x0F;
    } else if (first >= 0xF0 && first <= 0xF4) {
        length = 4;
        codePoint = first & 0x07;
    } else {
        return false;
    }

    if (offset + length > value.size()) {
        return false;
    }

    for (size_t i = 1; i < length; ++i) {
        const unsigned char ch = static_cast<unsigned char>(value[offset + i]);
        if ((ch & 0xC0) != 0x80) {
            return false;
        }
        codePoint = (codePoint << 6) | (ch & 0x3F);
    }

    if ((length == 3 && codePoint < 0x800) ||
        (length == 4 && codePoint < 0x10000) ||
        codePoint > 0x10FFFF ||
        (codePoint >= 0xD800 && codePoint <= 0xDFFF)) {
        return false;
    }

    offset += length;
    outCodePoint = codePoint;
    return true;
}

bool isDisallowedControlCodePoint(uint32_t codePoint) {
    return codePoint < 0x20 || (codePoint >= 0x7F && codePoint <= 0x9F);
}

bool isWhitespaceCodePoint(uint32_t codePoint) {
    return codePoint == 0x20 || codePoint == 0x09 || codePoint == 0x0A ||
           codePoint == 0x0D || codePoint == 0x0B || codePoint == 0x0C ||
           codePoint == 0x85 || codePoint == 0xA0 || codePoint == 0x1680 ||
           (codePoint >= 0x2000 && codePoint <= 0x200A) ||
           codePoint == 0x2028 || codePoint == 0x2029 ||
           codePoint == 0x202F || codePoint == 0x205F ||
           codePoint == 0x3000;
}

bool isValidRoomTitleUtf8(std::string_view roomTitle) {
    if (roomTitle.empty() || roomTitle.size() > Net::kRoomTitleMaxLength) {
        return false;
    }

    size_t offset = 0;
    size_t visibleCharacters = 0;
    bool previousWhitespace = false;
    bool firstCharacter = true;
    while (offset < roomTitle.size()) {
        uint32_t codePoint = 0;
        if (!decodeNextUtf8CodePoint(roomTitle, offset, codePoint) ||
            isDisallowedControlCodePoint(codePoint)) {
            return false;
        }

        const bool whitespace = isWhitespaceCodePoint(codePoint);
        if ((firstCharacter && whitespace) || (previousWhitespace && whitespace)) {
            return false;
        }

        previousWhitespace = whitespace;
        firstCharacter = false;
        ++visibleCharacters;
        if (visibleCharacters > Net::kRoomTitleMaxVisibleCharacters) {
            return false;
        }
    }

    return visibleCharacters > 0 && !previousWhitespace;
}

bool isValidCreateRoomCapacity(uint8_t maxPlayers) {
    return maxPlayers >= Net::kCreateRoomMinCapacity &&
           maxPlayers <= Net::kCreateRoomMaxCapacity;
}

bool isValidRoomTitle(const std::string& roomTitle) {
    return isValidRoomTitleUtf8(roomTitle);
}

bool isValidNickname(const std::string& nickname) {
    return !nickname.empty() &&
           nickname.size() <= Net::kNicknameMaxLength &&
           isPrintableAsciiText(nickname);
}

bool isValidRoomStatus(Net::TcpRoomStatus status) {
    return status == Net::TcpRoomStatus::kOpen ||
           status == Net::TcpRoomStatus::kInProgress;
}

bool isValidRoomStatusValue(uint8_t value) {
    return value == static_cast<uint8_t>(Net::TcpRoomStatus::kOpen) ||
           value == static_cast<uint8_t>(Net::TcpRoomStatus::kInProgress);
}

bool isValidLobbyReturnReason(Net::TcpLobbyReturnReason reason) {
    return reason == Net::TcpLobbyReturnReason::kNone ||
           reason == Net::TcpLobbyReturnReason::kArenaLoadTimeout ||
           reason == Net::TcpLobbyReturnReason::kArenaLoadMinimumFailure ||
           reason == Net::TcpLobbyReturnReason::kHostKick ||
           reason == Net::TcpLobbyReturnReason::kResultGenerationFailure;
}

bool isValidLobbyReturnReasonValue(uint8_t value) {
    return value == static_cast<uint8_t>(Net::TcpLobbyReturnReason::kNone) ||
           value == static_cast<uint8_t>(Net::TcpLobbyReturnReason::kArenaLoadTimeout) ||
           value == static_cast<uint8_t>(Net::TcpLobbyReturnReason::kArenaLoadMinimumFailure) ||
           value == static_cast<uint8_t>(Net::TcpLobbyReturnReason::kHostKick) ||
           value == static_cast<uint8_t>(Net::TcpLobbyReturnReason::kResultGenerationFailure);
}

bool isValidReadyStateValue(uint8_t value) {
    return value == 0 || value == 1;
}

bool containsSessionId(const std::vector<uint64_t>& sessionIds, uint64_t sessionId) {
    return std::find(sessionIds.begin(), sessionIds.end(), sessionId) != sessionIds.end();
}

bool isValidBattleFinalRanking(
    uint32_t roomId,
    uint64_t battleInstanceId,
    const std::vector<Net::BattleFinalRankingEntry>& rankings) {
    if (roomId == 0 ||
        battleInstanceId == 0 ||
        rankings.size() < Net::kBattleFinalRankingMinRows ||
        rankings.size() > Net::kBattleFinalRankingMaxRows) {
        return false;
    }

    std::vector<uint64_t> sessionIds;
    sessionIds.reserve(rankings.size());
    for (const Net::BattleFinalRankingEntry& ranking : rankings) {
        if (ranking.rank == 0 ||
            ranking.sessionId == 0 ||
            ranking.totalAssetValue < 0 ||
            !isValidNickname(ranking.nickname) ||
            containsSessionId(sessionIds, ranking.sessionId)) {
            return false;
        }
        sessionIds.push_back(ranking.sessionId);
    }

    return true;
}

bool isValidRoomDetailState(const Net::TcpRoomDetailState& detail) {
    if (detail.roomId == 0 ||
        !isValidRoomStatus(detail.roomStatus) ||
        !isValidRoomTitle(detail.roomTitle) ||
        !isValidCreateRoomCapacity(detail.maxPlayers) ||
        detail.members.empty() ||
        detail.members.size() > Net::kRoomDetailMaxMembers ||
        detail.members.size() > detail.maxPlayers ||
        detail.targetActions.size() > std::numeric_limits<uint8_t>::max() ||
        (detail.selfActionMask & ~Net::kTcpRoomActionMaskAll) != 0) {
        return false;
    }

    std::vector<uint64_t> memberIds;
    memberIds.reserve(detail.members.size());
    for (const Net::TcpRoomMemberEntry& member : detail.members) {
        if (member.sessionId == 0 ||
            member.sessionId > std::numeric_limits<uint32_t>::max() ||
            !isValidNickname(member.nickname) ||
            containsSessionId(memberIds, member.sessionId)) {
            return false;
        }
        memberIds.push_back(member.sessionId);
    }

    std::vector<uint64_t> targetIds;
    targetIds.reserve(detail.targetActions.size());
    for (const Net::TcpTargetActionEntry& action : detail.targetActions) {
        if (action.targetSessionId == 0 ||
            action.targetSessionId > std::numeric_limits<uint32_t>::max() ||
            action.targetActionMask == 0 ||
            (action.targetActionMask & ~Net::kTcpTargetActionMaskAll) != 0 ||
            !containsSessionId(memberIds, action.targetSessionId) ||
            containsSessionId(targetIds, action.targetSessionId)) {
            return false;
        }
        targetIds.push_back(action.targetSessionId);
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

bool serializeAuthenticateGameSessionPacket(std::string_view token, std::vector<uint8_t>& outPacket) {
    outPacket.clear();
    if (token.empty() || token.size() > kGameSessionTokenMaxLength) {
        return false;
    }

    const size_t packetSize =
        kTcpHeaderSize + kGameSessionTokenLengthFieldSize + token.size();
    if (packetSize > kMaxTcpPacketSize) {
        return false;
    }

    outPacket.assign(packetSize, 0);
    writeU16BE(static_cast<uint16_t>(packetSize), outPacket.data());
    writeU16BE(
        static_cast<uint16_t>(TcpPacketType::kAuthenticateGameSession),
        outPacket.data() + 2);
    writeU16BE(static_cast<uint16_t>(token.size()), outPacket.data() + kTcpHeaderSize);
    std::copy(
        token.begin(),
        token.end(),
        outPacket.begin() + static_cast<std::vector<uint8_t>::difference_type>(
                                kTcpHeaderSize + kGameSessionTokenLengthFieldSize));
    return true;
}

bool serializeSessionReplacedPacket(std::array<uint8_t, kTcpHeaderSize>& outPacket) {
    writePacketHeader(TcpPacketType::kSessionReplaced, outPacket);
    return true;
}

bool serializeHeartbeatRequestPacket(std::array<uint8_t, kTcpHeaderSize>& outPacket) {
    writePacketHeader(TcpPacketType::kHeartbeatRequest, outPacket);
    return true;
}

bool serializeCreateRoomRequestPacket(
    std::string_view roomTitle,
    uint8_t maxPlayers,
    std::vector<uint8_t>& outPacket) {
    outPacket.clear();
    if (!isValidRoomTitleUtf8(roomTitle) || !isValidCreateRoomCapacity(maxPlayers)) {
        return false;
    }

    const size_t packetSize =
        kTcpHeaderSize + kRoomTitleLengthFieldSize + roomTitle.size() +
        kCreateRoomCapacityFieldSize;
    if (packetSize > kMaxTcpPacketSize) {
        return false;
    }

    outPacket.assign(packetSize, 0);
    writeU16BE(static_cast<uint16_t>(packetSize), outPacket.data());
    writeU16BE(
        static_cast<uint16_t>(TcpPacketType::kCreateRoomRequest),
        outPacket.data() + 2);
    outPacket[kTcpHeaderSize] = static_cast<uint8_t>(roomTitle.size());
    std::copy(
        roomTitle.begin(),
        roomTitle.end(),
        outPacket.begin() + static_cast<std::vector<uint8_t>::difference_type>(
                                kTcpHeaderSize + kRoomTitleLengthFieldSize));
    outPacket.back() = maxPlayers;
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

bool serializeUnreadyRoomRequestPacket(std::array<uint8_t, kTcpHeaderSize>& outPacket) {
    writePacketHeader(TcpPacketType::kUnreadyRoomRequest, outPacket);
    return true;
}

bool serializeHostStartBattleRequestPacket(std::array<uint8_t, kTcpHeaderSize>& outPacket) {
    writePacketHeader(TcpPacketType::kHostStartBattleRequest, outPacket);
    return true;
}

bool serializeLeaveRoomResponsePacket(
    uint32_t roomId,
    std::array<uint8_t, kRoomIdPacketSize>& outPacket) {
    if (roomId == 0) {
        return false;
    }

    writePacketHeader(TcpPacketType::kLeaveRoomResponse, outPacket);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    return true;
}

bool serializeUnreadyRoomResponsePacket(
    uint32_t roomId,
    std::array<uint8_t, kRoomIdPacketSize>& outPacket) {
    if (roomId == 0) {
        return false;
    }

    writePacketHeader(TcpPacketType::kUnreadyRoomResponse, outPacket);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    return true;
}

bool serializeHostStartBattleResponsePacket(
    uint32_t roomId,
    std::array<uint8_t, kRoomIdPacketSize>& outPacket) {
    if (roomId == 0) {
        return false;
    }

    writePacketHeader(TcpPacketType::kHostStartBattleResponse, outPacket);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    return true;
}

bool serializeReadyRoomResponsePacket(
    uint32_t roomId,
    uint16_t readyPlayerCount,
    uint16_t totalPlayerCount,
    std::array<uint8_t, kReadyRoomStatusPacketSize>& outPacket) {
    if (roomId == 0) {
        return false;
    }

    writePacketHeader(TcpPacketType::kReadyRoomResponse, outPacket);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    writeU16BE(readyPlayerCount, outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize);
    writeU16BE(
        totalPlayerCount,
        outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize + kPlayerCountFieldSize);
    return true;
}

bool serializeHostKickRequestPacket(
    uint32_t targetSessionId,
    std::array<uint8_t, kHostKickRequestPacketSize>& outPacket) {
    if (targetSessionId == 0) {
        return false;
    }

    writePacketHeader(TcpPacketType::kHostKickRequest, outPacket);
    writeU32BE(targetSessionId, outPacket.data() + kTcpHeaderSize);
    return true;
}

bool serializeHostKickResponsePacket(
    uint32_t roomId,
    uint32_t targetSessionId,
    std::array<uint8_t, kHostKickResponsePacketSize>& outPacket) {
    if (roomId == 0 || targetSessionId == 0) {
        return false;
    }

    writePacketHeader(TcpPacketType::kHostKickResponse, outPacket);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    writeU32BE(targetSessionId, outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize);
    return true;
}

bool serializeLobbyReturnVisibilityPacket(
    uint32_t previousRoomId,
    TcpLobbyReturnReason reason,
    std::array<uint8_t, kLobbyReturnVisibilityPacketSize>& outPacket) {
    if (previousRoomId == 0 || !isValidLobbyReturnReason(reason)) {
        return false;
    }

    writePacketHeader(TcpPacketType::kLobbyReturnVisibility, outPacket);
    writeU32BE(previousRoomId, outPacket.data() + kTcpHeaderSize);
    outPacket[kTcpHeaderSize + kRoomIdFieldSize] = static_cast<uint8_t>(reason);
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
    return kTcpHeaderSize + kRoomListCountFieldSize + (roomCount * kRoomListEntryMinSize);
}

size_t roomListSnapshotPacketSize(const std::vector<TcpRoomEntry>& rooms) {
    size_t packetSize = kTcpHeaderSize + kRoomListCountFieldSize;
    for (const TcpRoomEntry& room : rooms) {
        packetSize += kRoomListEntryMinSize + room.title.size();
    }
    return packetSize;
}

bool serializeRoomListSnapshotPacket(const std::vector<TcpRoomEntry>& rooms, std::vector<uint8_t>& outPacket) {
    if (rooms.size() > std::numeric_limits<uint16_t>::max()) {
        return false;
    }
    for (const TcpRoomEntry& room : rooms) {
        if (!isValidRoomStatus(room.roomStatus) ||
            room.title.size() > kRoomTitleMaxLength ||
            (!room.title.empty() && !isValidRoomTitleUtf8(room.title))) {
            return false;
        }
    }

    const size_t packetSize = roomListSnapshotPacketSize(rooms);
    if (packetSize > kMaxTcpPacketSize) {
        return false;
    }

    outPacket.assign(packetSize, 0);
    writeU16BE(static_cast<uint16_t>(packetSize), outPacket.data());
    writeU16BE(static_cast<uint16_t>(TcpPacketType::kRoomListSnapshot), outPacket.data() + 2);
    writeU16BE(static_cast<uint16_t>(rooms.size()), outPacket.data() + kTcpHeaderSize);

    size_t offset = kTcpHeaderSize + kRoomListCountFieldSize;
    for (size_t i = 0; i < rooms.size(); ++i) {
        uint8_t* entry = outPacket.data() + offset;
        writeU32BE(rooms[i].roomId, entry);
        writeU16BE(rooms[i].playerCount, entry + kRoomIdFieldSize);
        writeU16BE(rooms[i].maxPlayers, entry + kRoomIdFieldSize + kPlayerCountFieldSize);
        entry[kRoomIdFieldSize + (2 * kPlayerCountFieldSize)] =
            static_cast<uint8_t>(rooms[i].roomStatus);
        offset += kRoomEntrySize;

        outPacket[offset] = static_cast<uint8_t>(rooms[i].title.size());
        ++offset;
        std::copy(
            rooms[i].title.begin(),
            rooms[i].title.end(),
            outPacket.begin() + static_cast<std::vector<uint8_t>::difference_type>(offset));
        offset += rooms[i].title.size();
    }

    return true;
}

size_t roomDetailStatePacketSize(const TcpRoomDetailState& detail) {
    size_t packetSize =
        kTcpHeaderSize + kRoomIdFieldSize + kRoomStatusFieldSize +
        kRoomTitleLengthFieldSize + detail.roomTitle.size() + 1 + 1 +
        kRoomActionMaskFieldSize + kTargetActionCountFieldSize;

    for (const TcpRoomMemberEntry& member : detail.members) {
        packetSize +=
            kRoomMemberSessionIdFieldSize + kNicknameLengthFieldSize +
            member.nickname.size() + kReadyStateFieldSize;
    }
    packetSize += detail.targetActions.size() * kTargetActionEntrySize;
    return packetSize;
}

bool serializeRoomDetailStatePacket(
    const TcpRoomDetailState& detail,
    std::vector<uint8_t>& outPacket) {
    outPacket.clear();
    if (!isValidRoomDetailState(detail)) {
        return false;
    }

    const size_t packetSize = roomDetailStatePacketSize(detail);
    if (packetSize > kMaxTcpPacketSize ||
        packetSize > std::numeric_limits<uint16_t>::max()) {
        return false;
    }

    outPacket.assign(packetSize, 0);
    writeU16BE(static_cast<uint16_t>(packetSize), outPacket.data());
    writeU16BE(static_cast<uint16_t>(TcpPacketType::kRoomDetailState), outPacket.data() + 2);

    size_t offset = kTcpHeaderSize;
    writeU32BE(detail.roomId, outPacket.data() + offset);
    offset += kRoomIdFieldSize;
    outPacket[offset] = static_cast<uint8_t>(detail.roomStatus);
    offset += kRoomStatusFieldSize;
    outPacket[offset] = static_cast<uint8_t>(detail.roomTitle.size());
    offset += kRoomTitleLengthFieldSize;
    std::copy(
        detail.roomTitle.begin(),
        detail.roomTitle.end(),
        outPacket.begin() + static_cast<std::vector<uint8_t>::difference_type>(offset));
    offset += detail.roomTitle.size();
    outPacket[offset] = detail.maxPlayers;
    offset += 1;
    outPacket[offset] = static_cast<uint8_t>(detail.members.size());
    offset += 1;

    for (const TcpRoomMemberEntry& member : detail.members) {
        writeU32BE(static_cast<uint32_t>(member.sessionId), outPacket.data() + offset);
        offset += kRoomMemberSessionIdFieldSize;
        outPacket[offset] = static_cast<uint8_t>(member.nickname.size());
        offset += kNicknameLengthFieldSize;
        std::copy(
            member.nickname.begin(),
            member.nickname.end(),
            outPacket.begin() + static_cast<std::vector<uint8_t>::difference_type>(offset));
        offset += member.nickname.size();
        outPacket[offset] = member.ready ? 1 : 0;
        offset += kReadyStateFieldSize;
    }

    writeU16BE(detail.selfActionMask, outPacket.data() + offset);
    offset += kRoomActionMaskFieldSize;
    outPacket[offset] = static_cast<uint8_t>(detail.targetActions.size());
    offset += kTargetActionCountFieldSize;
    for (const TcpTargetActionEntry& action : detail.targetActions) {
        writeU32BE(static_cast<uint32_t>(action.targetSessionId), outPacket.data() + offset);
        offset += kRoomMemberSessionIdFieldSize;
        writeU16BE(action.targetActionMask, outPacket.data() + offset);
        offset += kRoomActionMaskFieldSize;
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

size_t battleStartRosterPacketSize(size_t playerCount) {
    return kTcpHeaderSize + kRoomIdFieldSize + kBattleStartRosterCountFieldSize +
        (playerCount * kBattleStartRosterEntrySize);
}

bool serializeBattleStartRosterPacket(
    uint32_t roomId,
    const std::vector<uint64_t>& playerSessionIds,
    std::vector<uint8_t>& outPacket) {
    outPacket.clear();
    if (!isValidBattleStartRoster(roomId, playerSessionIds)) {
        return false;
    }

    const size_t packetSize = battleStartRosterPacketSize(playerSessionIds.size());
    if (packetSize > kMaxTcpPacketSize) {
        return false;
    }

    outPacket.assign(packetSize, 0);
    writeU16BE(static_cast<uint16_t>(packetSize), outPacket.data());
    writeU16BE(
        static_cast<uint16_t>(TcpPacketType::kBattleStartRoster),
        outPacket.data() + 2);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    writeU16BE(
        static_cast<uint16_t>(playerSessionIds.size()),
        outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize);

    uint8_t* payload =
        outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize +
        kBattleStartRosterCountFieldSize;
    for (size_t i = 0; i < playerSessionIds.size(); ++i) {
        writeU64BE(playerSessionIds[i], payload + (i * kBattleStartRosterEntrySize));
    }

    return true;
}

size_t battleLoadEntryPacketSize(size_t playerCount) {
    return kTcpHeaderSize + kRoomIdFieldSize + kBattleInstanceIdFieldSize +
        kBattleLoadEntryCountFieldSize + (playerCount * kBattleLoadEntryEntrySize);
}

bool serializeBattleLoadEntryPacket(
    uint32_t roomId,
    uint64_t battleInstanceId,
    const std::vector<uint64_t>& playerSessionIds,
    std::vector<uint8_t>& outPacket) {
    outPacket.clear();
    if (!isValidBattleLoadEntry(roomId, battleInstanceId, playerSessionIds)) {
        return false;
    }

    const size_t packetSize = battleLoadEntryPacketSize(playerSessionIds.size());
    if (packetSize > kMaxTcpPacketSize) {
        return false;
    }

    outPacket.assign(packetSize, 0);
    writeU16BE(static_cast<uint16_t>(packetSize), outPacket.data());
    writeU16BE(
        static_cast<uint16_t>(TcpPacketType::kBattleLoadEntry),
        outPacket.data() + 2);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    writeU64BE(
        battleInstanceId,
        outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize);
    writeU16BE(
        static_cast<uint16_t>(playerSessionIds.size()),
        outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize + kBattleInstanceIdFieldSize);

    uint8_t* payload =
        outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize + kBattleInstanceIdFieldSize +
        kBattleLoadEntryCountFieldSize;
    for (size_t i = 0; i < playerSessionIds.size(); ++i) {
        writeU64BE(playerSessionIds[i], payload + (i * kBattleLoadEntryEntrySize));
    }

    return true;
}

size_t battleFinalRankingPacketSize(const std::vector<BattleFinalRankingEntry>& rankings) {
    size_t packetSize = kTcpHeaderSize + kRoomIdFieldSize + kBattleInstanceIdFieldSize +
        kBattleFinalRankingCountFieldSize;
    for (const BattleFinalRankingEntry& ranking : rankings) {
        packetSize += kBattleFinalRankingRankFieldSize + kSessionIdFieldSize +
            kNicknameLengthFieldSize + ranking.nickname.size() + kFinalAssetValueFieldSize;
    }
    return packetSize;
}

bool serializeBattleFinalRankingPacket(
    uint32_t roomId,
    uint64_t battleInstanceId,
    const std::vector<BattleFinalRankingEntry>& rankings,
    std::vector<uint8_t>& outPacket) {
    outPacket.clear();
    if (!isValidBattleFinalRanking(roomId, battleInstanceId, rankings)) {
        return false;
    }

    const size_t packetSize = battleFinalRankingPacketSize(rankings);
    if (packetSize > kMaxTcpPacketSize ||
        packetSize > std::numeric_limits<uint16_t>::max()) {
        return false;
    }

    outPacket.assign(packetSize, 0);
    writeU16BE(static_cast<uint16_t>(packetSize), outPacket.data());
    writeU16BE(
        static_cast<uint16_t>(TcpPacketType::kBattleFinalRanking),
        outPacket.data() + 2);
    size_t offset = kTcpHeaderSize;
    writeU32BE(roomId, outPacket.data() + offset);
    offset += kRoomIdFieldSize;
    writeU64BE(battleInstanceId, outPacket.data() + offset);
    offset += kBattleInstanceIdFieldSize;
    outPacket[offset] = static_cast<uint8_t>(rankings.size());
    offset += kBattleFinalRankingCountFieldSize;

    for (const BattleFinalRankingEntry& ranking : rankings) {
        writeU16BE(ranking.rank, outPacket.data() + offset);
        offset += kBattleFinalRankingRankFieldSize;
        writeU64BE(ranking.sessionId, outPacket.data() + offset);
        offset += kSessionIdFieldSize;
        outPacket[offset] = static_cast<uint8_t>(ranking.nickname.size());
        offset += kNicknameLengthFieldSize;
        std::copy(
            ranking.nickname.begin(),
            ranking.nickname.end(),
            outPacket.begin() + static_cast<std::vector<uint8_t>::difference_type>(offset));
        offset += ranking.nickname.size();
        writeI64BE(ranking.totalAssetValue, outPacket.data() + offset);
        offset += kFinalAssetValueFieldSize;
    }

    return true;
}

bool serializeArenaLoadCompletePacket(
    uint32_t roomId,
    uint64_t battleInstanceId,
    std::array<uint8_t, kArenaLoadCompletePacketSize>& outPacket) {
    if (roomId == 0 || battleInstanceId == 0) {
        return false;
    }

    writePacketHeader(TcpPacketType::kArenaLoadComplete, outPacket);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    writeU64BE(battleInstanceId, outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize);
    return true;
}

bool serializeArenaGameplayStartPacket(
    uint32_t roomId,
    uint64_t battleInstanceId,
    std::array<uint8_t, kArenaGameplayStartPacketSize>& outPacket) {
    if (roomId == 0 || battleInstanceId == 0) {
        return false;
    }

    writePacketHeader(TcpPacketType::kArenaGameplayStart, outPacket);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    writeU64BE(battleInstanceId, outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize);
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

bool serializeMonsterHealthSnapshotPacket(
    uint32_t roomId,
    uint32_t monsterId,
    uint16_t currentHp,
    uint16_t maxHp,
    std::array<uint8_t, kMonsterHealthSnapshotPacketSize>& outPacket) {
    writePacketHeader(TcpPacketType::kMonsterHealthSnapshot, outPacket);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    writeU32BE(monsterId, outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize);
    writeU16BE(
        currentHp,
        outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize + kMonsterIdFieldSize);
    writeU16BE(
        maxHp,
        outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize + kMonsterIdFieldSize +
            kMonsterHpFieldSize);
    return true;
}

size_t dropListSnapshotPacketSize(size_t dropCount) {
    return kTcpHeaderSize + kRoomIdFieldSize + kDropListCountFieldSize + (dropCount * kDropEntrySize);
}

size_t dropListSnapshotV2PacketSize(size_t dropCount) {
    return kDropListSnapshotV2FixedPacketSize + (dropCount * kDropEntryV2Size);
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

bool serializeDropListSnapshotV2Packet(
    uint32_t roomId,
    uint32_t scatterSeed,
    const std::vector<TcpDropEntryV2>& drops,
    std::vector<uint8_t>& outPacket) {
    if (drops.size() > std::numeric_limits<uint16_t>::max()) {
        return false;
    }

    const size_t packetSize = dropListSnapshotV2PacketSize(drops.size());
    if (packetSize > kMaxTcpPacketSize) {
        return false;
    }

    outPacket.assign(packetSize, 0);
    writeU16BE(static_cast<uint16_t>(packetSize), outPacket.data());
    writeU16BE(static_cast<uint16_t>(TcpPacketType::kDropListSnapshotV2), outPacket.data() + 2);
    writeU32BE(roomId, outPacket.data() + kTcpHeaderSize);
    writeU32BE(scatterSeed, outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize);
    writeU16BE(
        static_cast<uint16_t>(drops.size()),
        outPacket.data() + kTcpHeaderSize + kRoomIdFieldSize + kScatterSeedFieldSize);

    uint8_t* payload = outPacket.data() + kDropListSnapshotV2FixedPacketSize;
    for (size_t i = 0; i < drops.size(); ++i) {
        uint8_t* entry = payload + (i * kDropEntryV2Size);
        writeU32BE(drops[i].dropId, entry);
        writeU32BE(drops[i].itemId, entry + kDropIdFieldSize);
        writeU16BE(drops[i].quantity, entry + kDropIdFieldSize + kItemIdFieldSize);
        writeI32BE(
            drops[i].posX,
            entry + kDropIdFieldSize + kItemIdFieldSize + kQuantityFieldSize);
        writeI32BE(
            drops[i].posY,
            entry + kDropIdFieldSize + kItemIdFieldSize + kQuantityFieldSize +
                kPositionFieldSize);
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

bool parseAuthenticateGameSessionPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    std::string& outToken) {
    outToken.clear();
    if (!parseTcpPacketHeader(data, size, outHeader)) {
        return false;
    }

    if (outHeader.type != TcpPacketType::kAuthenticateGameSession ||
        size < kTcpHeaderSize + kGameSessionTokenLengthFieldSize) {
        return false;
    }

    const uint16_t tokenLen = readU16BE(data + kTcpHeaderSize);
    if (tokenLen == 0 || tokenLen > kGameSessionTokenMaxLength) {
        return false;
    }

    const size_t expectedSize =
        kTcpHeaderSize + kGameSessionTokenLengthFieldSize + tokenLen;
    if (expectedSize != size) {
        return false;
    }

    outToken.assign(
        reinterpret_cast<const char*>(data + kTcpHeaderSize + kGameSessionTokenLengthFieldSize),
        reinterpret_cast<const char*>(
            data + kTcpHeaderSize + kGameSessionTokenLengthFieldSize + tokenLen));
    return true;
}

bool parseHeartbeatRequestPacket(const uint8_t* data, size_t size, TcpPacketHeader& outHeader) {
    return parseExactPacketType(data, size, TcpPacketType::kHeartbeatRequest, outHeader) &&
           size == kTcpHeaderSize;
}

bool parseCreateRoomRequestPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    std::string& outRoomTitle,
    uint8_t& outMaxPlayers) {
    outRoomTitle.clear();
    outMaxPlayers = 0;
    if (!parseExactPacketType(data, size, TcpPacketType::kCreateRoomRequest, outHeader) ||
        size < kTcpHeaderSize + kRoomTitleLengthFieldSize + kCreateRoomCapacityFieldSize) {
        return false;
    }

    const uint8_t roomTitleLen = data[kTcpHeaderSize];
    const size_t expectedSize =
        kTcpHeaderSize + kRoomTitleLengthFieldSize + roomTitleLen +
        kCreateRoomCapacityFieldSize;
    if (expectedSize != size) {
        return false;
    }

    const char* titleBegin = reinterpret_cast<const char*>(
        data + kTcpHeaderSize + kRoomTitleLengthFieldSize);
    outRoomTitle.assign(titleBegin, titleBegin + roomTitleLen);
    outMaxPlayers = data[size - 1];
    return isValidRoomTitleUtf8(outRoomTitle) && isValidCreateRoomCapacity(outMaxPlayers);
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

bool parseUnreadyRoomRequestPacket(const uint8_t* data, size_t size, TcpPacketHeader& outHeader) {
    return parseExactPacketType(data, size, TcpPacketType::kUnreadyRoomRequest, outHeader) &&
           size == kTcpHeaderSize;
}

bool parseHostStartBattleRequestPacket(const uint8_t* data, size_t size, TcpPacketHeader& outHeader) {
    return parseExactPacketType(data, size, TcpPacketType::kHostStartBattleRequest, outHeader) &&
           size == kTcpHeaderSize;
}

bool parseLeaveRoomResponsePacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId) {
    return parseRoomIdPacket(data, size, TcpPacketType::kLeaveRoomResponse, outHeader, outRoomId);
}

bool parseUnreadyRoomResponsePacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId) {
    return parseRoomIdPacket(data, size, TcpPacketType::kUnreadyRoomResponse, outHeader, outRoomId) &&
           outRoomId != 0;
}

bool parseHostStartBattleResponsePacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId) {
    return parseRoomIdPacket(data, size, TcpPacketType::kHostStartBattleResponse, outHeader, outRoomId) &&
           outRoomId != 0;
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
    outRooms.reserve(count);
    size_t offset = kTcpHeaderSize + kRoomListCountFieldSize;
    for (uint16_t i = 0; i < count; ++i) {
        if (offset + kRoomListEntryMinSize > size) {
            outRooms.clear();
            return false;
        }

        const uint8_t* entry = data + offset;
        const uint8_t statusValue = entry[kRoomIdFieldSize + (2 * kPlayerCountFieldSize)];
        if (!isValidRoomStatusValue(statusValue)) {
            outRooms.clear();
            return false;
        }

        offset += kRoomEntrySize;
        const uint8_t titleLength = data[offset];
        ++offset;
        if (offset + titleLength > size) {
            outRooms.clear();
            return false;
        }

        std::string title(
            reinterpret_cast<const char*>(data + offset),
            reinterpret_cast<const char*>(data + offset + titleLength));
        if (!title.empty() && !isValidRoomTitleUtf8(title)) {
            outRooms.clear();
            return false;
        }
        offset += titleLength;

        outRooms.push_back(TcpRoomEntry{
            readU32BE(entry),
            readU16BE(entry + kRoomIdFieldSize),
            readU16BE(entry + kRoomIdFieldSize + kPlayerCountFieldSize),
            static_cast<TcpRoomStatus>(statusValue),
            std::move(title),
        });
    }

    return offset == size;
}

bool parseHostKickRequestPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outTargetSessionId) {
    if (!parseExactPacketType(data, size, TcpPacketType::kHostKickRequest, outHeader) ||
        size != kHostKickRequestPacketSize) {
        return false;
    }

    outTargetSessionId = readU32BE(data + kTcpHeaderSize);
    return outTargetSessionId != 0;
}

bool parseHostKickResponsePacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint32_t& outTargetSessionId) {
    if (!parseExactPacketType(data, size, TcpPacketType::kHostKickResponse, outHeader) ||
        size != kHostKickResponsePacketSize) {
        return false;
    }

    outRoomId = readU32BE(data + kTcpHeaderSize);
    outTargetSessionId = readU32BE(data + kTcpHeaderSize + kRoomIdFieldSize);
    return outRoomId != 0 && outTargetSessionId != 0;
}

bool parseLobbyReturnVisibilityPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outPreviousRoomId,
    TcpLobbyReturnReason& outReason) {
    if (!parseExactPacketType(data, size, TcpPacketType::kLobbyReturnVisibility, outHeader) ||
        size != kLobbyReturnVisibilityPacketSize) {
        return false;
    }

    const uint32_t previousRoomId = readU32BE(data + kTcpHeaderSize);
    const uint8_t reasonValue = data[kTcpHeaderSize + kRoomIdFieldSize];
    if (previousRoomId == 0 || !isValidLobbyReturnReasonValue(reasonValue)) {
        return false;
    }

    outPreviousRoomId = previousRoomId;
    outReason = static_cast<TcpLobbyReturnReason>(reasonValue);
    return true;
}

bool parseRoomDetailStatePacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    TcpRoomDetailState& outDetail) {
    if (!parseExactPacketType(data, size, TcpPacketType::kRoomDetailState, outHeader) ||
        size < kTcpHeaderSize + kRoomIdFieldSize + kRoomStatusFieldSize +
                   kRoomTitleLengthFieldSize + 1 + 1 +
                   kRoomActionMaskFieldSize + kTargetActionCountFieldSize) {
        return false;
    }

    TcpRoomDetailState parsed;
    size_t offset = kTcpHeaderSize;
    parsed.roomId = readU32BE(data + offset);
    offset += kRoomIdFieldSize;
    const uint8_t roomStatusValue = data[offset];
    if (!isValidRoomStatusValue(roomStatusValue)) {
        return false;
    }
    parsed.roomStatus = static_cast<TcpRoomStatus>(roomStatusValue);
    offset += kRoomStatusFieldSize;

    const uint8_t roomTitleLength = data[offset];
    offset += kRoomTitleLengthFieldSize;
    if (offset + roomTitleLength > size) {
        return false;
    }
    parsed.roomTitle = std::string(
        reinterpret_cast<const char*>(data + offset),
        reinterpret_cast<const char*>(data + offset + roomTitleLength));
    offset += roomTitleLength;

    if (offset + 2 > size) {
        return false;
    }
    parsed.maxPlayers = data[offset];
    offset += 1;
    const uint8_t memberCount = data[offset];
    offset += 1;

    parsed.members.reserve(memberCount);
    for (uint8_t i = 0; i < memberCount; ++i) {
        if (offset + kRoomMemberSessionIdFieldSize + kNicknameLengthFieldSize > size) {
            return false;
        }

        TcpRoomMemberEntry member;
        member.sessionId = readU32BE(data + offset);
        offset += kRoomMemberSessionIdFieldSize;
        const uint8_t nicknameLength = data[offset];
        offset += kNicknameLengthFieldSize;
        if (offset + nicknameLength + kReadyStateFieldSize > size) {
            return false;
        }
        member.nickname = std::string(
            reinterpret_cast<const char*>(data + offset),
            reinterpret_cast<const char*>(data + offset + nicknameLength));
        offset += nicknameLength;
        const uint8_t readyValue = data[offset];
        if (!isValidReadyStateValue(readyValue)) {
            return false;
        }
        member.ready = readyValue == 1;
        offset += kReadyStateFieldSize;
        parsed.members.push_back(std::move(member));
    }

    if (offset + kRoomActionMaskFieldSize + kTargetActionCountFieldSize > size) {
        return false;
    }
    parsed.selfActionMask = readU16BE(data + offset);
    offset += kRoomActionMaskFieldSize;
    const uint8_t targetActionCount = data[offset];
    offset += kTargetActionCountFieldSize;
    if (offset + (static_cast<size_t>(targetActionCount) * kTargetActionEntrySize) != size) {
        return false;
    }

    parsed.targetActions.reserve(targetActionCount);
    for (uint8_t i = 0; i < targetActionCount; ++i) {
        TcpTargetActionEntry action;
        action.targetSessionId = readU32BE(data + offset);
        offset += kRoomMemberSessionIdFieldSize;
        action.targetActionMask = readU16BE(data + offset);
        offset += kRoomActionMaskFieldSize;
        parsed.targetActions.push_back(action);
    }

    if (!isValidRoomDetailState(parsed) ||
        roomDetailStatePacketSize(parsed) != size) {
        return false;
    }

    outDetail = std::move(parsed);
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

bool parseBattleStartRosterPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    std::vector<uint64_t>& outPlayerSessionIds) {
    outRoomId = 0;
    outPlayerSessionIds.clear();
    if (!parseExactPacketType(data, size, TcpPacketType::kBattleStartRoster, outHeader) ||
        size < kTcpHeaderSize + kRoomIdFieldSize + kBattleStartRosterCountFieldSize) {
        return false;
    }

    const uint32_t roomId = readU32BE(data + kTcpHeaderSize);
    const uint16_t playerCount =
        readU16BE(data + kTcpHeaderSize + kRoomIdFieldSize);
    if (size != battleStartRosterPacketSize(playerCount)) {
        return false;
    }

    std::vector<uint64_t> playerSessionIds;
    playerSessionIds.reserve(playerCount);
    const uint8_t* payload =
        data + kTcpHeaderSize + kRoomIdFieldSize + kBattleStartRosterCountFieldSize;
    for (uint16_t i = 0; i < playerCount; ++i) {
        playerSessionIds.push_back(readU64BE(payload + (i * kBattleStartRosterEntrySize)));
    }

    if (!isValidBattleStartRoster(roomId, playerSessionIds)) {
        return false;
    }

    outRoomId = roomId;
    outPlayerSessionIds = std::move(playerSessionIds);
    return true;
}

bool parseBattleLoadEntryPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint64_t& outBattleInstanceId,
    std::vector<uint64_t>& outPlayerSessionIds) {
    outRoomId = 0;
    outBattleInstanceId = 0;
    outPlayerSessionIds.clear();
    if (!parseExactPacketType(data, size, TcpPacketType::kBattleLoadEntry, outHeader) ||
        size < kTcpHeaderSize + kRoomIdFieldSize + kBattleInstanceIdFieldSize +
            kBattleLoadEntryCountFieldSize) {
        return false;
    }

    const uint32_t roomId = readU32BE(data + kTcpHeaderSize);
    const uint64_t battleInstanceId =
        readU64BE(data + kTcpHeaderSize + kRoomIdFieldSize);
    const uint16_t playerCount =
        readU16BE(data + kTcpHeaderSize + kRoomIdFieldSize + kBattleInstanceIdFieldSize);
    if (size != battleLoadEntryPacketSize(playerCount)) {
        return false;
    }

    std::vector<uint64_t> playerSessionIds;
    playerSessionIds.reserve(playerCount);
    const uint8_t* payload =
        data + kTcpHeaderSize + kRoomIdFieldSize + kBattleInstanceIdFieldSize +
        kBattleLoadEntryCountFieldSize;
    for (uint16_t i = 0; i < playerCount; ++i) {
        playerSessionIds.push_back(readU64BE(payload + (i * kBattleLoadEntryEntrySize)));
    }

    if (!isValidBattleLoadEntry(roomId, battleInstanceId, playerSessionIds)) {
        return false;
    }

    outRoomId = roomId;
    outBattleInstanceId = battleInstanceId;
    outPlayerSessionIds = std::move(playerSessionIds);
    return true;
}

bool parseBattleFinalRankingPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint64_t& outBattleInstanceId,
    std::vector<BattleFinalRankingEntry>& outRankings) {
    outRoomId = 0;
    outBattleInstanceId = 0;
    outRankings.clear();
    if (!parseExactPacketType(data, size, TcpPacketType::kBattleFinalRanking, outHeader) ||
        size < kTcpHeaderSize + kRoomIdFieldSize + kBattleInstanceIdFieldSize +
            kBattleFinalRankingCountFieldSize) {
        return false;
    }

    size_t offset = kTcpHeaderSize;
    const uint32_t roomId = readU32BE(data + offset);
    offset += kRoomIdFieldSize;
    const uint64_t battleInstanceId = readU64BE(data + offset);
    offset += kBattleInstanceIdFieldSize;
    const uint8_t rankingCount = data[offset];
    offset += kBattleFinalRankingCountFieldSize;
    if (rankingCount < kBattleFinalRankingMinRows ||
        rankingCount > kBattleFinalRankingMaxRows) {
        return false;
    }

    std::vector<BattleFinalRankingEntry> rankings;
    rankings.reserve(rankingCount);
    for (uint8_t i = 0; i < rankingCount; ++i) {
        if (offset + kBattleFinalRankingRankFieldSize + kSessionIdFieldSize +
                kNicknameLengthFieldSize + kFinalAssetValueFieldSize > size) {
            return false;
        }

        BattleFinalRankingEntry ranking;
        ranking.rank = readU16BE(data + offset);
        offset += kBattleFinalRankingRankFieldSize;
        ranking.sessionId = readU64BE(data + offset);
        offset += kSessionIdFieldSize;
        const uint8_t nicknameLength = data[offset];
        offset += kNicknameLengthFieldSize;
        if (offset + nicknameLength + kFinalAssetValueFieldSize > size) {
            return false;
        }
        ranking.nickname = std::string(
            reinterpret_cast<const char*>(data + offset),
            reinterpret_cast<const char*>(data + offset + nicknameLength));
        offset += nicknameLength;
        ranking.totalAssetValue = readI64BE(data + offset);
        offset += kFinalAssetValueFieldSize;
        rankings.push_back(std::move(ranking));
    }

    if (offset != size ||
        battleFinalRankingPacketSize(rankings) != size ||
        !isValidBattleFinalRanking(roomId, battleInstanceId, rankings)) {
        return false;
    }

    outRoomId = roomId;
    outBattleInstanceId = battleInstanceId;
    outRankings = std::move(rankings);
    return true;
}

bool parseArenaLoadCompletePacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint64_t& outBattleInstanceId) {
    return parseRoomBattleInstancePacket(
        data,
        size,
        TcpPacketType::kArenaLoadComplete,
        kArenaLoadCompletePacketSize,
        outHeader,
        outRoomId,
        outBattleInstanceId);
}

bool parseArenaGameplayStartPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint64_t& outBattleInstanceId) {
    return parseRoomBattleInstancePacket(
        data,
        size,
        TcpPacketType::kArenaGameplayStart,
        kArenaGameplayStartPacketSize,
        outHeader,
        outRoomId,
        outBattleInstanceId);
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

bool parseMonsterHealthSnapshotPacket(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint32_t& outMonsterId,
    uint16_t& outCurrentHp,
    uint16_t& outMaxHp) {
    if (!parseExactPacketType(data, size, TcpPacketType::kMonsterHealthSnapshot, outHeader) ||
        size != kMonsterHealthSnapshotPacketSize) {
        return false;
    }

    outRoomId = readU32BE(data + kTcpHeaderSize);
    outMonsterId = readU32BE(data + kTcpHeaderSize + kRoomIdFieldSize);
    outCurrentHp =
        readU16BE(data + kTcpHeaderSize + kRoomIdFieldSize + kMonsterIdFieldSize);
    outMaxHp = readU16BE(
        data + kTcpHeaderSize + kRoomIdFieldSize + kMonsterIdFieldSize + kMonsterHpFieldSize);
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

bool parseDropListSnapshotV2Packet(
    const uint8_t* data,
    size_t size,
    TcpPacketHeader& outHeader,
    uint32_t& outRoomId,
    uint32_t& outScatterSeed,
    std::vector<TcpDropEntryV2>& outDrops) {
    outDrops.clear();
    if (!parseTcpPacketHeader(data, size, outHeader)) {
        return false;
    }

    if (outHeader.type != TcpPacketType::kDropListSnapshotV2 ||
        size < kDropListSnapshotV2FixedPacketSize) {
        return false;
    }

    outRoomId = readU32BE(data + kTcpHeaderSize);
    outScatterSeed = readU32BE(data + kTcpHeaderSize + kRoomIdFieldSize);
    const uint16_t count =
        readU16BE(data + kTcpHeaderSize + kRoomIdFieldSize + kScatterSeedFieldSize);
    const size_t expectedSize = dropListSnapshotV2PacketSize(count);
    if (expectedSize != size) {
        return false;
    }

    outDrops.reserve(count);
    const uint8_t* payload = data + kDropListSnapshotV2FixedPacketSize;
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t* entry = payload + (static_cast<size_t>(i) * kDropEntryV2Size);
        outDrops.push_back(TcpDropEntryV2{
            readU32BE(entry),
            readU32BE(entry + kDropIdFieldSize),
            readU16BE(entry + kDropIdFieldSize + kItemIdFieldSize),
            readI32BE(entry + kDropIdFieldSize + kItemIdFieldSize + kQuantityFieldSize),
            readI32BE(
                entry + kDropIdFieldSize + kItemIdFieldSize + kQuantityFieldSize +
                    kPositionFieldSize),
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
