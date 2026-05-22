#include <gtest/gtest.h>

#include <cstdint>

#include "Game/OutboundSendQueue.hpp"

namespace {
Game::OutboundEnvelope popEnvelope(Game::OutboundSendQueue& queue) {
    Game::OutboundEnvelope envelope;
    EXPECT_TRUE(queue.tryPop(envelope));
    return envelope;
}

void expectRoomTarget(
    const Game::OutboundEnvelope& envelope,
    Game::OutboundMessageType message,
    uint32_t roomId) {
    EXPECT_EQ(envelope.target, Game::OutboundTargetType::kRoom);
    EXPECT_EQ(envelope.message, message);
    EXPECT_EQ(envelope.targetRoomId, roomId);
}

void expectSessionTarget(
    const Game::OutboundEnvelope& envelope,
    Game::OutboundMessageType message,
    uint64_t sessionId,
    uint32_t roomId) {
    EXPECT_EQ(envelope.target, Game::OutboundTargetType::kSession);
    EXPECT_EQ(envelope.message, message);
    EXPECT_EQ(envelope.targetSessionId, sessionId);
    EXPECT_EQ(envelope.targetRoomId, roomId);
}

struct BattleRoomFixture {
    Game::RoomManager manager;
    uint32_t roomId{0};

    BattleRoomFixture() {
        const Game::RoomCommandResult created = manager.createRoom(10);
        EXPECT_TRUE(created.ok);
        roomId = created.room.roomId;
        EXPECT_TRUE(manager.joinRoom(20, roomId).ok);
        EXPECT_TRUE(manager.markReady(10).ok);
        EXPECT_TRUE(manager.markReady(20).ok);
    }
};

Game::RoomCommandResult defeatSpawnedMonster(BattleRoomFixture& fixture) {
    const Game::RoomCommandResult spawned = fixture.manager.spawnMonster(fixture.roomId);
    EXPECT_TRUE(spawned.ok);
    return fixture.manager.defeatMonster(10, spawned.monster.monsterId);
}
}  // namespace

TEST(OutboundSendQueueTests, PushAndTryPopPreserveFifoAndDepth) {
    Game::OutboundSendQueue queue;

    Game::OutboundEnvelope ready;
    ready.target = Game::OutboundTargetType::kSession;
    ready.message = Game::OutboundMessageType::kReadyRoomResponse;
    ready.targetSessionId = 10;
    ready.targetRoomId = 1;

    Game::OutboundEnvelope battleStart;
    battleStart.target = Game::OutboundTargetType::kRoom;
    battleStart.message = Game::OutboundMessageType::kBattleStart;
    battleStart.targetRoomId = 1;

    EXPECT_TRUE(queue.empty());
    queue.push(ready);
    queue.push(battleStart);
    EXPECT_FALSE(queue.empty());
    EXPECT_EQ(queue.depth(), 2U);

    Game::OutboundEnvelope popped;
    ASSERT_TRUE(queue.tryPop(popped));
    expectSessionTarget(
        popped,
        Game::OutboundMessageType::kReadyRoomResponse,
        10,
        1);
    ASSERT_TRUE(queue.tryPop(popped));
    expectRoomTarget(popped, Game::OutboundMessageType::kBattleStart, 1);
    EXPECT_FALSE(queue.tryPop(popped));
    EXPECT_TRUE(queue.empty());
}

TEST(OutboundSendQueueTests, ReadyApplyResultProducesReadyResponseThenBattleStart) {
    Game::RoomManager manager;
    const Game::RoomCommandResult created = manager.createRoom(10);
    ASSERT_TRUE(created.ok);
    ASSERT_TRUE(manager.joinRoom(20, created.room.roomId).ok);
    const Game::RoomActor actor(created.room.roomId);
    ASSERT_EQ(
        actor.apply(manager, Game::makeReadyRoomEvent(10, created.room.roomId)).status,
        Game::RoomEventApplyStatus::kApplied);

    const Game::RoomEvent ready = Game::makeReadyRoomEvent(20, created.room.roomId);
    const Game::RoomEventApplyResult result = actor.apply(manager, ready);
    ASSERT_EQ(result.status, Game::RoomEventApplyStatus::kApplied);
    ASSERT_TRUE(result.commandResult.battleJustStarted);

    Game::OutboundSendQueue queue;
    EXPECT_EQ(queue.enqueueFromRoomEventApplyResult(ready, result), 2U);

    const Game::OutboundEnvelope readyResponse = popEnvelope(queue);
    expectSessionTarget(
        readyResponse,
        Game::OutboundMessageType::kReadyRoomResponse,
        20,
        created.room.roomId);
    EXPECT_EQ(readyResponse.sourceEventType, Game::RoomEventType::kReady);
    EXPECT_EQ(readyResponse.room.readyPlayerCount, 2U);

    const Game::OutboundEnvelope battleStart = popEnvelope(queue);
    expectRoomTarget(
        battleStart,
        Game::OutboundMessageType::kBattleStart,
        created.room.roomId);
    EXPECT_EQ(battleStart.sourceEventType, Game::RoomEventType::kReady);
    ASSERT_EQ(battleStart.playerSessionIds.size(), 2U);
    EXPECT_EQ(battleStart.playerSessionIds[0], 10U);
    EXPECT_EQ(battleStart.playerSessionIds[1], 20U);
    EXPECT_TRUE(queue.empty());
}

TEST(OutboundSendQueueTests, MonsterDeathApplyResultProducesDeathAndDropSnapshot) {
    BattleRoomFixture fixture;
    const Game::RoomCommandResult spawned = fixture.manager.spawnMonster(fixture.roomId);
    ASSERT_TRUE(spawned.ok);
    const Game::RoomActor actor(fixture.roomId);

    const Game::RoomEvent monsterDeath =
        Game::makeMonsterDeathRoomEvent(10, fixture.roomId, spawned.monster.monsterId);
    const Game::RoomEventApplyResult result = actor.apply(fixture.manager, monsterDeath);
    ASSERT_EQ(result.status, Game::RoomEventApplyStatus::kApplied);
    ASSERT_TRUE(result.commandResult.monsterJustDefeated);
    ASSERT_EQ(result.commandResult.drops.size(), 1U);

    Game::OutboundSendQueue queue;
    EXPECT_EQ(queue.enqueueFromRoomEventApplyResult(monsterDeath, result), 2U);

    const Game::OutboundEnvelope death = popEnvelope(queue);
    expectRoomTarget(death, Game::OutboundMessageType::kMonsterDeath, fixture.roomId);
    EXPECT_EQ(death.sourceEventType, Game::RoomEventType::kMonsterDeath);
    EXPECT_EQ(death.monster.monsterId, spawned.monster.monsterId);
    EXPECT_FALSE(death.monster.alive);

    const Game::OutboundEnvelope dropSnapshot = popEnvelope(queue);
    expectRoomTarget(
        dropSnapshot,
        Game::OutboundMessageType::kDropListSnapshot,
        fixture.roomId);
    EXPECT_EQ(dropSnapshot.sourceEventType, Game::RoomEventType::kMonsterDeath);
    ASSERT_EQ(dropSnapshot.drops.size(), 1U);
    EXPECT_EQ(dropSnapshot.drops[0].dropId, result.commandResult.drops[0].dropId);
    EXPECT_TRUE(queue.empty());
}

TEST(OutboundSendQueueTests, ClickLootClaimAndRejectResultsProduceExpectedEnvelopes) {
    BattleRoomFixture fixture;
    const Game::RoomCommandResult defeated = defeatSpawnedMonster(fixture);
    ASSERT_TRUE(defeated.ok);
    ASSERT_EQ(defeated.drops.size(), 1U);
    const uint32_t dropId = defeated.drops[0].dropId;
    const Game::RoomActor actor(fixture.roomId);
    Game::OutboundSendQueue queue;

    const Game::RoomEvent firstClick =
        Game::makeClickLootRoomEvent(10, fixture.roomId, dropId);
    const Game::RoomEventApplyResult firstResult =
        actor.apply(fixture.manager, firstClick);
    ASSERT_EQ(firstResult.status, Game::RoomEventApplyStatus::kApplied);
    ASSERT_TRUE(firstResult.commandResult.lootJustClaimed);
    EXPECT_EQ(queue.enqueueFromRoomEventApplyResult(firstClick, firstResult), 2U);

    const Game::OutboundEnvelope resolved = popEnvelope(queue);
    expectRoomTarget(resolved, Game::OutboundMessageType::kLootResolved, fixture.roomId);
    EXPECT_EQ(resolved.sourceEventType, Game::RoomEventType::kClickLoot);
    EXPECT_EQ(resolved.winnerSessionId, 10U);
    EXPECT_EQ(resolved.drop.dropId, dropId);
    EXPECT_TRUE(resolved.drop.claimed);

    const Game::OutboundEnvelope inventory = popEnvelope(queue);
    expectSessionTarget(
        inventory,
        Game::OutboundMessageType::kInventorySnapshot,
        10,
        fixture.roomId);
    EXPECT_EQ(inventory.sourceEventType, Game::RoomEventType::kClickLoot);
    EXPECT_EQ(inventory.inventory.sessionId, 10U);
    ASSERT_EQ(inventory.inventory.entries.size(), 1U);

    const Game::RoomEvent secondClick =
        Game::makeClickLootRoomEvent(20, fixture.roomId, dropId);
    const Game::RoomEventApplyResult secondResult =
        actor.apply(fixture.manager, secondClick);
    ASSERT_EQ(secondResult.status, Game::RoomEventApplyStatus::kApplied);
    ASSERT_TRUE(secondResult.commandResult.lootRejected);
    EXPECT_EQ(queue.enqueueFromRoomEventApplyResult(secondClick, secondResult), 1U);

    const Game::OutboundEnvelope rejected = popEnvelope(queue);
    expectSessionTarget(
        rejected,
        Game::OutboundMessageType::kLootRejected,
        20,
        fixture.roomId);
    EXPECT_EQ(rejected.sourceEventType, Game::RoomEventType::kClickLoot);
    EXPECT_EQ(rejected.lootRejectReason, Game::LootRejectReason::kAlreadyClaimed);
    EXPECT_EQ(rejected.winnerSessionId, 10U);
    EXPECT_EQ(rejected.drop.dropId, dropId);
    EXPECT_TRUE(queue.empty());
}

TEST(OutboundSendQueueTests, RoomCommandRejectedProducesSessionErrorEnvelope) {
    BattleRoomFixture fixture;
    const Game::RoomCommandResult spawned = fixture.manager.spawnMonster(fixture.roomId);
    ASSERT_TRUE(spawned.ok);
    const Game::RoomActor actor(fixture.roomId);

    const Game::RoomEvent wrongMonster =
        Game::makeMonsterDeathRoomEvent(10, fixture.roomId, spawned.monster.monsterId + 1);
    const Game::RoomEventApplyResult result = actor.apply(fixture.manager, wrongMonster);
    ASSERT_EQ(result.status, Game::RoomEventApplyStatus::kRoomCommandRejected);
    ASSERT_EQ(result.commandResult.error, Game::RoomCommandError::kNotFound);

    Game::OutboundSendQueue queue;
    EXPECT_EQ(queue.enqueueFromRoomEventApplyResult(wrongMonster, result), 1U);

    const Game::OutboundEnvelope error = popEnvelope(queue);
    expectSessionTarget(error, Game::OutboundMessageType::kError, 10, fixture.roomId);
    EXPECT_EQ(error.sourceEventType, Game::RoomEventType::kMonsterDeath);
    EXPECT_EQ(error.error, Game::RoomCommandError::kNotFound);
    EXPECT_TRUE(queue.empty());
}

TEST(OutboundSendQueueTests, InvalidOrMisroutedActorResultsProduceNoOutbound) {
    Game::RoomEvent invalidReady{Game::RoomEventType::kReady, 10, 1, 7};
    Game::RoomEventApplyResult invalidResult;
    invalidResult.status = Game::RoomEventApplyStatus::kInvalidEvent;

    Game::OutboundSendQueue queue;
    EXPECT_EQ(queue.enqueueFromRoomEventApplyResult(invalidReady, invalidResult), 0U);
    EXPECT_TRUE(queue.empty());

    const Game::RoomEvent misrouted = Game::makeReadyRoomEvent(10, 1);
    Game::RoomEventApplyResult misroutedResult;
    misroutedResult.status = Game::RoomEventApplyStatus::kRoomMismatch;
    EXPECT_EQ(queue.enqueueFromRoomEventApplyResult(misrouted, misroutedResult), 0U);
    EXPECT_TRUE(queue.empty());
}

TEST(OutboundSendQueueTests, RoomCommandBroadcastsPreserveMonsterSpawnEnvelope) {
    BattleRoomFixture fixture;
    const Game::RoomCommandResult spawned = fixture.manager.spawnMonster(fixture.roomId);
    ASSERT_TRUE(spawned.ok);
    ASSERT_TRUE(spawned.monsterJustSpawned);

    Game::OutboundSendQueue queue;
    EXPECT_EQ(queue.enqueueRoomCommandBroadcasts(spawned), 1U);

    const Game::OutboundEnvelope monsterSpawn = popEnvelope(queue);
    expectRoomTarget(
        monsterSpawn,
        Game::OutboundMessageType::kMonsterSpawn,
        fixture.roomId);
    EXPECT_EQ(monsterSpawn.monster.monsterId, spawned.monster.monsterId);
    EXPECT_TRUE(monsterSpawn.monster.alive);
    EXPECT_EQ(monsterSpawn.room.roomId, fixture.roomId);
    EXPECT_TRUE(queue.empty());
}
