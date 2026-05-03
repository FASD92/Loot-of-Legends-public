#include "Game/RoomManager.hpp"

#include <algorithm>

namespace Game {
namespace {
constexpr uint32_t kDefaultMonsterTypeId = 1;
constexpr uint16_t kDefaultMonsterMaxHp = 100;
constexpr uint32_t kDefaultDropItemId = 1001;
constexpr uint16_t kDefaultDropQuantity = 1;
}  // namespace

RoomManager::RoomManager(uint16_t maxPlayersPerRoom, uint16_t maxInventoryWeight)
    : nextRoomId_(1),
      nextMonsterId_(1),
      nextDropId_(1),
      maxPlayersPerRoom_(maxPlayersPerRoom),
      maxInventoryWeight_(maxInventoryWeight) {}

RoomCommandResult RoomManager::createRoom(uint64_t sessionId) {
    if (findRoomIdForSession(sessionId).has_value()) {
        return RoomCommandResult(false, RoomCommandError::kAlreadyInRoom);
    }

    const uint32_t roomId = nextRoomId_++;
    Room room(roomId, maxPlayersPerRoom_, maxInventoryWeight_);
    const bool added = room.addPlayer(sessionId);
    if (!added) {
        return RoomCommandResult(false, RoomCommandError::kFull);
    }

    rooms_.emplace(roomId, room);
    sessionToRoomId_.emplace(sessionId, roomId);
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

    if (roomIt->second.isFull()) {
        return RoomCommandResult(false, RoomCommandError::kFull, summarizeRoom(roomIt->second));
    }

    if (!roomIt->second.addPlayer(sessionId)) {
        return RoomCommandResult(false, RoomCommandError::kAlreadyInRoom);
    }

    sessionToRoomId_.emplace(sessionId, roomId);
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
        return RoomCommandResult(false, RoomCommandError::kNotFound);
    }

    roomIt->second.removePlayer(sessionId);
    sessionToRoomId_.erase(mappingIt);

    RoomSummary summary = summarizeRoom(roomIt->second);
    const std::vector<uint64_t> playerSessionIds = roomIt->second.playerSessionIds();

    if (roomIt->second.empty()) {
        rooms_.erase(roomIt);
    }

    return RoomCommandResult(true, RoomCommandError::kNone, summary, playerSessionIds);
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

    room.markReady(sessionId);
    const bool battleJustStarted = room.tryStartBattle();
    return RoomCommandResult(
        true,
        RoomCommandError::kNone,
        summarizeRoom(room),
        room.playerSessionIds(),
        battleJustStarted);
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

    const uint32_t dropId = nextDropId_;
    if (!room.defeatMonster(monsterId, dropId, kDefaultDropItemId, kDefaultDropQuantity)) {
        return RoomCommandResult(false, RoomCommandError::kNotFound, summarizeRoom(room));
    }
    ++nextDropId_;

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
    return result;
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
/*  Room 내부 상태를 외부용 요약 구조체로 변환하는 함수
 *  서버가 Room 전체를 다시 열어보지 않아도 기본 상태 판단이 가능하게 함 */
RoomSummary RoomManager::summarizeRoom(const Room& room) const {
    return RoomSummary(
        room.roomId(),
        room.playerCount(),
        room.maxPlayers(),
        room.readyPlayerCount(),
        room.battleStarted(),
        room.hasAliveMonster());
}
}  // namespace Game
