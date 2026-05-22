#include <gtest/gtest.h>

#include "Game/RoomEvent.hpp"

TEST(RoomEventTests, CreatesReadyRoomEvent) {
    const Game::RoomEvent event = Game::makeReadyRoomEvent(10, 2);

    EXPECT_EQ(event.type, Game::RoomEventType::kReady);
    EXPECT_EQ(event.sessionId, 10u);
    EXPECT_EQ(event.roomId, 2u);
    EXPECT_EQ(event.argument, 0u);
    EXPECT_TRUE(Game::isValidRoomEvent(event));
}

TEST(RoomEventTests, CreatesMonsterDeathRoomEvent) {
    const Game::RoomEvent event = Game::makeMonsterDeathRoomEvent(10, 2, 7);

    EXPECT_EQ(event.type, Game::RoomEventType::kMonsterDeath);
    EXPECT_EQ(event.sessionId, 10u);
    EXPECT_EQ(event.roomId, 2u);
    EXPECT_EQ(event.argument, 7u);
    EXPECT_TRUE(Game::isValidRoomEvent(event));
}

TEST(RoomEventTests, CreatesClickLootRoomEvent) {
    const Game::RoomEvent event = Game::makeClickLootRoomEvent(10, 2, 9);

    EXPECT_EQ(event.type, Game::RoomEventType::kClickLoot);
    EXPECT_EQ(event.sessionId, 10u);
    EXPECT_EQ(event.roomId, 2u);
    EXPECT_EQ(event.argument, 9u);
    EXPECT_TRUE(Game::isValidRoomEvent(event));
}

TEST(RoomEventTests, RejectsZeroSessionOrRoom) {
    EXPECT_FALSE(Game::isValidRoomEvent(Game::makeReadyRoomEvent(0, 2)));
    EXPECT_FALSE(Game::isValidRoomEvent(Game::makeReadyRoomEvent(10, 0)));
}

TEST(RoomEventTests, RejectsReadyWithArgument) {
    const Game::RoomEvent event{Game::RoomEventType::kReady, 10, 2, 7};

    EXPECT_FALSE(Game::isValidRoomEvent(event));
}

TEST(RoomEventTests, RejectsActionWithoutArgument) {
    const Game::RoomEvent monsterDeath{Game::RoomEventType::kMonsterDeath, 10, 2, 0};
    const Game::RoomEvent clickLoot{Game::RoomEventType::kClickLoot, 10, 2, 0};

    EXPECT_FALSE(Game::isValidRoomEvent(monsterDeath));
    EXPECT_FALSE(Game::isValidRoomEvent(clickLoot));
}
