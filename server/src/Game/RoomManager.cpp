#include "Game/RoomManager.hpp"

#include "Game/LootScatter.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <sstream>
#include <string>

namespace Game {
namespace {
constexpr uint32_t kDefaultMonsterTypeId = 1;
constexpr uint16_t kDefaultMonsterMaxHp = 100;
constexpr uint32_t kDefaultDropItemId = 1001;
constexpr uint16_t kDefaultDropQuantity = 1;
constexpr uint16_t kMinCreateRoomCapacity = 2;
constexpr uint16_t kMaxCreateRoomCapacity = 10;
constexpr std::size_t kRoomTitleMaxVisibleCharacters = 20;
constexpr std::size_t kRoomTitleMaxBytes = 64;

uint64_t currentUnixTimeMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}
// settlementId는 사람이 읽을 수 있는 문자열. roomId, sessionId, sequence를 조합해서 만든다.
std::string makeSettlementId(uint32_t roomId, uint64_t sessionId, uint32_t sequence) {
    std::ostringstream out;
    out << "room-" << roomId << "-session-" << sessionId << "-finish-" << sequence;
    return out.str();
}

std::string makeDefaultRoomTitle(uint32_t roomId) {
    return "Room" + std::to_string(roomId);
}

bool decodeNextUtf8CodePoint(const std::string& value, std::size_t& offset, uint32_t& outCodePoint) {
    const unsigned char first = static_cast<unsigned char>(value[offset]);
    if (first <= 0x7F) {
        outCodePoint = first;
        ++offset;
        return true;
    }

    std::size_t length = 0;
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

    for (std::size_t i = 1; i < length; ++i) {
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

bool isValidRoomTitle(const std::string& title) {
    if (title.empty() || title.size() > kRoomTitleMaxBytes) {
        return false;
    }

    std::size_t offset = 0;
    std::size_t visibleCharacters = 0;
    bool previousWhitespace = false;
    bool firstCharacter = true;
    while (offset < title.size()) {
        uint32_t codePoint = 0;
        if (!decodeNextUtf8CodePoint(title, offset, codePoint) ||
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
        if (visibleCharacters > kRoomTitleMaxVisibleCharacters) {
            return false;
        }
    }

    return visibleCharacters > 0 && !previousWhitespace;
}

bool isValidCreateRoomCapacity(uint16_t maxPlayers) {
    return maxPlayers >= kMinCreateRoomCapacity && maxPlayers <= kMaxCreateRoomCapacity;
}

bool isValidFinalRankingNickname(const std::string& nickname) {
    if (nickname.empty() || nickname.size() > 32) {
        return false;
    }

    for (const unsigned char ch : nickname) {
        if (ch < 0x20 || ch > 0x7E) {
            return false;
        }
    }

    return true;
}

bool checkedAddAssetValue(
    int64_t currentTotal,
    int64_t unitValue,
    uint16_t quantity,
    int64_t& outTotal) {
    if (unitValue < 0 || quantity == 0) {
        return false;
    }
    if (unitValue != 0 &&
        quantity > std::numeric_limits<int64_t>::max() / unitValue) {
        return false;
    }

    const int64_t stackValue = unitValue * static_cast<int64_t>(quantity);
    if (currentTotal > std::numeric_limits<int64_t>::max() - stackValue) {
        return false;
    }

    outTotal = currentTotal + stackValue;
    return outTotal >= 0;
}
}  // namespace

RoomManager::RoomManager(
    uint16_t maxPlayersPerRoom,
    uint16_t maxInventoryWeight,
    ItemValueCatalog itemValueCatalog)
    : nextRoomId_(1),
      nextMonsterId_(1),
      nextDropId_(1),
      nextSettlementSequence_(1),
      nextCombatSequence_(1),
      nextBattleInstanceId_(1),
      maxPlayersPerRoom_(maxPlayersPerRoom),
      maxInventoryWeight_(maxInventoryWeight),
      itemValueCatalog_(std::move(itemValueCatalog)) {}

RoomCommandResult RoomManager::createRoom(uint64_t sessionId) {
    const uint32_t roomId = nextRoomId_;
    return createRoom(sessionId, makeDefaultRoomTitle(roomId), maxPlayersPerRoom_);
}

RoomCommandResult RoomManager::createRoom(uint64_t sessionId, std::string title, uint16_t maxPlayers) {
    if (findRoomIdForSession(sessionId).has_value()) {
        return RoomCommandResult(false, RoomCommandError::kAlreadyInRoom);
    }

    if (!isValidRoomTitle(title) || !isValidCreateRoomCapacity(maxPlayers)) {
        return RoomCommandResult(false, RoomCommandError::kInvalidTarget);
    }

    const uint32_t roomId = nextRoomId_++;
    Room room(roomId, maxPlayers, maxInventoryWeight_, std::move(title));
    const bool added = room.addPlayer(sessionId);
    if (!added) {
        return RoomCommandResult(false, RoomCommandError::kFull);
    }

    rooms_.emplace(roomId, room);
    sessionToRoomId_.emplace(sessionId, roomId);
    sessionStartedAtMs_[sessionId] = currentUnixTimeMs();
    forgetSettlement(sessionId);
    return RoomCommandResult(
        true,
        RoomCommandError::kNone,
        summarizeRoom(rooms_.at(roomId)),
        rooms_.at(roomId).playerSessionIds());
}

RoomCommandResult RoomManager::joinRoom(uint64_t sessionId, uint32_t roomId) {
    if (findRoomIdForSession(sessionId).has_value()) {
        return RoomCommandResult(false, RoomCommandError::kAlreadyInRoom);
    }

    auto roomIt = rooms_.find(roomId);
    if (roomIt == rooms_.end()) {
        return RoomCommandResult(false, RoomCommandError::kNotFound);
    }

    if (roomIt->second.battleStarted()) {
        return RoomCommandResult(
            false,
            RoomCommandError::kAlreadyStarted,
            summarizeRoom(roomIt->second));
    }

    if (roomIt->second.isFull()) {
        return RoomCommandResult(false, RoomCommandError::kFull, summarizeRoom(roomIt->second));
    }

    if (!roomIt->second.addPlayer(sessionId)) {
        return RoomCommandResult(false, RoomCommandError::kAlreadyInRoom);
    }

    sessionToRoomId_.emplace(sessionId, roomId);
    sessionStartedAtMs_[sessionId] = currentUnixTimeMs();
    forgetSettlementsForPlayers(roomIt->second.playerSessionIds()); // join은 Room 멤버십을 바꾸므로 해당 Room의 모든 플레이어 정산 캐시를 무효화해야 한다.
    return RoomCommandResult(
        true,
        RoomCommandError::kNone,
        summarizeRoom(roomIt->second),
        roomIt->second.playerSessionIds());
}

RoomCommandResult RoomManager::leaveRoom(uint64_t sessionId) {
    auto mappingIt = sessionToRoomId_.find(sessionId);
    if (mappingIt == sessionToRoomId_.end()) {
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    const uint32_t roomId = mappingIt->second;
    auto roomIt = rooms_.find(roomId);
    if (roomIt == rooms_.end()) {
        sessionToRoomId_.erase(mappingIt);
        sessionStartedAtMs_.erase(sessionId);
        forgetSettlement(sessionId);
        return RoomCommandResult(false, RoomCommandError::kNotFound);
    }

    const std::vector<uint64_t> affectedSessionIds = roomIt->second.playerSessionIds();
    const bool hostTransferred =
        roomIt->second.isHost(sessionId) && roomIt->second.playerCount() > 1;
    roomIt->second.removePlayer(sessionId);
    sessionToRoomId_.erase(mappingIt);
    sessionStartedAtMs_.erase(sessionId);
    forgetSettlementsForPlayers(affectedSessionIds);

    RoomSummary summary = summarizeRoom(roomIt->second);
    const std::vector<uint64_t> playerSessionIds = roomIt->second.playerSessionIds();

    if (roomIt->second.empty()) {
        rooms_.erase(roomIt);
    }

    RoomCommandResult result(true, RoomCommandError::kNone, summary, playerSessionIds);
    result.hostTransferred = hostTransferred;
    return result;
}

RoomCommandResult RoomManager::markReady(uint64_t sessionId) {
    auto mappingIt = sessionToRoomId_.find(sessionId);
    if (mappingIt == sessionToRoomId_.end()) {
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    auto roomIt = rooms_.find(mappingIt->second);
    if (roomIt == rooms_.end()) {
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotFound);
    }

    Room& room = roomIt->second;
    if (!room.contains(sessionId)) {
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    if (room.battleStarted()) {
        return RoomCommandResult(false, RoomCommandError::kAlreadyStarted, summarizeRoom(room));
    }

    room.markReady(sessionId);
    return RoomCommandResult(
        true,
        RoomCommandError::kNone,
        summarizeRoom(room),
        room.playerSessionIds(),
        false);
}

RoomCommandResult RoomManager::markUnready(uint64_t sessionId) {
    auto mappingIt = sessionToRoomId_.find(sessionId);
    if (mappingIt == sessionToRoomId_.end()) {
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    auto roomIt = rooms_.find(mappingIt->second);
    if (roomIt == rooms_.end()) {
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotFound);
    }

    Room& room = roomIt->second;
    if (!room.contains(sessionId)) {
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    if (room.battleStarted()) {
        return RoomCommandResult(false, RoomCommandError::kAlreadyStarted, summarizeRoom(room));
    }

    room.markUnready(sessionId);
    return RoomCommandResult(
        true,
        RoomCommandError::kNone,
        summarizeRoom(room),
        room.playerSessionIds());
}

RoomCommandResult RoomManager::hostStartBattle(uint64_t sessionId) {
    auto mappingIt = sessionToRoomId_.find(sessionId);
    if (mappingIt == sessionToRoomId_.end()) {
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    auto roomIt = rooms_.find(mappingIt->second);
    if (roomIt == rooms_.end()) {
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotFound);
    }

    Room& room = roomIt->second;
    if (!room.contains(sessionId)) {
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    if (room.battleStarted()) {
        return RoomCommandResult(false, RoomCommandError::kAlreadyStarted, summarizeRoom(room));
    }

    if (!room.isHost(sessionId)) {
        return RoomCommandResult(false, RoomCommandError::kNotHost, summarizeRoom(room));
    }

    if (room.playerCount() < Room::kMinPlayersToStart) {
        return RoomCommandResult(false, RoomCommandError::kNotEnoughPlayers, summarizeRoom(room));
    }

    if (!room.canStartBattle()) {
        return RoomCommandResult(false, RoomCommandError::kNotAllReady, summarizeRoom(room));
    }

    const uint64_t battleInstanceId = nextBattleInstanceId_;
    const bool battleJustStarted = room.hostStartBattle(sessionId, battleInstanceId);
    if (!battleJustStarted) {
        return RoomCommandResult(false, RoomCommandError::kNotAllReady, summarizeRoom(room));
    }
    ++nextBattleInstanceId_;

    forgetSettlementsForPlayers(room.playerSessionIds());
    RoomCommandResult result(
        true,
        RoomCommandError::kNone,
        summarizeRoom(room),
        room.playerSessionIds(),
        true);
    result.battleInstanceId = battleInstanceId;
    result.arenaLoadBarrierOpened = true;
    return result;
}

RoomCommandResult RoomManager::markArenaLoadComplete(
    uint64_t sessionId,
    uint32_t roomId,
    uint64_t battleInstanceId) {
    auto mappingIt = sessionToRoomId_.find(sessionId);
    if (mappingIt == sessionToRoomId_.end()) {
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    if (mappingIt->second != roomId) {
        return RoomCommandResult(false, RoomCommandError::kInvalidTarget);
    }

    auto roomIt = rooms_.find(roomId);
    if (roomIt == rooms_.end()) {
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotFound);
    }

    Room& room = roomIt->second;
    if (!room.contains(sessionId)) {
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    bool arenaGameplayJustStarted = false;
    if (!room.markArenaLoadComplete(sessionId, battleInstanceId, arenaGameplayJustStarted)) {
        RoomCommandResult rejected(
            false,
            RoomCommandError::kInvalidTarget,
            summarizeRoom(room),
            room.playerSessionIds());
        rejected.battleInstanceId = room.battleInstanceId();
        return rejected;
    }

    RoomCommandResult result(
        true,
        RoomCommandError::kNone,
        summarizeRoom(room),
        room.playerSessionIds());
    result.battleInstanceId = room.battleInstanceId();
    result.arenaGameplayJustStarted = arenaGameplayJustStarted;
    return result;
}

RoomCommandResult RoomManager::hostKick(uint64_t hostSessionId, uint64_t targetSessionId) {
    auto mappingIt = sessionToRoomId_.find(hostSessionId);
    if (mappingIt == sessionToRoomId_.end()) {
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    auto roomIt = rooms_.find(mappingIt->second);
    if (roomIt == rooms_.end()) {
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotFound);
    }

    Room& room = roomIt->second;
    if (!room.contains(hostSessionId)) {
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    if (room.battleStarted()) {
        return RoomCommandResult(false, RoomCommandError::kAlreadyStarted, summarizeRoom(room));
    }

    if (!room.isHost(hostSessionId)) {
        return RoomCommandResult(false, RoomCommandError::kNotHost, summarizeRoom(room));
    }

    if (targetSessionId == hostSessionId || !room.contains(targetSessionId)) {
        return RoomCommandResult(false, RoomCommandError::kInvalidTarget, summarizeRoom(room));
    }

    const std::vector<uint64_t> affectedSessionIds = room.playerSessionIds();
    if (!room.kickPlayer(targetSessionId)) {
        return RoomCommandResult(false, RoomCommandError::kInvalidTarget, summarizeRoom(room));
    }

    sessionToRoomId_.erase(targetSessionId);
    sessionStartedAtMs_.erase(targetSessionId);
    forgetSettlementsForPlayers(affectedSessionIds);

    RoomCommandResult result(
        true,
        RoomCommandError::kNone,
        summarizeRoom(room),
        room.playerSessionIds());
    result.kickedSessionId = targetSessionId;
    return result;
}

RoomCommandResult RoomManager::spawnMonster(uint32_t roomId) {
    auto roomIt = rooms_.find(roomId);
    if (roomIt == rooms_.end()) {
        return RoomCommandResult(false, RoomCommandError::kNotFound);
    }

    Room& room = roomIt->second;    // room 객체
    if (!room.battleStarted() || room.hasAliveMonster() || !room.drops().empty()) {
        return RoomCommandResult(false, RoomCommandError::kNotFound, summarizeRoom(room));
    }

    const uint32_t monsterId = nextMonsterId_;
    if (!room.spawnMonster(monsterId, kDefaultMonsterTypeId, kDefaultMonsterMaxHp)) {
        return RoomCommandResult(false, RoomCommandError::kNotFound, summarizeRoom(room));
    }
    ++nextMonsterId_;   // spawnMonster가 성공해야만 실행된다.

    return RoomCommandResult(
        true,
        RoomCommandError::kNone,
        summarizeRoom(room),
        room.playerSessionIds(),
        false,
        true,
        false,
        room.monster(),
        room.drops());
}

RoomCommandResult RoomManager::defeatMonster(uint64_t sessionId, uint32_t monsterId) {
    auto mappingIt = sessionToRoomId_.find(sessionId);  // 참고로 이 함수는 sessionId를 인자로 받는다. 즉 서버는 이 세션이 어느 방에 있는지 먼저 확인해야 한다.
    if (mappingIt == sessionToRoomId_.end()) {          // 어떤 Room에도 속하지 않았다면 몬스터를 처치할 수 없음.
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    auto roomIt = rooms_.find(mappingIt->second);
    if (roomIt == rooms_.end()) {                   // 매핑은 있는데 Room이 없다? -> 내부 정합성 깨짐
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotFound);
    }

    Room& room = roomIt->second;
    if (!room.contains(sessionId)) {                // 매핑은 있는데 Room 내부에 session이 없다?
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    const uint32_t scatterSeed =
        nextDropId_ ^ (room.roomId() * 2654435761U) ^ monsterId;
    const std::vector<ScatterDropPlacement> placements =
        buildLootScatter(scatterSeed, room.playerCount(), room.monster().position, nextDropId_);

    std::vector<Drop> scatteredDrops;
    scatteredDrops.reserve(placements.size());
    for (const ScatterDropPlacement& placement : placements) {
        scatteredDrops.push_back(
            Drop{
                placement.sequence,
                kDefaultDropItemId,
                kDefaultDropQuantity,
                1,
                false,
                0,
                placement.position});
    }

    if (!room.defeatMonster(monsterId, scatteredDrops)) {
        return RoomCommandResult(false, RoomCommandError::kNotFound, summarizeRoom(room));
    }
    room.markBattleDropsVisible(currentUnixTimeMs());
    nextDropId_ += static_cast<uint32_t>(scatteredDrops.size());

    return RoomCommandResult(
        true,
        RoomCommandError::kNone,
        summarizeRoom(room),
        room.playerSessionIds(),
        false,
        false,
        true,
        room.monster(),
        room.drops());
}

RoomCommandResult RoomManager::attackMonster(
    uint64_t sessionId,
    uint32_t targetHintMonsterId) {
    auto mappingIt = sessionToRoomId_.find(sessionId);
    if (mappingIt == sessionToRoomId_.end()) {
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    auto roomIt = rooms_.find(mappingIt->second);
    if (roomIt == rooms_.end()) {
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotFound);
    }

    Room& room = roomIt->second;
    if (!room.contains(sessionId)) {
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    const uint32_t scatterSeed =
        nextDropId_ ^
        (room.roomId() * 2654435761U) ^
        targetHintMonsterId ^
        nextCombatSequence_;
    const std::vector<ScatterDropPlacement> placements =
        buildLootScatter(scatterSeed, room.playerCount(), room.monster().position, nextDropId_);

    std::vector<Drop> deathDrops;
    deathDrops.reserve(placements.size());
    for (const ScatterDropPlacement& placement : placements) {
        deathDrops.push_back(
            Drop{
                placement.sequence,
                kDefaultDropItemId,
                kDefaultDropQuantity,
                1,
                false,
                0,
                placement.position});
    }

    const AttackApplyResult attack =
        room.applyAttack(sessionId, targetHintMonsterId, deathDrops);
    if (attack.status != AttackApplyStatus::kApplied) {
        return RoomCommandResult(false, RoomCommandError::kNotFound, summarizeRoom(room));
    }

    ++nextCombatSequence_;
    if (attack.monsterJustDefeated) {
        room.markBattleDropsVisible(currentUnixTimeMs());
        nextDropId_ += static_cast<uint32_t>(deathDrops.size());
        forgetSettlementsForPlayers(room.playerSessionIds());
    }

    RoomCommandResult result(
        true,
        RoomCommandError::kNone,
        summarizeRoom(room),
        room.playerSessionIds(),
        false,
        false,
        attack.monsterJustDefeated,
        attack.monster,
        attack.drops);
    result.scatterSeed = scatterSeed;
    return result;
}

RoomCommandResult RoomManager::createCenterDropForSmoke(uint64_t sessionId) {
    auto mappingIt = sessionToRoomId_.find(sessionId);
    if (mappingIt == sessionToRoomId_.end()) {
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    auto roomIt = rooms_.find(mappingIt->second);
    if (roomIt == rooms_.end()) {
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotFound);
    }

    Room& room = roomIt->second;
    if (!room.contains(sessionId)) {
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    const uint32_t dropId = nextDropId_;
    if (!room.createSmokeDrop(dropId, kDefaultDropItemId, kDefaultDropQuantity)) {
        return RoomCommandResult(false, RoomCommandError::kNotFound, summarizeRoom(room));
    }
    room.markBattleDropsVisible(currentUnixTimeMs());
    ++nextDropId_;

    return RoomCommandResult(
        true,
        RoomCommandError::kNone,
        summarizeRoom(room),
        room.playerSessionIds(),
        false,
        false,
        false,
        room.monster(),
        room.drops());
}

RoomCommandResult RoomManager::claimLoot(uint64_t sessionId, uint32_t dropId) {
    auto mappingIt = sessionToRoomId_.find(sessionId);
    if (mappingIt == sessionToRoomId_.end()) {
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    auto roomIt = rooms_.find(mappingIt->second);
    if (roomIt == rooms_.end()) {
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotFound);
    }

    Room& room = roomIt->second;
    if (!room.contains(sessionId)) {    // sessionToRoomId_에는 있는데 Room 내부에는 session이 없는지 재확인
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    const LootClaimResult claim = room.claimLoot(sessionId, dropId);
    if (!claim.found) {     // dropId가 없거나, inventory를 못 찾는 등 루팅 판정 대상이 없다면
        return RoomCommandResult(false, RoomCommandError::kNotFound, summarizeRoom(room));
    }

    RoomCommandResult result(
        true,
        RoomCommandError::kNone,
        summarizeRoom(room),
        room.playerSessionIds(),
        false,
        false,
        false,
        room.monster(),
        room.drops());
    // claimLoot 결과 -> RoomCommandResult의 루팅 필드로 옮기기
    result.lootJustClaimed = claim.claimed;
    result.lootRejected = claim.rejected;
    result.lootRejectReason = claim.rejectReason;
    result.winnerSessionId = claim.winnerSessionId;
    result.drop = claim.drop;
    result.inventory = claim.inventory;
    if (claim.claimed) {    // 실제로 이 세션이 loot를 획득한 경우에만 정산 캐시 무효화.
        forgetSettlement(sessionId);
    }
    return result;
}

RoomCommandResult RoomManager::claimNearestLoot(uint64_t sessionId) {
    auto mappingIt = sessionToRoomId_.find(sessionId);
    if (mappingIt == sessionToRoomId_.end()) {
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    auto roomIt = rooms_.find(mappingIt->second);
    if (roomIt == rooms_.end()) {
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotFound);
    }

    Room& room = roomIt->second;
    if (!room.contains(sessionId)) {
        sessionToRoomId_.erase(mappingIt);
        return RoomCommandResult(false, RoomCommandError::kNotInRoom);
    }

    const LootClaimResult claim = room.claimNearestLoot(sessionId);
    if (!claim.found) {
        return RoomCommandResult(false, RoomCommandError::kNotFound, summarizeRoom(room));
    }

    RoomCommandResult result(
        true,
        RoomCommandError::kNone,
        summarizeRoom(room),
        room.playerSessionIds(),
        false,
        false,
        false,
        room.monster(),
        room.drops());
    result.lootJustClaimed = claim.claimed;
    result.lootRejected = claim.rejected;
    result.lootRejectReason = claim.rejectReason;
    result.winnerSessionId = claim.winnerSessionId;
    result.drop = claim.drop;
    result.inventory = claim.inventory;
    if (claim.claimed) {
        forgetSettlement(sessionId);
    }
    return result;
}

SmokePlayerPlacementResult RoomManager::placePlayersAroundCenterDropForSmoke(uint64_t sessionId) {
    auto mappingIt = sessionToRoomId_.find(sessionId);
    if (mappingIt == sessionToRoomId_.end()) {
        return SmokePlayerPlacementResult{false, RoomCommandError::kNotInRoom};
    }

    const uint32_t roomId = mappingIt->second;
    auto roomIt = rooms_.find(roomId);
    if (roomIt == rooms_.end()) {
        sessionToRoomId_.erase(mappingIt);
        return SmokePlayerPlacementResult{false, RoomCommandError::kNotFound, roomId};
    }

    Room& room = roomIt->second;
    if (!room.contains(sessionId)) {
        sessionToRoomId_.erase(mappingIt);
        return SmokePlayerPlacementResult{false, RoomCommandError::kNotInRoom, roomId};
    }

    if (!room.battleStarted() || room.drops().size() != 1 || room.drops()[0].claimed) {
        return SmokePlayerPlacementResult{false, RoomCommandError::kNotFound, roomId};
    }

    if (!room.placePlayersAroundSmokeCenter()) {
        return SmokePlayerPlacementResult{false, RoomCommandError::kNotFound, roomId};
    }

    return SmokePlayerPlacementResult{
        true,
        RoomCommandError::kNone,
        room.roomId(),
        room.playerSessionIds(),
        room.movementSnapshots()};
}

MovementCommandResult RoomManager::applyMovement(
    uint64_t sessionId,
    int16_t dirX,
    int16_t dirY,
    uint32_t elapsedMs) {
    auto mappingIt = sessionToRoomId_.find(sessionId);
    if (mappingIt == sessionToRoomId_.end()) {
        return MovementCommandResult{false, RoomCommandError::kNotInRoom, 0, sessionId};
    }

    const uint32_t roomId = mappingIt->second;
    auto roomIt = rooms_.find(roomId);
    if (roomIt == rooms_.end()) {
        sessionToRoomId_.erase(mappingIt);
        return MovementCommandResult{false, RoomCommandError::kNotFound, roomId, sessionId};
    }

    Room& room = roomIt->second;
    if (!room.contains(sessionId)) {
        sessionToRoomId_.erase(mappingIt);
        return MovementCommandResult{false, RoomCommandError::kNotInRoom, roomId, sessionId};
    }

    const MovementApplyResult applied =
        room.applyMovement(sessionId, dirX, dirY, elapsedMs);
    if (applied.status != MovementApplyStatus::kApplied) {
        return MovementCommandResult{false, RoomCommandError::kNotInRoom, roomId, sessionId};
    }

    return MovementCommandResult{
        true,
        RoomCommandError::kNone,
        roomId,
        sessionId,
        applied.previousPosition,
        applied.currentPosition};
}

SettlementCommandResult RoomManager::buildSettlementResult(
    uint64_t sessionId,
    uint64_t finishedAtUnixMs) {
    auto cachedIt = settlementBySessionId_.find(sessionId);
    if (cachedIt != settlementBySessionId_.end()) {
        return SettlementCommandResult{true, RoomCommandError::kNone, cachedIt->second};
    }

    auto mappingIt = sessionToRoomId_.find(sessionId);
    if (mappingIt == sessionToRoomId_.end()) {
        return SettlementCommandResult{false, RoomCommandError::kNotInRoom, SettlementResult{}};
    }

    auto roomIt = rooms_.find(mappingIt->second);
    if (roomIt == rooms_.end()) {
        sessionToRoomId_.erase(mappingIt);
        sessionStartedAtMs_.erase(sessionId);
        forgetSettlement(sessionId);
        return SettlementCommandResult{false, RoomCommandError::kNotFound, SettlementResult{}};
    }

    const Room& room = roomIt->second;
    if (!room.contains(sessionId)) {
        sessionToRoomId_.erase(mappingIt);
        sessionStartedAtMs_.erase(sessionId);
        forgetSettlement(sessionId);
        return SettlementCommandResult{false, RoomCommandError::kNotInRoom, SettlementResult{}};
    }

    const InventorySnapshot* inventory = room.findInventory(sessionId);
    if (inventory == nullptr) {
        return SettlementCommandResult{false, RoomCommandError::kNotFound, SettlementResult{}};
    }

    SettlementResult settlement;
    settlement.settlementId =
        makeSettlementId(room.roomId(), sessionId, nextSettlementSequence_++);
    settlement.sessionId = sessionId;
    // Week 6 Debug CLI에는 별도 계정 인증이 없으므로 sessionId를 debug accountId로 사용한다.
    settlement.accountId = sessionId;
    settlement.roomId = room.roomId();
    const auto startedIt = sessionStartedAtMs_.find(sessionId);
    settlement.startedAtUnixMs =
        startedIt == sessionStartedAtMs_.end() ? finishedAtUnixMs : startedIt->second;
    settlement.finishedAtUnixMs = finishedAtUnixMs;
    settlement.goldDelta = 0;
    settlement.reason = SettlementReason::kNormal;

    settlement.inventoryDeltas.reserve(room.drops().size());
    for (const Drop& drop : room.drops()) {
        if (!drop.claimed || drop.ownerSessionId != sessionId) {
            continue;
        }

        settlement.inventoryDeltas.push_back(
            SettlementInventoryDelta{
                drop.itemId,
                static_cast<int32_t>(drop.quantity),
                drop.dropId});
    }

    settlementBySessionId_[sessionId] = settlement;
    return SettlementCommandResult{
        true,
        RoomCommandError::kNone,
        settlementBySessionId_.at(sessionId)};
}

BattleFinalRankingResult RoomManager::buildBattleFinalRanking(
    uint32_t roomId,
    NicknameLookup nicknameLookup) const {
    BattleFinalRankingResult result;
    result.roomId = roomId;

    const auto roomIt = rooms_.find(roomId);
    if (roomIt == rooms_.end()) {
        result.status = BattleFinalRankingStatus::kRoomNotFound;
        return result;
    }

    const Room& room = roomIt->second;
    result.battleInstanceId = room.battleInstanceId();
    result.participantSessionIds = room.arenaGameplayParticipantSessionIds();

    if (!room.arenaGameplayStarted() || result.battleInstanceId == 0) {
        result.status = BattleFinalRankingStatus::kNotInGameplay;
        return result;
    }

    if (!room.allBattleDropsResolved()) {
        result.status = BattleFinalRankingStatus::kNotComplete;
        return result;
    }

    if (!nicknameLookup || result.participantSessionIds.empty()) {
        result.status = BattleFinalRankingStatus::kResultGenerationFailure;
        return result;
    }

    std::vector<BattleFinalRankingRow> rows;
    rows.reserve(result.participantSessionIds.size());
    for (const uint64_t sessionId : result.participantSessionIds) {
        const std::optional<std::string> nickname = nicknameLookup(sessionId);
        if (!nickname.has_value() || !isValidFinalRankingNickname(*nickname)) {
            result.status = BattleFinalRankingStatus::kResultGenerationFailure;
            return result;
        }

        const InventorySnapshot* inventory = room.findInventory(sessionId);
        if (inventory == nullptr) {
            result.status = BattleFinalRankingStatus::kResultGenerationFailure;
            return result;
        }

        int64_t totalAssetValue = 0;
        for (const InventoryEntry& entry : inventory->entries) {
            if (entry.quantity == 0) {
                continue;
            }
            const std::optional<int64_t> unitValue =
                itemValueCatalog_.findUnitValue(entry.itemId);
            if (!unitValue.has_value()) {
                result.status = BattleFinalRankingStatus::kResultGenerationFailure;
                return result;
            }

            int64_t nextTotal = 0;
            if (!checkedAddAssetValue(
                    totalAssetValue,
                    *unitValue,
                    entry.quantity,
                    nextTotal)) {
                result.status = BattleFinalRankingStatus::kResultGenerationFailure;
                return result;
            }
            totalAssetValue = nextTotal;
        }

        rows.push_back(BattleFinalRankingRow{0, sessionId, *nickname, totalAssetValue});
    }

    std::sort(
        rows.begin(),
        rows.end(),
        [](const BattleFinalRankingRow& lhs, const BattleFinalRankingRow& rhs) {
            if (lhs.totalAssetValue != rhs.totalAssetValue) {
                return lhs.totalAssetValue > rhs.totalAssetValue;
            }
            return lhs.sessionId < rhs.sessionId;
        });

    uint16_t currentRank = 0;
    int64_t previousValue = -1;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        if (index == 0 || rows[index].totalAssetValue != previousValue) {
            currentRank = static_cast<uint16_t>(index + 1);
            previousValue = rows[index].totalAssetValue;
        }
        rows[index].rank = currentRank;
    }

    result.status = BattleFinalRankingStatus::kOk;
    result.rows = std::move(rows);
    return result;
}

std::vector<BattleFinalRankingResult> RoomManager::processBattleDropTimeouts(
    uint64_t nowUnixMs,
    NicknameLookup nicknameLookup) {
    std::vector<BattleFinalRankingResult> completed;
    for (auto& entry : rooms_) {
        Room& room = entry.second;
        if (!room.resolveExpiredBattleDrops(nowUnixMs) || !room.allBattleDropsResolved()) {
            continue;
        }
        completed.push_back(buildBattleFinalRanking(room.roomId(), nicknameLookup));
    }

    std::sort(
        completed.begin(),
        completed.end(),
        [](const BattleFinalRankingResult& lhs, const BattleFinalRankingResult& rhs) {
            return lhs.roomId < rhs.roomId;
        });
    return completed;
}

std::vector<uint64_t> RoomManager::closeRoom(uint32_t roomId) {
    auto roomIt = rooms_.find(roomId);
    if (roomIt == rooms_.end()) {
        return {};
    }

    std::vector<uint64_t> sessionIds = roomIt->second.playerSessionIds();
    for (uint64_t sessionId : roomIt->second.arenaGameplayParticipantSessionIds()) {
        if (std::find(sessionIds.begin(), sessionIds.end(), sessionId) == sessionIds.end()) {
            sessionIds.push_back(sessionId);
        }
    }

    const uint64_t finishedAtUnixMs = currentUnixTimeMs();
    for (uint64_t sessionId : roomIt->second.playerSessionIds()) {
        buildSettlementResult(sessionId, finishedAtUnixMs);
    }

    for (uint64_t sessionId : sessionIds) {
        sessionToRoomId_.erase(sessionId);
        sessionStartedAtMs_.erase(sessionId);
    }
    rooms_.erase(roomIt);
    return sessionIds;
}

std::optional<uint32_t> RoomManager::findRoomIdForSession(uint64_t sessionId) const {
    auto it = sessionToRoomId_.find(sessionId);
    if (it == sessionToRoomId_.end()) {
        return std::nullopt;
    }

    return it->second;
}

const Room* RoomManager::findRoom(uint32_t roomId) const {
    auto it = rooms_.find(roomId);
    if (it == rooms_.end()) {
        return nullptr;
    }

    return &it->second;
}

std::vector<RoomSummary> RoomManager::roomList() const {
    std::vector<RoomSummary> rooms;
    rooms.reserve(rooms_.size());
    for (const auto& entry : rooms_) {
        rooms.push_back(summarizeRoom(entry.second));
    }

    std::sort(
        rooms.begin(),
        rooms.end(),
        [](const RoomSummary& lhs, const RoomSummary& rhs) { return lhs.roomId < rhs.roomId; });
    return rooms;
}

size_t RoomManager::roomCount() const {
    return rooms_.size();
}

void RoomManager::forgetSettlement(uint64_t sessionId) {
    settlementBySessionId_.erase(sessionId);
}

void RoomManager::forgetSettlementsForPlayers(const std::vector<uint64_t>& sessionIds) {
    for (uint64_t sessionId : sessionIds) {
        forgetSettlement(sessionId);
    }
}

/*  Room 내부 상태를 외부용 요약 구조체로 변환하는 함수
 *  서버가 Room 전체를 다시 열어보지 않아도 기본 상태 판단이 가능하게 함 */
RoomSummary RoomManager::summarizeRoom(const Room& room) const {
    return RoomSummary(
        room.roomId(),
        room.title(),
        room.playerCount(),
        room.maxPlayers(),
        room.readyPlayerCount(),
        room.battleStarted(),
        room.hasAliveMonster());
}
}  // namespace Game
