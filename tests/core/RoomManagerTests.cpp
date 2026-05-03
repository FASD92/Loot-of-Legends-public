#include <gtest/gtest.h>

#include "Game/RoomManager.hpp"

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
