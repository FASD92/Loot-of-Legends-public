#include <gtest/gtest.h>

#include <chrono>

#include "Game/RoomManager.hpp"

namespace {
uint64_t currentUnixTimeMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

void expectPosition(
    const Game::MovementPosition* position,
    int32_t expectedX,
    int32_t expectedY) {
    ASSERT_NE(position, nullptr);
    EXPECT_EQ(position->x, expectedX);
    EXPECT_EQ(position->y, expectedY);
}

const Game::MovementPosition* findSnapshotPosition(
    const std::vector<Game::MovementSnapshot>& snapshots,
    uint64_t sessionId) {
    for (const Game::MovementSnapshot& snapshot : snapshots) {
        if (snapshot.sessionId == sessionId) {
            return &snapshot.position;
        }
    }

    return nullptr;
}
}  // namespace

TEST(RoomManagerTests, CreateRoomAssignsUniqueRoomIds) {
    Game::RoomManager manager;

    const Game::RoomCommandResult roomA = manager.createRoom(10);
    const Game::RoomCommandResult roomB = manager.createRoom(20);

    ASSERT_TRUE(roomA.ok);
    ASSERT_TRUE(roomB.ok);
    EXPECT_NE(roomA.room.roomId, roomB.room.roomId);
    EXPECT_EQ(manager.roomCount(), 2u);
}

TEST(RoomManagerTests, JoinRoomAllowsTwoPlayersAndRejectsThirdPlayer) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);

    const Game::RoomCommandResult joined = manager.joinRoom(20, created.room.roomId);
    ASSERT_TRUE(joined.ok);
    EXPECT_EQ(joined.room.playerCount, 2);

    const Game::RoomCommandResult rejected = manager.joinRoom(30, created.room.roomId);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.error, Game::RoomCommandError::kFull);

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_EQ(room->playerCount(), 2);
}

TEST(RoomManagerTests, MovementStateUsesDeterministicSlotSpawnAndLeaveCleanup) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    ASSERT_EQ(room->movementSnapshots().size(), 2u);
    expectPosition(room->findMovementPosition(10), -1000, 0);
    expectPosition(room->findMovementPosition(20), 1000, 0);

    const Game::RoomCommandResult left = manager.leaveRoom(20);
    ASSERT_TRUE(left.ok);

    room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    ASSERT_EQ(room->movementSnapshots().size(), 1u);
    expectPosition(room->findMovementPosition(10), -1000, 0);
    EXPECT_EQ(room->findMovementPosition(20), nullptr);
}

TEST(RoomManagerTests, BattleStartResetsMovementPositionsToSlotSpawn) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);

    ASSERT_TRUE(manager.applyMovement(10, 1000, 0, 1000).ok);
    ASSERT_TRUE(manager.applyMovement(20, -1000, 0, 500).ok);
    const Game::Room* movedRoom = manager.findRoom(created.room.roomId);
    ASSERT_NE(movedRoom, nullptr);
    expectPosition(movedRoom->findMovementPosition(10), 0, 0);
    expectPosition(movedRoom->findMovementPosition(20), 500, 0);

    ASSERT_TRUE(manager.markReady(10).ok);
    const Game::RoomCommandResult started = manager.markReady(20);
    ASSERT_TRUE(started.ok);
    ASSERT_TRUE(started.battleJustStarted);

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    expectPosition(room->findMovementPosition(10), -1000, 0);
    expectPosition(room->findMovementPosition(20), 1000, 0);
}

TEST(RoomManagerTests, ApplyMovementUsesServerSpeedAndElapsedTime) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);

    const Game::MovementCommandResult moved = manager.applyMovement(10, 1000, 0, 500);
    ASSERT_TRUE(moved.ok);
    EXPECT_EQ(moved.roomId, created.room.roomId);
    EXPECT_EQ(moved.previousPosition.x, -1000);
    EXPECT_EQ(moved.previousPosition.y, 0);
    EXPECT_EQ(moved.currentPosition.x, -500);
    EXPECT_EQ(moved.currentPosition.y, 0);

    const Game::MovementCommandResult noElapsed = manager.applyMovement(10, 1000, 0, 0);
    ASSERT_TRUE(noElapsed.ok);
    EXPECT_EQ(noElapsed.previousPosition.x, -500);
    EXPECT_EQ(noElapsed.currentPosition.x, -500);
}

TEST(RoomManagerTests, ApplyMovementAcceptsZeroVectorAsStopIntent) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);

    const Game::MovementCommandResult stopped = manager.applyMovement(10, 0, 0, 1000);
    ASSERT_TRUE(stopped.ok);
    EXPECT_EQ(stopped.previousPosition.x, -1000);
    EXPECT_EQ(stopped.previousPosition.y, 0);
    EXPECT_EQ(stopped.currentPosition.x, -1000);
    EXPECT_EQ(stopped.currentPosition.y, 0);
}

TEST(RoomManagerTests, ApplyMovementNormalizesDirectionWithoutUsingClientMagnitudeAsSpeed) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);

    const Game::MovementCommandResult axis = manager.applyMovement(10, 32767, 0, 1000);
    ASSERT_TRUE(axis.ok);
    EXPECT_EQ(axis.currentPosition.x, 0);
    EXPECT_EQ(axis.currentPosition.y, 0);

    const Game::MovementCommandResult diagonal =
        manager.applyMovement(20, 32767, 32767, 1000);
    ASSERT_TRUE(diagonal.ok);
    EXPECT_EQ(diagonal.currentPosition.x, 1707);
    EXPECT_EQ(diagonal.currentPosition.y, 707);
}

TEST(RoomManagerTests, ApplyMovementClampsToRoomBounds) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);

    const Game::MovementCommandResult minClamped =
        manager.applyMovement(10, -1000, 0, 100000);
    ASSERT_TRUE(minClamped.ok);
    EXPECT_EQ(minClamped.currentPosition.x, Game::Room::kMovementMinPosition);
    EXPECT_EQ(minClamped.currentPosition.y, 0);

    const Game::MovementCommandResult maxClamped =
        manager.applyMovement(10, 1000, 1000, 200000);
    ASSERT_TRUE(maxClamped.ok);
    EXPECT_EQ(maxClamped.currentPosition.x, Game::Room::kMovementMaxPosition);
    EXPECT_EQ(maxClamped.currentPosition.y, Game::Room::kMovementMaxPosition);
}

TEST(RoomManagerTests, ApplyMovementRejectsSessionOutsideRoomWithoutMutating) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    expectPosition(room->findMovementPosition(10), -1000, 0);

    const Game::MovementCommandResult rejected = manager.applyMovement(999, 1000, 0, 1000);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.error, Game::RoomCommandError::kNotInRoom);

    room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    expectPosition(room->findMovementPosition(10), -1000, 0);
}

TEST(RoomManagerTests, RoomRejectsUnknownPlayerMovementWithoutMutatingState) {
    Game::Room room(1);
    ASSERT_TRUE(room.addPlayer(10));

    const Game::MovementApplyResult rejected = room.applyMovement(20, 1000, 0, 1000);
    EXPECT_EQ(rejected.status, Game::MovementApplyStatus::kNoPlayer);
    expectPosition(room.findMovementPosition(10), -1000, 0);
    EXPECT_EQ(room.findMovementPosition(20), nullptr);
}

TEST(RoomManagerTests, CreateRoomRejectsSessionAlreadyInRoom) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);

    const Game::RoomCommandResult duplicate = manager.createRoom(10);
    EXPECT_FALSE(duplicate.ok);
    EXPECT_EQ(duplicate.error, Game::RoomCommandError::kAlreadyInRoom);
}

TEST(RoomManagerTests, LeaveRoomRemovesEmptyRoom) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);

    const Game::RoomCommandResult firstLeave = manager.leaveRoom(20);
    ASSERT_TRUE(firstLeave.ok);
    EXPECT_EQ(firstLeave.room.roomId, created.room.roomId);
    EXPECT_EQ(firstLeave.room.playerCount, 1);
    EXPECT_EQ(manager.roomCount(), 1u);

    const Game::RoomCommandResult secondLeave = manager.leaveRoom(10);
    ASSERT_TRUE(secondLeave.ok);
    EXPECT_EQ(secondLeave.room.roomId, created.room.roomId);
    EXPECT_EQ(secondLeave.room.playerCount, 0);
    EXPECT_EQ(manager.roomCount(), 0u);
    EXPECT_EQ(manager.findRoom(created.room.roomId), nullptr);
}

TEST(RoomManagerTests, RoomListIsSortedByRoomId) {
    Game::RoomManager manager;

    ASSERT_TRUE(manager.createRoom(30).ok);
    ASSERT_TRUE(manager.createRoom(10).ok);
    ASSERT_TRUE(manager.createRoom(20).ok);

    const std::vector<Game::RoomSummary> rooms = manager.roomList();
    ASSERT_EQ(rooms.size(), 3u);
    EXPECT_LT(rooms[0].roomId, rooms[1].roomId);
    EXPECT_LT(rooms[1].roomId, rooms[2].roomId);
}

TEST(RoomManagerTests, MarkReadyStartsBattleOnlyAfterBothPlayersReady) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);

    const Game::RoomCommandResult firstReady = manager.markReady(10);
    ASSERT_TRUE(firstReady.ok);
    EXPECT_EQ(firstReady.room.roomId, created.room.roomId);
    EXPECT_EQ(firstReady.room.readyPlayerCount, 1u);
    EXPECT_FALSE(firstReady.room.battleStarted);
    EXPECT_FALSE(firstReady.battleJustStarted);
    EXPECT_EQ(firstReady.playerSessionIds, (std::vector<uint64_t>{10, 20}));

    const Game::RoomCommandResult duplicateReady = manager.markReady(10);
    ASSERT_TRUE(duplicateReady.ok);
    EXPECT_EQ(duplicateReady.room.readyPlayerCount, 1u);
    EXPECT_FALSE(duplicateReady.room.battleStarted);
    EXPECT_FALSE(duplicateReady.battleJustStarted);

    const Game::RoomCommandResult secondReady = manager.markReady(20);
    ASSERT_TRUE(secondReady.ok);
    EXPECT_EQ(secondReady.room.readyPlayerCount, 2u);
    EXPECT_TRUE(secondReady.room.battleStarted);
    EXPECT_TRUE(secondReady.battleJustStarted);
    EXPECT_EQ(secondReady.playerSessionIds, (std::vector<uint64_t>{10, 20}));

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_EQ(room->readyPlayerCount(), 2u);
    EXPECT_TRUE(room->battleStarted());
}

TEST(RoomManagerTests, MarkReadyRejectsSessionOutsideRoom) {
    Game::RoomManager manager;

    const Game::RoomCommandResult ready = manager.markReady(10);
    EXPECT_FALSE(ready.ok);
    EXPECT_EQ(ready.error, Game::RoomCommandError::kNotInRoom);
}

TEST(RoomManagerTests, JoinRoomResetsReadyStateForAllPlayers) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.markReady(10).ok);

    const Game::RoomCommandResult joined = manager.joinRoom(20, created.room.roomId);
    ASSERT_TRUE(joined.ok);
    EXPECT_EQ(joined.room.readyPlayerCount, 0u);
    EXPECT_FALSE(joined.room.battleStarted);

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_EQ(room->readyPlayerCount(), 0u);
    EXPECT_FALSE(room->battleStarted());
}

TEST(RoomManagerTests, LeaveRoomResetsBattleStateForRemainingPlayer) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);

    const Game::RoomCommandResult left = manager.leaveRoom(20);
    ASSERT_TRUE(left.ok);
    EXPECT_EQ(left.room.playerCount, 1u);
    EXPECT_EQ(left.room.readyPlayerCount, 0u);
    EXPECT_FALSE(left.room.battleStarted);
    EXPECT_EQ(left.playerSessionIds, (std::vector<uint64_t>{10}));

    const Game::RoomCommandResult readyAgain = manager.markReady(10);
    ASSERT_TRUE(readyAgain.ok);
    EXPECT_EQ(readyAgain.room.readyPlayerCount, 1u);
    EXPECT_FALSE(readyAgain.room.battleStarted);
    EXPECT_FALSE(readyAgain.battleJustStarted);
}

TEST(RoomManagerTests, SpawnMonsterAfterBattleStartOnlyOnce) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);

    const Game::RoomCommandResult spawned = manager.spawnMonster(created.room.roomId);
    ASSERT_TRUE(spawned.ok);
    EXPECT_TRUE(spawned.monsterJustSpawned);
    EXPECT_TRUE(spawned.room.monsterAlive);
    EXPECT_EQ(spawned.monster.monsterId, 1u);
    EXPECT_EQ(spawned.monster.monsterTypeId, 1u);
    EXPECT_EQ(spawned.monster.maxHp, 100u);
    EXPECT_TRUE(spawned.monster.alive);
    EXPECT_TRUE(spawned.drops.empty());
    EXPECT_EQ(spawned.playerSessionIds, (std::vector<uint64_t>{10, 20}));

    const Game::RoomCommandResult duplicateSpawn = manager.spawnMonster(created.room.roomId);
    EXPECT_FALSE(duplicateSpawn.ok);
    EXPECT_EQ(duplicateSpawn.error, Game::RoomCommandError::kNotFound);
}

TEST(RoomManagerTests, SpawnMonsterRejectsRoomBeforeBattleStart) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);

    const Game::RoomCommandResult spawned = manager.spawnMonster(created.room.roomId);
    EXPECT_FALSE(spawned.ok);
    EXPECT_EQ(spawned.error, Game::RoomCommandError::kNotFound);
    EXPECT_FALSE(spawned.room.monsterAlive);
}

TEST(RoomManagerTests, DefeatMonsterCreatesSingleDrop) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);
    const Game::RoomCommandResult spawned = manager.spawnMonster(created.room.roomId);
    ASSERT_TRUE(spawned.ok);

    const Game::RoomCommandResult defeated = manager.defeatMonster(10, spawned.monster.monsterId);
    ASSERT_TRUE(defeated.ok);
    EXPECT_TRUE(defeated.monsterJustDefeated);
    EXPECT_FALSE(defeated.room.monsterAlive);
    EXPECT_EQ(defeated.monster.monsterId, spawned.monster.monsterId);
    EXPECT_FALSE(defeated.monster.alive);
    ASSERT_EQ(defeated.drops.size(), 1u);
    EXPECT_EQ(defeated.drops[0].dropId, 1u);
    EXPECT_EQ(defeated.drops[0].itemId, 1001u);
    EXPECT_EQ(defeated.drops[0].quantity, 1u);
    EXPECT_EQ(defeated.playerSessionIds, (std::vector<uint64_t>{10, 20}));
}

TEST(RoomManagerTests, DefeatMonsterRejectsSessionOutsideRoom) {
    Game::RoomManager manager;

    const Game::RoomCommandResult defeated = manager.defeatMonster(10, 1);
    EXPECT_FALSE(defeated.ok);
    EXPECT_EQ(defeated.error, Game::RoomCommandError::kNotInRoom);
}

TEST(RoomManagerTests, DefeatMonsterRejectsWrongOrAlreadyDeadMonster) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);
    const Game::RoomCommandResult spawned = manager.spawnMonster(created.room.roomId);
    ASSERT_TRUE(spawned.ok);

    const Game::RoomCommandResult wrongMonster = manager.defeatMonster(10, spawned.monster.monsterId + 1);
    EXPECT_FALSE(wrongMonster.ok);
    EXPECT_EQ(wrongMonster.error, Game::RoomCommandError::kNotFound);

    const Game::RoomCommandResult defeated = manager.defeatMonster(10, spawned.monster.monsterId);
    ASSERT_TRUE(defeated.ok);
    ASSERT_EQ(defeated.drops.size(), 1u);
    EXPECT_EQ(defeated.drops[0].dropId, 1u);

    const Game::RoomCommandResult alreadyDead = manager.defeatMonster(20, spawned.monster.monsterId);
    EXPECT_FALSE(alreadyDead.ok);
    EXPECT_EQ(alreadyDead.error, Game::RoomCommandError::kNotFound);
}

TEST(RoomManagerTests, CreateCenterDropForSmokeCreatesServerAssignedDrop) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);
    const Game::RoomCommandResult spawned = manager.spawnMonster(created.room.roomId);
    ASSERT_TRUE(spawned.ok);

    const Game::RoomCommandResult dropped = manager.createCenterDropForSmoke(10);
    ASSERT_TRUE(dropped.ok);
    EXPECT_FALSE(dropped.monsterJustDefeated);
    EXPECT_FALSE(dropped.lootJustClaimed);
    EXPECT_FALSE(dropped.lootRejected);
    EXPECT_EQ(dropped.playerSessionIds, (std::vector<uint64_t>{10, 20}));
    ASSERT_EQ(dropped.drops.size(), 1u);
    EXPECT_EQ(dropped.drops[0].dropId, 1u);
    EXPECT_EQ(dropped.drops[0].itemId, 1001u);
    EXPECT_EQ(dropped.drops[0].quantity, 1u);
    EXPECT_FALSE(dropped.drops[0].claimed);
    EXPECT_EQ(dropped.drops[0].ownerSessionId, 0u);
    EXPECT_TRUE(dropped.monster.alive);
    EXPECT_EQ(dropped.monster.monsterId, spawned.monster.monsterId);

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_TRUE(room->hasAliveMonster());
    ASSERT_EQ(room->drops().size(), 1u);
    EXPECT_EQ(room->drops()[0].dropId, dropped.drops[0].dropId);
}

TEST(RoomManagerTests, CreateCenterDropForSmokeRejectsSessionOutsideRoomWithoutConsumingDropId) {
    Game::RoomManager manager;

    const Game::RoomCommandResult rejected = manager.createCenterDropForSmoke(999);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.error, Game::RoomCommandError::kNotInRoom);

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);

    const Game::RoomCommandResult dropped = manager.createCenterDropForSmoke(10);
    ASSERT_TRUE(dropped.ok);
    ASSERT_EQ(dropped.drops.size(), 1u);
    EXPECT_EQ(dropped.drops[0].dropId, 1u);
}

TEST(RoomManagerTests, CreateCenterDropForSmokeRejectsBeforeBattleStartWithoutConsumingDropId) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);

    const Game::RoomCommandResult rejected = manager.createCenterDropForSmoke(10);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.error, Game::RoomCommandError::kNotFound);
    EXPECT_FALSE(rejected.room.battleStarted);

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_TRUE(room->drops().empty());

    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);

    const Game::RoomCommandResult dropped = manager.createCenterDropForSmoke(10);
    ASSERT_TRUE(dropped.ok);
    ASSERT_EQ(dropped.drops.size(), 1u);
    EXPECT_EQ(dropped.drops[0].dropId, 1u);
}

TEST(RoomManagerTests, CreateCenterDropForSmokeRejectsExistingDropWithoutConsumingDropId) {
    Game::RoomManager manager;

    const Game::RoomCommandResult roomA = manager.createRoom(10);
    ASSERT_TRUE(roomA.ok);
    ASSERT_TRUE(manager.joinRoom(20, roomA.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);

    const Game::RoomCommandResult firstDrop = manager.createCenterDropForSmoke(10);
    ASSERT_TRUE(firstDrop.ok);
    ASSERT_EQ(firstDrop.drops.size(), 1u);
    EXPECT_EQ(firstDrop.drops[0].dropId, 1u);

    const Game::RoomCommandResult duplicateDrop = manager.createCenterDropForSmoke(20);
    EXPECT_FALSE(duplicateDrop.ok);
    EXPECT_EQ(duplicateDrop.error, Game::RoomCommandError::kNotFound);

    const Game::RoomCommandResult roomB = manager.createRoom(30);
    ASSERT_TRUE(roomB.ok);
    ASSERT_TRUE(manager.joinRoom(40, roomB.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(30).ok);
    ASSERT_TRUE(manager.markReady(40).ok);

    const Game::RoomCommandResult secondRoomDrop = manager.createCenterDropForSmoke(30);
    ASSERT_TRUE(secondRoomDrop.ok);
    ASSERT_EQ(secondRoomDrop.drops.size(), 1u);
    EXPECT_EQ(secondRoomDrop.drops[0].dropId, 2u);
}

TEST(RoomManagerTests, CreateCenterDropForSmokeUsesExistingClaimLootAuthority) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);
    const Game::RoomCommandResult dropped = manager.createCenterDropForSmoke(10);
    ASSERT_TRUE(dropped.ok);
    ASSERT_EQ(dropped.drops.size(), 1u);

    const Game::RoomCommandResult claimed = manager.claimLoot(10, dropped.drops[0].dropId);
    ASSERT_TRUE(claimed.ok);
    EXPECT_TRUE(claimed.lootJustClaimed);
    EXPECT_FALSE(claimed.lootRejected);
    EXPECT_EQ(claimed.winnerSessionId, 10u);
    EXPECT_TRUE(claimed.drop.claimed);
    EXPECT_EQ(claimed.drop.ownerSessionId, 10u);
    EXPECT_EQ(claimed.inventory.sessionId, 10u);
    EXPECT_EQ(claimed.inventory.currentWeight, 1u);
    ASSERT_EQ(claimed.inventory.entries.size(), 1u);
    EXPECT_EQ(claimed.inventory.entries[0].itemId, 1001u);
    EXPECT_EQ(claimed.inventory.entries[0].quantity, 1u);

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    const Game::InventorySnapshot* loserInventory = room->findInventory(20);
    ASSERT_NE(loserInventory, nullptr);
    EXPECT_EQ(loserInventory->currentWeight, 0u);
    EXPECT_TRUE(loserInventory->entries.empty());
}

TEST(RoomManagerTests, PlacePlayersAroundCenterDropForSmokePlacesAroundCenterDrop) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);
    ASSERT_TRUE(manager.createCenterDropForSmoke(10).ok);

    const Game::SmokePlayerPlacementResult placed =
        manager.placePlayersAroundCenterDropForSmoke(10);
    ASSERT_TRUE(placed.ok);
    EXPECT_EQ(placed.error, Game::RoomCommandError::kNone);
    EXPECT_EQ(placed.roomId, created.room.roomId);
    EXPECT_EQ(placed.playerSessionIds, (std::vector<uint64_t>{10, 20}));
    ASSERT_EQ(placed.movementSnapshots.size(), 2u);
    expectPosition(findSnapshotPosition(placed.movementSnapshots, 10), -1000, 0);
    expectPosition(findSnapshotPosition(placed.movementSnapshots, 20), 1000, 0);

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    expectPosition(room->findMovementPosition(10), -1000, 0);
    expectPosition(room->findMovementPosition(20), 1000, 0);
    ASSERT_EQ(room->drops().size(), 1u);
    EXPECT_FALSE(room->drops()[0].claimed);
}

TEST(RoomManagerTests, PlacePlayersAroundCenterDropForSmokeOverwritesMovedPositions) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);
    ASSERT_TRUE(manager.createCenterDropForSmoke(10).ok);
    ASSERT_TRUE(manager.applyMovement(10, 1000, 1000, 1000).ok);
    ASSERT_TRUE(manager.applyMovement(20, -1000, 1000, 1000).ok);

    const Game::Room* movedRoom = manager.findRoom(created.room.roomId);
    ASSERT_NE(movedRoom, nullptr);
    EXPECT_NE(movedRoom->findMovementPosition(10)->x, -1000);
    EXPECT_NE(movedRoom->findMovementPosition(20)->x, 1000);

    const Game::SmokePlayerPlacementResult placed =
        manager.placePlayersAroundCenterDropForSmoke(20);
    ASSERT_TRUE(placed.ok);
    expectPosition(findSnapshotPosition(placed.movementSnapshots, 10), -1000, 0);
    expectPosition(findSnapshotPosition(placed.movementSnapshots, 20), 1000, 0);

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    expectPosition(room->findMovementPosition(10), -1000, 0);
    expectPosition(room->findMovementPosition(20), 1000, 0);
}

TEST(RoomManagerTests, PlacePlayersAroundCenterDropForSmokeRejectsNoRoomWithoutMutating) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);
    ASSERT_TRUE(manager.createCenterDropForSmoke(10).ok);
    ASSERT_TRUE(manager.applyMovement(10, 1000, 0, 1000).ok);

    const Game::SmokePlayerPlacementResult rejected =
        manager.placePlayersAroundCenterDropForSmoke(999);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.error, Game::RoomCommandError::kNotInRoom);

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    expectPosition(room->findMovementPosition(10), 0, 0);
    expectPosition(room->findMovementPosition(20), 1000, 0);
}

TEST(RoomManagerTests, PlacePlayersAroundCenterDropForSmokeRejectsBeforeBattleStartWithoutMutating) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.applyMovement(10, 1000, 0, 1000).ok);

    const Game::SmokePlayerPlacementResult rejected =
        manager.placePlayersAroundCenterDropForSmoke(10);
    EXPECT_FALSE(rejected.ok);
    EXPECT_EQ(rejected.error, Game::RoomCommandError::kNotFound);
    EXPECT_EQ(rejected.roomId, created.room.roomId);

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    expectPosition(room->findMovementPosition(10), 0, 0);
    expectPosition(room->findMovementPosition(20), 1000, 0);
}

TEST(RoomManagerTests, PlacePlayersAroundCenterDropForSmokeRequiresUnclaimedDrop) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);

    const Game::SmokePlayerPlacementResult noDrop =
        manager.placePlayersAroundCenterDropForSmoke(10);
    EXPECT_FALSE(noDrop.ok);
    EXPECT_EQ(noDrop.error, Game::RoomCommandError::kNotFound);

    const Game::RoomCommandResult dropped = manager.createCenterDropForSmoke(10);
    ASSERT_TRUE(dropped.ok);
    ASSERT_EQ(dropped.drops.size(), 1u);
    ASSERT_TRUE(manager.claimLoot(10, dropped.drops[0].dropId).ok);
    ASSERT_TRUE(manager.applyMovement(10, 1000, 0, 1000).ok);

    const Game::SmokePlayerPlacementResult claimedDrop =
        manager.placePlayersAroundCenterDropForSmoke(20);
    EXPECT_FALSE(claimedDrop.ok);
    EXPECT_EQ(claimedDrop.error, Game::RoomCommandError::kNotFound);

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    expectPosition(room->findMovementPosition(10), 0, 0);
    expectPosition(room->findMovementPosition(20), 1000, 0);
    ASSERT_EQ(room->drops().size(), 1u);
    EXPECT_TRUE(room->drops()[0].claimed);
    EXPECT_EQ(room->drops()[0].ownerSessionId, 10u);
}

TEST(RoomManagerTests, PlacePlayersAroundCenterDropForSmokeDoesNotChangeLootOrMonsterState) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);
    const Game::RoomCommandResult spawned = manager.spawnMonster(created.room.roomId);
    ASSERT_TRUE(spawned.ok);
    const Game::RoomCommandResult dropped = manager.createCenterDropForSmoke(10);
    ASSERT_TRUE(dropped.ok);
    ASSERT_EQ(dropped.drops.size(), 1u);

    const Game::SmokePlayerPlacementResult placed =
        manager.placePlayersAroundCenterDropForSmoke(10);
    ASSERT_TRUE(placed.ok);

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_TRUE(room->hasAliveMonster());
    EXPECT_EQ(room->monster().monsterId, spawned.monster.monsterId);
    ASSERT_EQ(room->drops().size(), 1u);
    EXPECT_EQ(room->drops()[0].dropId, dropped.drops[0].dropId);
    EXPECT_FALSE(room->drops()[0].claimed);
    EXPECT_EQ(room->drops()[0].ownerSessionId, 0u);

    const Game::InventorySnapshot* inventoryA = room->findInventory(10);
    ASSERT_NE(inventoryA, nullptr);
    EXPECT_EQ(inventoryA->currentWeight, 0u);
    EXPECT_TRUE(inventoryA->entries.empty());
    const Game::InventorySnapshot* inventoryB = room->findInventory(20);
    ASSERT_NE(inventoryB, nullptr);
    EXPECT_EQ(inventoryB->currentWeight, 0u);
    EXPECT_TRUE(inventoryB->entries.empty());
}

TEST(RoomManagerTests, RoomRejectsDefeatMonsterWithZeroQuantityDrop) {
    Game::Room room(1);

    ASSERT_TRUE(room.addPlayer(10));
    ASSERT_TRUE(room.addPlayer(20));
    ASSERT_TRUE(room.markReady(10));
    ASSERT_TRUE(room.markReady(20));
    ASSERT_TRUE(room.tryStartBattle());
    ASSERT_TRUE(room.spawnMonster(1, 1, 100));

    EXPECT_FALSE(room.defeatMonster(1, 1, 1001, 0));
    EXPECT_TRUE(room.hasAliveMonster());
    EXPECT_TRUE(room.drops().empty());
}

// 이 테스트는 2인 MVP 때만 적용한다.
// 추후 10인 이상의 방을 고려할 때는 다른 로직을 고려할 것임.
TEST(RoomManagerTests, LeaveRoomResetsMonsterAndDropState) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);
    const Game::RoomCommandResult spawned = manager.spawnMonster(created.room.roomId);
    ASSERT_TRUE(spawned.ok);
    ASSERT_TRUE(manager.defeatMonster(10, spawned.monster.monsterId).ok);

    const Game::RoomCommandResult left = manager.leaveRoom(20);
    ASSERT_TRUE(left.ok);
    EXPECT_FALSE(left.room.battleStarted);
    EXPECT_FALSE(left.room.monsterAlive);

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_FALSE(room->battleStarted());
    EXPECT_FALSE(room->hasAliveMonster());
    EXPECT_TRUE(room->drops().empty());
}

TEST(RoomManagerTests, ClaimLootAssignsSingleOwnerAndInventory) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);
    const Game::RoomCommandResult spawned = manager.spawnMonster(created.room.roomId);
    ASSERT_TRUE(spawned.ok);
    const Game::RoomCommandResult defeated = manager.defeatMonster(10, spawned.monster.monsterId);
    ASSERT_TRUE(defeated.ok);
    ASSERT_EQ(defeated.drops.size(), 1u);

    const Game::RoomCommandResult claimed = manager.claimLoot(10, defeated.drops[0].dropId);
    ASSERT_TRUE(claimed.ok);
    EXPECT_TRUE(claimed.lootJustClaimed);
    EXPECT_FALSE(claimed.lootRejected);
    EXPECT_EQ(claimed.lootRejectReason, Game::LootRejectReason::kNone);
    EXPECT_EQ(claimed.winnerSessionId, 10u);
    EXPECT_TRUE(claimed.drop.claimed);
    EXPECT_EQ(claimed.drop.ownerSessionId, 10u);
    EXPECT_EQ(claimed.inventory.sessionId, 10u);
    EXPECT_EQ(claimed.inventory.currentWeight, 1u);
    EXPECT_EQ(claimed.inventory.maxWeight, Game::Room::kDefaultMaxInventoryWeight);
    ASSERT_EQ(claimed.inventory.entries.size(), 1u);
    EXPECT_EQ(claimed.inventory.entries[0].itemId, 1001u);
    EXPECT_EQ(claimed.inventory.entries[0].quantity, 1u);

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    ASSERT_EQ(room->drops().size(), 1u);
    EXPECT_TRUE(room->drops()[0].claimed);
    EXPECT_EQ(room->drops()[0].ownerSessionId, 10u);

    const Game::InventorySnapshot* winnerInventory = room->findInventory(10);
    ASSERT_NE(winnerInventory, nullptr);
    EXPECT_EQ(winnerInventory->currentWeight, 1u);
    ASSERT_EQ(winnerInventory->entries.size(), 1u);
    EXPECT_EQ(winnerInventory->entries[0].itemId, 1001u);
    EXPECT_EQ(winnerInventory->entries[0].quantity, 1u);
}

TEST(RoomManagerTests, ClaimLootRejectsAlreadyClaimedDropWithoutChangingOwner) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);
    const Game::RoomCommandResult spawned = manager.spawnMonster(created.room.roomId);
    ASSERT_TRUE(spawned.ok);
    const Game::RoomCommandResult defeated = manager.defeatMonster(10, spawned.monster.monsterId);
    ASSERT_TRUE(defeated.ok);
    ASSERT_EQ(defeated.drops.size(), 1u);
    ASSERT_TRUE(manager.claimLoot(10, defeated.drops[0].dropId).ok);

    const Game::RoomCommandResult rejected = manager.claimLoot(20, defeated.drops[0].dropId);
    ASSERT_TRUE(rejected.ok);
    EXPECT_FALSE(rejected.lootJustClaimed);
    EXPECT_TRUE(rejected.lootRejected);
    EXPECT_EQ(rejected.lootRejectReason, Game::LootRejectReason::kAlreadyClaimed);
    EXPECT_EQ(rejected.winnerSessionId, 10u);
    EXPECT_TRUE(rejected.drop.claimed);
    EXPECT_EQ(rejected.drop.ownerSessionId, 10u);
    EXPECT_EQ(rejected.inventory.sessionId, 20u);
    EXPECT_EQ(rejected.inventory.currentWeight, 0u);
    EXPECT_TRUE(rejected.inventory.entries.empty());

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    ASSERT_EQ(room->drops().size(), 1u);
    EXPECT_EQ(room->drops()[0].ownerSessionId, 10u);
}

TEST(RoomManagerTests, ClaimLootRejectsSessionOutsideRoom) {
    Game::RoomManager manager;

    const Game::RoomCommandResult claimed = manager.claimLoot(10, 1);
    EXPECT_FALSE(claimed.ok);
    EXPECT_EQ(claimed.error, Game::RoomCommandError::kNotInRoom);
}

TEST(RoomManagerTests, ClaimLootRejectsUnknownDrop) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);
    const Game::RoomCommandResult spawned = manager.spawnMonster(created.room.roomId);
    ASSERT_TRUE(spawned.ok);
    ASSERT_TRUE(manager.defeatMonster(10, spawned.monster.monsterId).ok);

    const Game::RoomCommandResult claimed = manager.claimLoot(10, 999);
    EXPECT_FALSE(claimed.ok);
    EXPECT_EQ(claimed.error, Game::RoomCommandError::kNotFound);
}

TEST(RoomManagerTests, ClaimLootRejectsOverweightWithoutClaimingDrop) {
    Game::RoomManager manager(Game::Room::kDefaultMaxPlayers, 0);

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);
    const Game::RoomCommandResult spawned = manager.spawnMonster(created.room.roomId);
    ASSERT_TRUE(spawned.ok);
    const Game::RoomCommandResult defeated = manager.defeatMonster(10, spawned.monster.monsterId);
    ASSERT_TRUE(defeated.ok);
    ASSERT_EQ(defeated.drops.size(), 1u);

    const Game::RoomCommandResult rejected = manager.claimLoot(10, defeated.drops[0].dropId);
    ASSERT_TRUE(rejected.ok);
    EXPECT_FALSE(rejected.lootJustClaimed);
    EXPECT_TRUE(rejected.lootRejected);
    EXPECT_EQ(rejected.lootRejectReason, Game::LootRejectReason::kOverweight);
    EXPECT_EQ(rejected.winnerSessionId, 0u);
    EXPECT_FALSE(rejected.drop.claimed);
    EXPECT_EQ(rejected.drop.ownerSessionId, 0u);
    EXPECT_EQ(rejected.inventory.currentWeight, 0u);
    EXPECT_TRUE(rejected.inventory.entries.empty());

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    ASSERT_EQ(room->drops().size(), 1u);
    EXPECT_FALSE(room->drops()[0].claimed);
    EXPECT_EQ(room->drops()[0].ownerSessionId, 0u);
}

TEST(RoomManagerTests, LeaveRoomResetsLootOwnershipAndInventories) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);
    const Game::RoomCommandResult spawned = manager.spawnMonster(created.room.roomId);
    ASSERT_TRUE(spawned.ok);
    const Game::RoomCommandResult defeated = manager.defeatMonster(10, spawned.monster.monsterId);
    ASSERT_TRUE(defeated.ok);
    ASSERT_EQ(defeated.drops.size(), 1u);
    ASSERT_TRUE(manager.claimLoot(10, defeated.drops[0].dropId).ok);

    const Game::RoomCommandResult left = manager.leaveRoom(20);
    ASSERT_TRUE(left.ok);
    EXPECT_FALSE(left.room.battleStarted);
    EXPECT_FALSE(left.room.monsterAlive);

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_TRUE(room->drops().empty());

    const Game::InventorySnapshot* remainingInventory = room->findInventory(10);
    ASSERT_NE(remainingInventory, nullptr);
    EXPECT_EQ(remainingInventory->currentWeight, 0u);
    EXPECT_TRUE(remainingInventory->entries.empty());
}

TEST(RoomManagerTests, BuildSettlementResultIncludesClaimedInventoryDeltas) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    ASSERT_TRUE(manager.markReady(10).ok);
    ASSERT_TRUE(manager.markReady(20).ok);
    const Game::RoomCommandResult spawned = manager.spawnMonster(created.room.roomId);
    ASSERT_TRUE(spawned.ok);
    const Game::RoomCommandResult defeated = manager.defeatMonster(10, spawned.monster.monsterId);
    ASSERT_TRUE(defeated.ok);
    ASSERT_EQ(defeated.drops.size(), 1u);
    ASSERT_TRUE(manager.claimLoot(10, defeated.drops[0].dropId).ok);

    const uint64_t finishedAtMs = currentUnixTimeMs();
    const Game::SettlementCommandResult result =
        manager.buildSettlementResult(10, finishedAtMs);

    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.error, Game::RoomCommandError::kNone);
    EXPECT_FALSE(result.settlement.settlementId.empty());
    EXPECT_EQ(result.settlement.sessionId, 10u);
    EXPECT_EQ(result.settlement.accountId, 10u);
    EXPECT_EQ(result.settlement.roomId, created.room.roomId);
    EXPECT_LE(result.settlement.startedAtUnixMs, result.settlement.finishedAtUnixMs);
    EXPECT_EQ(result.settlement.finishedAtUnixMs, finishedAtMs);
    EXPECT_EQ(result.settlement.goldDelta, 0);
    EXPECT_EQ(result.settlement.reason, Game::SettlementReason::kNormal);
    ASSERT_EQ(result.settlement.inventoryDeltas.size(), 1u);
    EXPECT_EQ(result.settlement.inventoryDeltas[0].itemId, defeated.drops[0].itemId);
    EXPECT_EQ(result.settlement.inventoryDeltas[0].quantityDelta, defeated.drops[0].quantity);
    EXPECT_EQ(result.settlement.inventoryDeltas[0].sourceDropId, defeated.drops[0].dropId);
}

TEST(RoomManagerTests, RepeatedBuildSettlementResultReturnsSamePayload) {
    Game::RoomManager manager;

    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);

    const uint64_t firstFinishedAtMs = currentUnixTimeMs();
    const Game::SettlementCommandResult first =
        manager.buildSettlementResult(10, firstFinishedAtMs);
    ASSERT_TRUE(first.ok);

    const Game::SettlementCommandResult second =
        manager.buildSettlementResult(10, firstFinishedAtMs + 1000);
    ASSERT_TRUE(second.ok);

    EXPECT_EQ(second.settlement.settlementId, first.settlement.settlementId);
    EXPECT_EQ(second.settlement.sessionId, first.settlement.sessionId);
    EXPECT_EQ(second.settlement.accountId, first.settlement.accountId);
    EXPECT_EQ(second.settlement.roomId, first.settlement.roomId);
    EXPECT_EQ(second.settlement.startedAtUnixMs, first.settlement.startedAtUnixMs);
    EXPECT_EQ(second.settlement.finishedAtUnixMs, first.settlement.finishedAtUnixMs);
    EXPECT_EQ(second.settlement.goldDelta, first.settlement.goldDelta);
    EXPECT_EQ(second.settlement.reason, first.settlement.reason);
    EXPECT_EQ(second.settlement.inventoryDeltas.size(), first.settlement.inventoryDeltas.size());
}

TEST(RoomManagerTests, BuildSettlementResultRejectsSessionOutsideRoom) {
    Game::RoomManager manager;

    const Game::SettlementCommandResult result =
        manager.buildSettlementResult(10, currentUnixTimeMs());

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, Game::RoomCommandError::kNotInRoom);
}
