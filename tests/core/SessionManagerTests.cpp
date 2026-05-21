#include <gtest/gtest.h>

#include "Core/SessionManager.hpp"
#include "Util/Time.hpp"

TEST(SessionManagerTests, FindOrCreateReusesSession) {
    Core::SessionManager manager(std::chrono::milliseconds(1000));
    Util::TimePoint now = Util::now();

    auto sessionA = manager.findOrCreate("127.0.0.1:10001", now);
    ASSERT_NE(sessionA, nullptr);

    auto sessionB = manager.findOrCreate("127.0.0.1:10001", now);
    ASSERT_NE(sessionB, nullptr);

    EXPECT_EQ(sessionA->sessionId(), sessionB->sessionId());
    EXPECT_EQ(manager.size(), 1u);
}

TEST(SessionManagerTests, TickRemovesExpiredSession) {
    Core::SessionManager manager(std::chrono::milliseconds(10));
    Util::TimePoint now = Util::now();

    auto session = manager.findOrCreate("127.0.0.1:10002", now);
    ASSERT_NE(session, nullptr);
    EXPECT_EQ(manager.size(), 1u);

    manager.tick(now + std::chrono::milliseconds(11));
    EXPECT_EQ(manager.size(), 0u);
}

TEST(SessionManagerTests, RemoveErasesSessionByRemoteKey) {
    Core::SessionManager manager(std::chrono::milliseconds(1000));
    Util::TimePoint now = Util::now();

    auto session = manager.findOrCreate("127.0.0.1:10003", now);
    ASSERT_NE(session, nullptr);
    ASSERT_EQ(manager.size(), 1u);

    manager.remove("127.0.0.1:10003");

    EXPECT_EQ(manager.size(), 0u);
    EXPECT_EQ(manager.find("127.0.0.1:10003"), nullptr);
}

TEST(SessionManagerTests, FindsSessionBySessionId) {
    Core::SessionManager manager(std::chrono::milliseconds(1000));
    Util::TimePoint now = Util::now();

    auto sessionA = manager.findOrCreate("127.0.0.1:10004", now);
    auto sessionB = manager.findOrCreate("127.0.0.1:10005", now);
    ASSERT_NE(sessionA, nullptr);
    ASSERT_NE(sessionB, nullptr);

    EXPECT_EQ(manager.findBySessionId(sessionA->sessionId()), sessionA);
    EXPECT_EQ(manager.findBySessionId(sessionB->sessionId()), sessionB);
    EXPECT_EQ(manager.findBySessionId(9999), nullptr);
}
