#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Game/RoomActor.hpp"
#include "Game/RoomEventDispatcher.hpp"
#include "Game/RoomEventQueue.hpp"

namespace {
void startTwoPlayerBattle(Game::RoomManager& manager, uint64_t hostSessionId, uint64_t guestSessionId) {
    ASSERT_TRUE(manager.markReady(hostSessionId).ok);
    ASSERT_TRUE(manager.markReady(guestSessionId).ok);
    const Game::RoomCommandResult started = manager.hostStartBattle(hostSessionId);
    ASSERT_TRUE(started.ok);
    ASSERT_TRUE(started.battleJustStarted);
}
}  // namespace

TEST(RoomActorTests, AppliesReadyEventsThroughRoomManager) {
    Game::RoomManager manager;
    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    const Game::RoomActor actor(created.room.roomId);

    const Game::RoomEventApplyResult firstReady =
        actor.apply(manager, Game::makeReadyRoomEvent(10, created.room.roomId));
    ASSERT_EQ(firstReady.status, Game::RoomEventApplyStatus::kApplied);
    EXPECT_TRUE(firstReady.commandResult.ok);
    EXPECT_FALSE(firstReady.commandResult.battleJustStarted);
    EXPECT_EQ(firstReady.commandResult.room.readyPlayerCount, 1u);

    const Game::RoomEventApplyResult secondReady =
        actor.apply(manager, Game::makeReadyRoomEvent(20, created.room.roomId));
    ASSERT_EQ(secondReady.status, Game::RoomEventApplyStatus::kApplied);
    EXPECT_TRUE(secondReady.commandResult.ok);
    EXPECT_FALSE(secondReady.commandResult.battleJustStarted);
    EXPECT_EQ(secondReady.commandResult.room.readyPlayerCount, 2u);

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_FALSE(room->battleStarted());

    const Game::RoomCommandResult started = manager.hostStartBattle(10);
    ASSERT_TRUE(started.ok);
    EXPECT_TRUE(started.battleJustStarted);
    EXPECT_TRUE(room->battleStarted());
}

TEST(RoomActorTests, AppliesMonsterDeathThroughRoomManager) {
    Game::RoomManager manager;
    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    startTwoPlayerBattle(manager, 10, 20);
    const Game::RoomCommandResult spawned = manager.spawnMonster(created.room.roomId);
    ASSERT_TRUE(spawned.ok);
    const Game::RoomActor actor(created.room.roomId);

    const Game::RoomEventApplyResult defeated = actor.apply(
        manager,
        Game::makeMonsterDeathRoomEvent(
            10,
            created.room.roomId,
            spawned.monster.monsterId));

    ASSERT_EQ(defeated.status, Game::RoomEventApplyStatus::kApplied);
    EXPECT_TRUE(defeated.commandResult.ok);
    EXPECT_TRUE(defeated.commandResult.monsterJustDefeated);
    EXPECT_FALSE(defeated.commandResult.room.monsterAlive);
    ASSERT_EQ(defeated.commandResult.drops.size(), 1u);
    EXPECT_EQ(defeated.commandResult.drops[0].dropId, 1u);
}

TEST(RoomActorTests, AppliesAttackThroughRoomManagerUntilMonsterDeathScatter) {
    Game::RoomManager manager;
    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    startTwoPlayerBattle(manager, 10, 20);
    const Game::RoomCommandResult spawned = manager.spawnMonster(created.room.roomId);
    ASSERT_TRUE(spawned.ok);

    const Game::RoomActor actor(created.room.roomId);
    const Game::RoomEvent attack =
        Game::makeAttackRoomEvent(10, created.room.roomId, spawned.monster.monsterId);

    const Game::RoomEventApplyResult damaged = actor.apply(manager, attack);
    ASSERT_EQ(damaged.status, Game::RoomEventApplyStatus::kApplied);
    ASSERT_TRUE(damaged.commandResult.ok);
    EXPECT_FALSE(damaged.commandResult.monsterJustDefeated);
    EXPECT_TRUE(damaged.commandResult.monster.alive);
    EXPECT_EQ(
        damaged.commandResult.monster.currentHp,
        spawned.monster.maxHp - Game::Room::kAttackDamage);

    ASSERT_EQ(actor.apply(manager, attack).status, Game::RoomEventApplyStatus::kApplied);
    ASSERT_EQ(actor.apply(manager, attack).status, Game::RoomEventApplyStatus::kApplied);
    const Game::RoomEventApplyResult defeated = actor.apply(manager, attack);

    ASSERT_EQ(defeated.status, Game::RoomEventApplyStatus::kApplied);
    ASSERT_TRUE(defeated.commandResult.ok);
    EXPECT_TRUE(defeated.commandResult.monsterJustDefeated);
    EXPECT_FALSE(defeated.commandResult.monster.alive);
    EXPECT_EQ(defeated.commandResult.monster.currentHp, 0u);
    EXPECT_GT(defeated.commandResult.scatterSeed, 0u);
    ASSERT_EQ(defeated.commandResult.drops.size(), 1u);
    EXPECT_GT(defeated.commandResult.drops[0].dropId, 0u);

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_FALSE(room->hasAliveMonster());
    ASSERT_EQ(room->drops().size(), 1u);
    EXPECT_EQ(room->drops()[0].dropId, defeated.commandResult.drops[0].dropId);
}

TEST(RoomActorTests, AppliesQueuedClickLootEventsWithoutChangingOwnershipInvariant) {
    Game::RoomManager manager;
    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    startTwoPlayerBattle(manager, 10, 20);
    const Game::RoomCommandResult spawned = manager.spawnMonster(created.room.roomId);
    ASSERT_TRUE(spawned.ok);
    const Game::RoomCommandResult defeated =
        manager.defeatMonster(10, spawned.monster.monsterId);
    ASSERT_TRUE(defeated.ok);
    ASSERT_EQ(defeated.drops.size(), 1u);

    Game::RoomEventQueue queue(2);
    ASSERT_EQ(
        queue.enqueue(Game::makeClickLootRoomEvent(
            10,
            created.room.roomId,
            defeated.drops[0].dropId)),
        Game::RoomEventQueueEnqueueResult::kEnqueued);
    ASSERT_EQ(
        queue.enqueue(Game::makeClickLootRoomEvent(
            20,
            created.room.roomId,
            defeated.drops[0].dropId)),
        Game::RoomEventQueueEnqueueResult::kEnqueued);
    const Game::RoomActor actor(created.room.roomId);

    Game::RoomEvent event;
    ASSERT_TRUE(queue.tryDequeue(event));
    const Game::RoomEventApplyResult firstClaim = actor.apply(manager, event);
    ASSERT_EQ(firstClaim.status, Game::RoomEventApplyStatus::kApplied);
    EXPECT_TRUE(firstClaim.commandResult.lootJustClaimed);
    EXPECT_FALSE(firstClaim.commandResult.lootRejected);
    EXPECT_EQ(firstClaim.commandResult.winnerSessionId, 10u);

    ASSERT_TRUE(queue.tryDequeue(event));
    const Game::RoomEventApplyResult secondClaim = actor.apply(manager, event);
    ASSERT_EQ(secondClaim.status, Game::RoomEventApplyStatus::kApplied);
    EXPECT_FALSE(secondClaim.commandResult.lootJustClaimed);
    EXPECT_TRUE(secondClaim.commandResult.lootRejected);
    EXPECT_EQ(
        secondClaim.commandResult.lootRejectReason,
        Game::LootRejectReason::kAlreadyClaimed);
    EXPECT_EQ(secondClaim.commandResult.winnerSessionId, 10u);

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    ASSERT_EQ(room->drops().size(), 1u);
    EXPECT_TRUE(room->drops()[0].claimed);
    EXPECT_EQ(room->drops()[0].ownerSessionId, 10u);
}

TEST(RoomActorTests, CenterDropPlacementQueuedClickLootKeepsSingleWinner) {
    Game::RoomManager manager;
    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    startTwoPlayerBattle(manager, 10, 20);

    const Game::RoomCommandResult dropped = manager.createCenterDropForSmoke(10);
    ASSERT_TRUE(dropped.ok);
    ASSERT_EQ(dropped.drops.size(), 1u);
    const uint32_t dropId = dropped.drops[0].dropId;
    const uint32_t itemId = dropped.drops[0].itemId;
    const uint16_t quantity = dropped.drops[0].quantity;

    const Game::SmokePlayerPlacementResult placed =
        manager.placePlayersAroundCenterDropForSmoke(10);
    ASSERT_TRUE(placed.ok);
    ASSERT_EQ(placed.movementSnapshots.size(), 2u);
    EXPECT_EQ(placed.movementSnapshots[0].sessionId, 10u);
    EXPECT_EQ(placed.movementSnapshots[0].position.x, -Game::Room::kMovementScale);
    EXPECT_EQ(placed.movementSnapshots[0].position.y, 0);
    EXPECT_EQ(placed.movementSnapshots[1].sessionId, 20u);
    EXPECT_EQ(placed.movementSnapshots[1].position.x, Game::Room::kMovementScale);
    EXPECT_EQ(placed.movementSnapshots[1].position.y, 0);

    Game::RoomEventQueue queue(2);
    ASSERT_EQ(
        queue.enqueue(Game::makeClickLootRoomEvent(10, created.room.roomId, dropId)),
        Game::RoomEventQueueEnqueueResult::kEnqueued);
    ASSERT_EQ(
        queue.enqueue(Game::makeClickLootRoomEvent(20, created.room.roomId, dropId)),
        Game::RoomEventQueueEnqueueResult::kEnqueued);

    const Game::RoomActor actor(created.room.roomId);
    Game::RoomEvent event;
    ASSERT_TRUE(queue.tryDequeue(event));
    const Game::RoomEventApplyResult firstClaim = actor.apply(manager, event);
    ASSERT_EQ(firstClaim.status, Game::RoomEventApplyStatus::kApplied);
    ASSERT_TRUE(firstClaim.commandResult.ok);
    EXPECT_TRUE(firstClaim.commandResult.lootJustClaimed);
    EXPECT_FALSE(firstClaim.commandResult.lootRejected);
    EXPECT_EQ(firstClaim.commandResult.winnerSessionId, 10u);
    EXPECT_EQ(firstClaim.commandResult.drop.dropId, dropId);
    EXPECT_EQ(firstClaim.commandResult.drop.itemId, itemId);
    EXPECT_EQ(firstClaim.commandResult.drop.quantity, quantity);
    EXPECT_TRUE(firstClaim.commandResult.drop.claimed);
    EXPECT_EQ(firstClaim.commandResult.drop.ownerSessionId, 10u);
    EXPECT_EQ(firstClaim.commandResult.inventory.sessionId, 10u);
    EXPECT_EQ(firstClaim.commandResult.inventory.currentWeight, quantity);
    ASSERT_EQ(firstClaim.commandResult.inventory.entries.size(), 1u);
    EXPECT_EQ(firstClaim.commandResult.inventory.entries[0].itemId, itemId);
    EXPECT_EQ(firstClaim.commandResult.inventory.entries[0].quantity, quantity);

    ASSERT_TRUE(queue.tryDequeue(event));
    const Game::RoomEventApplyResult secondClaim = actor.apply(manager, event);
    ASSERT_EQ(secondClaim.status, Game::RoomEventApplyStatus::kApplied);
    ASSERT_TRUE(secondClaim.commandResult.ok);
    EXPECT_FALSE(secondClaim.commandResult.lootJustClaimed);
    EXPECT_TRUE(secondClaim.commandResult.lootRejected);
    EXPECT_EQ(secondClaim.commandResult.lootRejectReason, Game::LootRejectReason::kAlreadyClaimed);
    EXPECT_EQ(secondClaim.commandResult.winnerSessionId, 10u);
    EXPECT_EQ(secondClaim.commandResult.drop.dropId, dropId);
    EXPECT_TRUE(secondClaim.commandResult.drop.claimed);
    EXPECT_EQ(secondClaim.commandResult.drop.ownerSessionId, 10u);
    EXPECT_EQ(secondClaim.commandResult.inventory.sessionId, 20u);
    EXPECT_EQ(secondClaim.commandResult.inventory.currentWeight, 0u);
    EXPECT_TRUE(secondClaim.commandResult.inventory.entries.empty());
    EXPECT_FALSE(queue.tryDequeue(event));

    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    ASSERT_EQ(room->drops().size(), 1u);
    EXPECT_TRUE(room->drops()[0].claimed);
    EXPECT_EQ(room->drops()[0].ownerSessionId, 10u);
    EXPECT_EQ(room->drops()[0].itemId, itemId);
    EXPECT_EQ(room->drops()[0].quantity, quantity);

    const Game::InventorySnapshot* winnerInventory = room->findInventory(10);
    ASSERT_NE(winnerInventory, nullptr);
    EXPECT_EQ(winnerInventory->currentWeight, quantity);
    ASSERT_EQ(winnerInventory->entries.size(), 1u);
    EXPECT_EQ(winnerInventory->entries[0].itemId, itemId);
    EXPECT_EQ(winnerInventory->entries[0].quantity, quantity);

    const Game::InventorySnapshot* loserInventory = room->findInventory(20);
    ASSERT_NE(loserInventory, nullptr);
    EXPECT_EQ(loserInventory->currentWeight, 0u);
    EXPECT_TRUE(loserInventory->entries.empty());

    const Game::MovementPosition* winnerPosition = room->findMovementPosition(10);
    ASSERT_NE(winnerPosition, nullptr);
    EXPECT_EQ(winnerPosition->x, -Game::Room::kMovementScale);
    EXPECT_EQ(winnerPosition->y, 0);
    const Game::MovementPosition* loserPosition = room->findMovementPosition(20);
    ASSERT_NE(loserPosition, nullptr);
    EXPECT_EQ(loserPosition->x, Game::Room::kMovementScale);
    EXPECT_EQ(loserPosition->y, 0);
}

TEST(RoomActorTests, AppliesSpaceLootThroughRoomManager) {
    Game::RoomManager manager;
    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    startTwoPlayerBattle(manager, 10, 20);
    const Game::RoomCommandResult dropped = manager.createCenterDropForSmoke(10);
    ASSERT_TRUE(dropped.ok);

    const Game::RoomActor actor(created.room.roomId);
    const Game::RoomEventApplyResult result =
        actor.apply(manager, Game::makeSpaceLootRoomEvent(10, created.room.roomId));

    ASSERT_EQ(result.status, Game::RoomEventApplyStatus::kApplied);
    ASSERT_TRUE(result.commandResult.ok);
    EXPECT_TRUE(result.commandResult.lootJustClaimed);
    EXPECT_EQ(result.commandResult.winnerSessionId, 10u);
    EXPECT_EQ(result.commandResult.drop.dropId, dropped.drops[0].dropId);
}

TEST(RoomActorTests, TenPlayerClickLootContentionKeepsSingleOwner) {
    constexpr std::size_t kPlayerCount = 10;
    constexpr uint64_t kFirstSessionId = 1000;

    Game::RoomManager manager(static_cast<uint16_t>(kPlayerCount));
    std::vector<uint64_t> sessionIds;
    sessionIds.reserve(kPlayerCount);
    for (std::size_t index = 0; index < kPlayerCount; ++index) {
        sessionIds.push_back(kFirstSessionId + index);
    }

    const Game::RoomCommandResult created = manager.createRoom(sessionIds.front());
    ASSERT_TRUE(created.ok);
    const uint32_t roomId = created.room.roomId;
    for (std::size_t index = 1; index < sessionIds.size(); ++index) {
        const Game::RoomCommandResult joined = manager.joinRoom(sessionIds[index], roomId);
        ASSERT_TRUE(joined.ok);
        EXPECT_EQ(joined.room.playerCount, index + 1);
        EXPECT_EQ(joined.room.maxPlayers, kPlayerCount);
    }

    for (std::size_t index = 0; index < sessionIds.size(); ++index) {
        const Game::RoomCommandResult ready = manager.markReady(sessionIds[index]);
        ASSERT_TRUE(ready.ok);
        EXPECT_EQ(ready.room.readyPlayerCount, index + 1);
        EXPECT_EQ(ready.room.playerCount, kPlayerCount);
        EXPECT_FALSE(ready.battleJustStarted);
    }
    const Game::RoomCommandResult started = manager.hostStartBattle(sessionIds.front());
    ASSERT_TRUE(started.ok);
    ASSERT_TRUE(started.battleJustStarted);

    const Game::RoomCommandResult spawned = manager.spawnMonster(roomId);
    ASSERT_TRUE(spawned.ok);
    const Game::RoomCommandResult defeated =
        manager.defeatMonster(sessionIds.front(), spawned.monster.monsterId);
    ASSERT_TRUE(defeated.ok);
    ASSERT_GE(defeated.drops.size(), 1u);
    ASSERT_LT(defeated.drops.size(), sessionIds.size());
    const uint32_t dropId = defeated.drops[0].dropId;
    const uint32_t itemId = defeated.drops[0].itemId;
    const uint16_t quantity = defeated.drops[0].quantity;

    Game::RoomEventQueue queue(kPlayerCount);
    for (uint64_t sessionId : sessionIds) {
        ASSERT_EQ(
            queue.enqueue(Game::makeClickLootRoomEvent(sessionId, roomId, dropId)),
            Game::RoomEventQueueEnqueueResult::kEnqueued);
    }

    const Game::RoomActor actor(roomId);
    std::size_t claimedCount = 0;
    std::size_t rejectedCount = 0;
    uint64_t winnerSessionId = 0;

    Game::RoomEvent event;
    while (queue.tryDequeue(event)) {
        const Game::RoomEventApplyResult result = actor.apply(manager, event);
        ASSERT_EQ(result.status, Game::RoomEventApplyStatus::kApplied);
        ASSERT_TRUE(result.commandResult.ok);

        if (result.commandResult.lootJustClaimed) {
            ++claimedCount;
            winnerSessionId = result.commandResult.winnerSessionId;
            EXPECT_FALSE(result.commandResult.lootRejected);
            EXPECT_EQ(result.commandResult.drop.dropId, dropId);
            EXPECT_EQ(result.commandResult.drop.itemId, itemId);
            EXPECT_EQ(result.commandResult.drop.quantity, quantity);
            continue;
        }

        ++rejectedCount;
        EXPECT_TRUE(result.commandResult.lootRejected);
        EXPECT_EQ(result.commandResult.lootRejectReason, Game::LootRejectReason::kAlreadyClaimed);
        EXPECT_EQ(result.commandResult.winnerSessionId, winnerSessionId);
    }

    EXPECT_EQ(claimedCount, 1u);
    EXPECT_EQ(rejectedCount, kPlayerCount - 1);
    ASSERT_NE(winnerSessionId, 0u);

    const Game::Room* room = manager.findRoom(roomId);
    ASSERT_NE(room, nullptr);
    const auto dropIt = std::find_if(
        room->drops().begin(),
        room->drops().end(),
        [dropId](const Game::Drop& drop) { return drop.dropId == dropId; });
    ASSERT_NE(dropIt, room->drops().end());
    EXPECT_TRUE(dropIt->claimed);
    EXPECT_EQ(dropIt->ownerSessionId, winnerSessionId);

    for (uint64_t sessionId : sessionIds) {
        const Game::InventorySnapshot* inventory = room->findInventory(sessionId);
        ASSERT_NE(inventory, nullptr);
        EXPECT_EQ(inventory->sessionId, sessionId);
        EXPECT_EQ(inventory->maxWeight, Game::Room::kDefaultMaxInventoryWeight);

        if (sessionId == winnerSessionId) {
            EXPECT_EQ(inventory->currentWeight, quantity);
            ASSERT_EQ(inventory->entries.size(), 1u);
            EXPECT_EQ(inventory->entries[0].itemId, itemId);
            EXPECT_EQ(inventory->entries[0].quantity, quantity);
        } else {
            EXPECT_EQ(inventory->currentWeight, 0u);
            EXPECT_TRUE(inventory->entries.empty());
        }
    }
}

TEST(RoomActorTests, MultipleRoomsKeepClickLootStateIsolated) {
    constexpr uint64_t kRoomAWinnerSessionId = 100;
    constexpr uint64_t kRoomALoserSessionId = 101;
    constexpr uint64_t kRoomBWinnerSessionId = 200;
    constexpr uint64_t kRoomBLoserSessionId = 201;

    Game::RoomManager manager;
    const Game::RoomCommandResult createdRoomA = manager.createRoom(kRoomAWinnerSessionId);
    ASSERT_TRUE(createdRoomA.ok);
    const uint32_t roomAId = createdRoomA.room.roomId;
    ASSERT_TRUE(manager.joinRoom(kRoomALoserSessionId, roomAId).ok);

    const Game::RoomCommandResult createdRoomB = manager.createRoom(kRoomBWinnerSessionId);
    ASSERT_TRUE(createdRoomB.ok);
    const uint32_t roomBId = createdRoomB.room.roomId;
    ASSERT_NE(roomBId, roomAId);
    ASSERT_TRUE(manager.joinRoom(kRoomBLoserSessionId, roomBId).ok);

    startTwoPlayerBattle(manager, kRoomAWinnerSessionId, kRoomALoserSessionId);
    startTwoPlayerBattle(manager, kRoomBWinnerSessionId, kRoomBLoserSessionId);

    const Game::RoomCommandResult spawnedA = manager.spawnMonster(roomAId);
    ASSERT_TRUE(spawnedA.ok);
    const Game::RoomCommandResult defeatedA =
        manager.defeatMonster(kRoomAWinnerSessionId, spawnedA.monster.monsterId);
    ASSERT_TRUE(defeatedA.ok);
    ASSERT_EQ(defeatedA.drops.size(), 1u);
    const uint32_t dropAId = defeatedA.drops[0].dropId;
    const uint32_t itemAId = defeatedA.drops[0].itemId;
    const uint16_t quantityA = defeatedA.drops[0].quantity;

    const Game::RoomCommandResult spawnedB = manager.spawnMonster(roomBId);
    ASSERT_TRUE(spawnedB.ok);
    const Game::RoomCommandResult defeatedB =
        manager.defeatMonster(kRoomBWinnerSessionId, spawnedB.monster.monsterId);
    ASSERT_TRUE(defeatedB.ok);
    ASSERT_EQ(defeatedB.drops.size(), 1u);
    const uint32_t dropBId = defeatedB.drops[0].dropId;
    const uint32_t itemBId = defeatedB.drops[0].itemId;
    const uint16_t quantityB = defeatedB.drops[0].quantity;
    ASSERT_NE(dropBId, dropAId);

    Game::RoomEventDispatcher dispatcher(4);
    ASSERT_TRUE(dispatcher.registerRoom(roomAId));
    ASSERT_TRUE(dispatcher.registerRoom(roomBId));

    ASSERT_TRUE(
        dispatcher.enqueue(
            Game::makeClickLootRoomEvent(kRoomAWinnerSessionId, roomAId, dropAId))
            .scheduledRoom);
    ASSERT_TRUE(
        dispatcher.enqueue(
            Game::makeClickLootRoomEvent(kRoomBWinnerSessionId, roomBId, dropBId))
            .scheduledRoom);
    ASSERT_FALSE(
        dispatcher.enqueue(
            Game::makeClickLootRoomEvent(kRoomALoserSessionId, roomAId, dropAId))
            .scheduledRoom);
    ASSERT_FALSE(
        dispatcher.enqueue(
            Game::makeClickLootRoomEvent(kRoomBLoserSessionId, roomBId, dropBId))
            .scheduledRoom);

    std::size_t processedRoomA = 0;
    std::size_t processedRoomB = 0;
    std::size_t rescheduledCount = 0;
    uint32_t activeRoomId = 0;
    while (dispatcher.tryPopActiveRoom(activeRoomId)) {
        Game::RoomEvent event;
        ASSERT_TRUE(dispatcher.tryDequeueRoomEvent(activeRoomId, event));
        ASSERT_EQ(event.roomId, activeRoomId);

        const Game::RoomActor actor(activeRoomId);
        const Game::RoomEventApplyResult result = actor.apply(manager, event);
        ASSERT_EQ(result.status, Game::RoomEventApplyStatus::kApplied);
        ASSERT_TRUE(result.commandResult.ok);

        if (activeRoomId == roomAId) {
            ++processedRoomA;
            if (event.sessionId == kRoomAWinnerSessionId) {
                EXPECT_TRUE(result.commandResult.lootJustClaimed);
                EXPECT_EQ(result.commandResult.winnerSessionId, kRoomAWinnerSessionId);
                EXPECT_EQ(result.commandResult.drop.dropId, dropAId);
            } else {
                EXPECT_EQ(event.sessionId, kRoomALoserSessionId);
                EXPECT_FALSE(result.commandResult.lootJustClaimed);
                EXPECT_TRUE(result.commandResult.lootRejected);
                EXPECT_EQ(
                    result.commandResult.lootRejectReason,
                    Game::LootRejectReason::kAlreadyClaimed);
                EXPECT_EQ(result.commandResult.winnerSessionId, kRoomAWinnerSessionId);
            }
        } else {
            ASSERT_EQ(activeRoomId, roomBId);
            ++processedRoomB;
            if (event.sessionId == kRoomBWinnerSessionId) {
                EXPECT_TRUE(result.commandResult.lootJustClaimed);
                EXPECT_EQ(result.commandResult.winnerSessionId, kRoomBWinnerSessionId);
                EXPECT_EQ(result.commandResult.drop.dropId, dropBId);
            } else {
                EXPECT_EQ(event.sessionId, kRoomBLoserSessionId);
                EXPECT_FALSE(result.commandResult.lootJustClaimed);
                EXPECT_TRUE(result.commandResult.lootRejected);
                EXPECT_EQ(
                    result.commandResult.lootRejectReason,
                    Game::LootRejectReason::kAlreadyClaimed);
                EXPECT_EQ(result.commandResult.winnerSessionId, kRoomBWinnerSessionId);
            }
        }

        const Game::RoomEventDispatcherCompletionResult completed =
            dispatcher.completeRoomProcessing(activeRoomId);
        ASSERT_TRUE(completed.knownRoom);
        if (completed.rescheduledRoom) {
            ++rescheduledCount;
        }
    }

    EXPECT_EQ(processedRoomA, 2u);
    EXPECT_EQ(processedRoomB, 2u);
    EXPECT_EQ(rescheduledCount, 2u);

    const Game::Room* roomA = manager.findRoom(roomAId);
    ASSERT_NE(roomA, nullptr);
    ASSERT_EQ(roomA->drops().size(), 1u);
    EXPECT_EQ(roomA->drops()[0].dropId, dropAId);
    EXPECT_TRUE(roomA->drops()[0].claimed);
    EXPECT_EQ(roomA->drops()[0].ownerSessionId, kRoomAWinnerSessionId);
    EXPECT_EQ(roomA->findInventory(kRoomBWinnerSessionId), nullptr);
    EXPECT_EQ(roomA->findInventory(kRoomBLoserSessionId), nullptr);

    const Game::InventorySnapshot* roomAWinnerInventory =
        roomA->findInventory(kRoomAWinnerSessionId);
    ASSERT_NE(roomAWinnerInventory, nullptr);
    EXPECT_EQ(roomAWinnerInventory->currentWeight, quantityA);
    ASSERT_EQ(roomAWinnerInventory->entries.size(), 1u);
    EXPECT_EQ(roomAWinnerInventory->entries[0].itemId, itemAId);
    EXPECT_EQ(roomAWinnerInventory->entries[0].quantity, quantityA);
    const Game::InventorySnapshot* roomALoserInventory =
        roomA->findInventory(kRoomALoserSessionId);
    ASSERT_NE(roomALoserInventory, nullptr);
    EXPECT_EQ(roomALoserInventory->currentWeight, 0u);
    EXPECT_TRUE(roomALoserInventory->entries.empty());

    const Game::Room* roomB = manager.findRoom(roomBId);
    ASSERT_NE(roomB, nullptr);
    ASSERT_EQ(roomB->drops().size(), 1u);
    EXPECT_EQ(roomB->drops()[0].dropId, dropBId);
    EXPECT_TRUE(roomB->drops()[0].claimed);
    EXPECT_EQ(roomB->drops()[0].ownerSessionId, kRoomBWinnerSessionId);
    EXPECT_EQ(roomB->findInventory(kRoomAWinnerSessionId), nullptr);
    EXPECT_EQ(roomB->findInventory(kRoomALoserSessionId), nullptr);

    const Game::InventorySnapshot* roomBWinnerInventory =
        roomB->findInventory(kRoomBWinnerSessionId);
    ASSERT_NE(roomBWinnerInventory, nullptr);
    EXPECT_EQ(roomBWinnerInventory->currentWeight, quantityB);
    ASSERT_EQ(roomBWinnerInventory->entries.size(), 1u);
    EXPECT_EQ(roomBWinnerInventory->entries[0].itemId, itemBId);
    EXPECT_EQ(roomBWinnerInventory->entries[0].quantity, quantityB);
    const Game::InventorySnapshot* roomBLoserInventory =
        roomB->findInventory(kRoomBLoserSessionId);
    ASSERT_NE(roomBLoserInventory, nullptr);
    EXPECT_EQ(roomBLoserInventory->currentWeight, 0u);
    EXPECT_TRUE(roomBLoserInventory->entries.empty());
}

TEST(RoomActorTests, RejectsInvalidEventBeforeMutatingRoomManager) {
    Game::RoomManager manager;
    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    const Game::RoomActor actor(created.room.roomId);

    const Game::RoomEvent invalidReady{
        Game::RoomEventType::kReady,
        10,
        created.room.roomId,
        7};
    const Game::RoomEventApplyResult result = actor.apply(manager, invalidReady);

    EXPECT_EQ(result.status, Game::RoomEventApplyStatus::kInvalidEvent);
    EXPECT_FALSE(result.commandResult.ok);
    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_EQ(room->readyPlayerCount(), 0u);
    EXPECT_FALSE(room->battleStarted());
}

TEST(RoomActorTests, RejectsEventForDifferentRoomBeforeRoomManagerCommand) {
    Game::RoomManager manager;
    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    const Game::RoomActor actor(created.room.roomId);

    const Game::RoomEventApplyResult result = actor.apply(
        manager,
        Game::makeReadyRoomEvent(10, created.room.roomId + 1));

    EXPECT_EQ(result.status, Game::RoomEventApplyStatus::kRoomMismatch);
    EXPECT_FALSE(result.commandResult.ok);
    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_EQ(room->readyPlayerCount(), 0u);
    EXPECT_FALSE(room->battleStarted());
}

TEST(RoomActorTests, RejectsStaleEventWhenSessionMovedToAnotherRoom) {
    Game::RoomManager manager;
    const Game::RoomCommandResult oldRoom = manager.createRoom(10);
    ASSERT_TRUE(oldRoom.ok);
    ASSERT_TRUE(manager.joinRoom(20, oldRoom.room.roomId).ok);
    ASSERT_TRUE(manager.leaveRoom(10).ok);
    const Game::RoomCommandResult newRoom = manager.createRoom(10);
    ASSERT_TRUE(newRoom.ok);
    const Game::RoomActor oldActor(oldRoom.room.roomId);

    const Game::RoomEventApplyResult result = oldActor.apply(
        manager,
        Game::makeReadyRoomEvent(10, oldRoom.room.roomId));

    EXPECT_EQ(result.status, Game::RoomEventApplyStatus::kRoomMismatch);
    EXPECT_FALSE(result.commandResult.ok);

    const Game::Room* oldRoomState = manager.findRoom(oldRoom.room.roomId);
    ASSERT_NE(oldRoomState, nullptr);
    EXPECT_EQ(oldRoomState->readyPlayerCount(), 0u);
    EXPECT_FALSE(oldRoomState->battleStarted());

    const Game::Room* newRoomState = manager.findRoom(newRoom.room.roomId);
    ASSERT_NE(newRoomState, nullptr);
    EXPECT_EQ(newRoomState->readyPlayerCount(), 0u);
    EXPECT_FALSE(newRoomState->battleStarted());
}

TEST(RoomActorTests, PreservesRoomCommandRejection) {
    Game::RoomManager manager;
    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    startTwoPlayerBattle(manager, 10, 20);
    const Game::RoomCommandResult spawned = manager.spawnMonster(created.room.roomId);
    ASSERT_TRUE(spawned.ok);
    const Game::RoomActor actor(created.room.roomId);

    const Game::RoomEventApplyResult result = actor.apply(
        manager,
        Game::makeMonsterDeathRoomEvent(
            10,
            created.room.roomId,
            spawned.monster.monsterId + 1));

    EXPECT_EQ(result.status, Game::RoomEventApplyStatus::kRoomCommandRejected);
    EXPECT_FALSE(result.commandResult.ok);
    EXPECT_EQ(result.commandResult.error, Game::RoomCommandError::kNotFound);
    const Game::Room* room = manager.findRoom(created.room.roomId);
    ASSERT_NE(room, nullptr);
    EXPECT_TRUE(room->hasAliveMonster());
}
