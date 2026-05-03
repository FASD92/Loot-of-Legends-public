#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <unordered_map>
#include <vector>

#include "Game/Room.hpp"

namespace Game {
enum class RoomCommandError : uint16_t {
    kNone = 0,
    kFull = 1,
    kNotFound = 2,
    kAlreadyInRoom = 3,
    kNotInRoom = 4,
};

struct RoomSummary {
    uint32_t roomId{0};
    uint16_t playerCount{0};
    uint16_t maxPlayers{0};
    uint16_t readyPlayerCount{0};
    bool battleStarted{false};
    bool monsterAlive{false};

    RoomSummary() = default;
    RoomSummary(
        uint32_t roomIdIn,
        uint16_t playerCountIn,
        uint16_t maxPlayersIn,
        uint16_t readyPlayerCountIn = 0,
        bool battleStartedIn = false,
        bool monsterAliveIn = false)
        : roomId(roomIdIn),
          playerCount(playerCountIn),
          maxPlayers(maxPlayersIn),
          readyPlayerCount(readyPlayerCountIn),
          battleStarted(battleStartedIn),
          monsterAlive(monsterAliveIn) {}
};

struct RoomCommandResult {
    bool ok{false};
    RoomCommandError error{RoomCommandError::kNone};
    RoomSummary room{};
    std::vector<uint64_t> playerSessionIds{};

    // 서버는 '상태'가 아니라 '전이 이벤트'를 보고 broadcast 해야 한다.
    // 근데 따지고 보면 multicast도 하지 않나? 둘 다 하니까 broadcast인가?
    bool battleJustStarted{false};
    bool monsterJustSpawned{false};
    bool monsterJustDefeated{false};

    Monster monster{};
    std::vector<Drop> drops{};
    bool lootJustClaimed{false};
    bool lootRejected{false};
    LootRejectReason lootRejectReason{LootRejectReason::kNone};
    uint64_t winnerSessionId{0};
    Drop drop{};
    InventorySnapshot inventory{};

    RoomCommandResult() = default;
    RoomCommandResult(
        bool okIn,
        RoomCommandError errorIn,
        RoomSummary roomIn = RoomSummary(),
        std::vector<uint64_t> playerSessionIdsIn = {},
        bool battleJustStartedIn = false,
        bool monsterJustSpawnedIn = false,
        bool monsterJustDefeatedIn = false,
        Monster monsterIn = Monster(),
        std::vector<Drop> dropsIn = {})
        : ok(okIn),
          error(errorIn),
          room(roomIn),
          playerSessionIds(std::move(playerSessionIdsIn)),
          battleJustStarted(battleJustStartedIn),
          monsterJustSpawned(monsterJustSpawnedIn),
          monsterJustDefeated(monsterJustDefeatedIn),
          monster(monsterIn),
          drops(std::move(dropsIn)) {}
};

class RoomManager {
public:
    explicit RoomManager(
        uint16_t maxPlayersPerRoom = Room::kDefaultMaxPlayers,
        uint16_t maxInventoryWeight = Room::kDefaultMaxInventoryWeight);

    RoomCommandResult createRoom(uint64_t sessionId);
    RoomCommandResult joinRoom(uint64_t sessionId, uint32_t roomId);
    RoomCommandResult leaveRoom(uint64_t sessionId);
    RoomCommandResult markReady(uint64_t sessionId);
    RoomCommandResult spawnMonster(uint32_t roomId);
    RoomCommandResult defeatMonster(uint64_t sessionId, uint32_t monsterId);
    RoomCommandResult claimLoot(uint64_t sessionId, uint32_t dropId);

    std::optional<uint32_t> findRoomIdForSession(uint64_t sessionId) const;   // sessionId와 일치하는 세션이 없으면 std::nullopt 반환
    const Room* findRoom(uint32_t roomId) const;
    std::vector<RoomSummary> roomList() const;
    size_t roomCount() const;

private:
    RoomSummary summarizeRoom(const Room& room) const;

    std::unordered_map<uint32_t, Room> rooms_;
    std::unordered_map<uint64_t, uint32_t> sessionToRoomId_;
    // 성공 후에만 증가
    uint32_t nextRoomId_;
    uint32_t nextMonsterId_;
    uint32_t nextDropId_;
    uint16_t maxPlayersPerRoom_;
    uint16_t maxInventoryWeight_;
};
}  // namespace Game
