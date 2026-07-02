#include <gtest/gtest.h>

#include "Core/RudpInputCommandRoomEventTranslator.hpp"

namespace {
Net::RudpInputCommandPayload input(
    uint32_t playerId,
    uint32_t cmdSeq,
    Net::RudpInputCommandOp op,
    uint32_t argValue = 0) {
    return Net::RudpInputCommandPayload{playerId, cmdSeq, op, argValue};
}

void expectTranslatedEvent(
    const Core::RudpInputCommandRoomEventTranslateResult& result,
    Game::RoomEventType type,
    uint64_t sessionId,
    uint32_t roomId,
    uint32_t argument) {
    ASSERT_EQ(
        result.status,
        Core::RudpInputCommandRoomEventTranslateStatus::kTranslated);
    ASSERT_TRUE(result.event.has_value());
    EXPECT_EQ(result.event->type, type);
    EXPECT_EQ(result.event->sessionId, sessionId);
    EXPECT_EQ(result.event->roomId, roomId);
    EXPECT_EQ(result.event->argument, argument);
    EXPECT_TRUE(Game::isValidRoomEvent(*result.event));
}

void expectRejected(
    const Core::RudpInputCommandRoomEventTranslateResult& result,
    Core::RudpInputCommandRoomEventTranslateStatus status) {
    EXPECT_EQ(result.status, status);
    EXPECT_FALSE(result.event.has_value());
}
}  // namespace

TEST(RudpInputCommandRoomEventTranslatorTests, TranslatesReadyCommand) {
    const auto result = Core::translateRudpInputCommandToRoomEvent(
        100,
        7,
        input(999, 1, Net::RudpInputCommandOp::kReady));

    expectTranslatedEvent(result, Game::RoomEventType::kReady, 100, 7, 0);
}

TEST(RudpInputCommandRoomEventTranslatorTests, TranslatesMonsterDeathCommand) {
    const auto result = Core::translateRudpInputCommandToRoomEvent(
        100,
        7,
        input(999, 2, Net::RudpInputCommandOp::kMonsterDeath, 33));

    expectTranslatedEvent(result, Game::RoomEventType::kMonsterDeath, 100, 7, 33);
}

TEST(RudpInputCommandRoomEventTranslatorTests, TranslatesClickLootCommand) {
    const auto result = Core::translateRudpInputCommandToRoomEvent(
        100,
        7,
        input(999, 3, Net::RudpInputCommandOp::kClickLoot, 44));

    expectTranslatedEvent(result, Game::RoomEventType::kClickLoot, 100, 7, 44);
}

TEST(RudpInputCommandRoomEventTranslatorTests, TranslatesAttackCommandWithZeroTargetHint) {
    const auto result = Core::translateRudpInputCommandToRoomEvent(
        100,
        7,
        input(999, 10, Net::RudpInputCommandOp::kAttack, 0));

    expectTranslatedEvent(result, Game::RoomEventType::kAttack, 100, 7, 0);
}

TEST(RudpInputCommandRoomEventTranslatorTests, TranslatesAttackCommandWithTargetHint) {
    const auto result = Core::translateRudpInputCommandToRoomEvent(
        100,
        7,
        input(999, 11, Net::RudpInputCommandOp::kAttack, 33));

    expectTranslatedEvent(result, Game::RoomEventType::kAttack, 100, 7, 33);
}

TEST(RudpInputCommandRoomEventTranslatorTests, TranslatesSpaceLootCommand) {
    const auto result = Core::translateRudpInputCommandToRoomEvent(
        100,
        7,
        input(999, 12, Net::RudpInputCommandOp::kSpaceLoot));

    expectTranslatedEvent(result, Game::RoomEventType::kSpaceLoot, 100, 7, 0);
}

TEST(RudpInputCommandRoomEventTranslatorTests, RejectsMoveUntilMovementDispatchPlan) {
    Net::RudpInputCommandPayload move =
        input(999, 4, Net::RudpInputCommandOp::kMove);
    move.move = Net::RudpInputCommandMoveArgs{100, -100, 0};

    const auto result = Core::translateRudpInputCommandToRoomEvent(100, 7, move);

    expectRejected(
        result,
        Core::RudpInputCommandRoomEventTranslateStatus::kUnsupportedOp);
}

TEST(RudpInputCommandRoomEventTranslatorTests, UsesBoundSessionInsteadOfPayloadPlayerId) {
    const auto result = Core::translateRudpInputCommandToRoomEvent(
        5000,
        9,
        input(1234, 4, Net::RudpInputCommandOp::kClickLoot, 55));

    expectTranslatedEvent(result, Game::RoomEventType::kClickLoot, 5000, 9, 55);
}

TEST(RudpInputCommandRoomEventTranslatorTests, RejectsZeroSession) {
    const auto result = Core::translateRudpInputCommandToRoomEvent(
        0,
        7,
        input(999, 5, Net::RudpInputCommandOp::kReady));

    expectRejected(
        result,
        Core::RudpInputCommandRoomEventTranslateStatus::kInvalidSession);
}

TEST(RudpInputCommandRoomEventTranslatorTests, RejectsZeroRoom) {
    const auto result = Core::translateRudpInputCommandToRoomEvent(
        100,
        0,
        input(999, 6, Net::RudpInputCommandOp::kReady));

    expectRejected(result, Core::RudpInputCommandRoomEventTranslateStatus::kInvalidRoom);
}

TEST(RudpInputCommandRoomEventTranslatorTests, RejectsReadyWithArgument) {
    const auto result = Core::translateRudpInputCommandToRoomEvent(
        100,
        7,
        input(999, 7, Net::RudpInputCommandOp::kReady, 1));

    expectRejected(
        result,
        Core::RudpInputCommandRoomEventTranslateStatus::kInvalidArgument);
}

TEST(RudpInputCommandRoomEventTranslatorTests, RejectsActionWithoutArgument) {
    const auto monsterDeath = Core::translateRudpInputCommandToRoomEvent(
        100,
        7,
        input(999, 8, Net::RudpInputCommandOp::kMonsterDeath, 0));
    const auto clickLoot = Core::translateRudpInputCommandToRoomEvent(
        100,
        7,
        input(999, 9, Net::RudpInputCommandOp::kClickLoot, 0));

    expectRejected(
        monsterDeath,
        Core::RudpInputCommandRoomEventTranslateStatus::kInvalidArgument);
    expectRejected(
        clickLoot,
        Core::RudpInputCommandRoomEventTranslateStatus::kInvalidArgument);
}

TEST(RudpInputCommandRoomEventTranslatorTests, RejectsUnsupportedOp) {
    const auto result = Core::translateRudpInputCommandToRoomEvent(
        100,
        7,
        input(
            999,
            10,
            static_cast<Net::RudpInputCommandOp>(0x7F),
            0));

    expectRejected(
        result,
        Core::RudpInputCommandRoomEventTranslateStatus::kUnsupportedOp);
}
