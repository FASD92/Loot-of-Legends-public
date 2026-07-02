#include <gtest/gtest.h>

#include <optional>

#include "Core/LocalDevMetaSessionClaimClient.hpp"

namespace {
std::optional<Core::MetaSessionClaimResult> claimToken(
    Core::LocalDevMetaSessionClaimClient& client,
    const std::string& token) {
    std::optional<Core::MetaSessionClaimResult> result;
    client.claimGameSessionAsync(
        Core::MetaSessionClaimRequest{7, token},
        [&result](Core::MetaSessionClaimResult nextResult) {
            result = std::move(nextResult);
        });
    return result;
}
}  // namespace

TEST(LocalDevMetaSessionClaimClientTests, AcceptsDevSessionTokenWithNickname) {
    Core::LocalDevMetaSessionClaimClient client;

    const std::optional<Core::MetaSessionClaimResult> result =
        claimToken(client, "dev-session:player1");

    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->accepted);
    EXPECT_NE(result->profile.accountId, 0u);
    EXPECT_EQ(result->profile.nickname, "player1");
    EXPECT_GT(result->reservationExpiresAtUnixMs, 0u);
}

TEST(LocalDevMetaSessionClaimClientTests, UsesStableAccountIdForSameNickname) {
    Core::LocalDevMetaSessionClaimClient client;

    const std::optional<Core::MetaSessionClaimResult> first =
        claimToken(client, "dev-session:player1");
    const std::optional<Core::MetaSessionClaimResult> second =
        claimToken(client, "dev-session:player1");

    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(first->profile.accountId, second->profile.accountId);
}

TEST(LocalDevMetaSessionClaimClientTests, RejectsNonDevOrInvalidNicknameTokens) {
    Core::LocalDevMetaSessionClaimClient client;

    const std::optional<Core::MetaSessionClaimResult> production =
        claimToken(client, "real-meta-token");
    const std::optional<Core::MetaSessionClaimResult> invalidNickname =
        claimToken(client, "dev-session:p!");

    ASSERT_TRUE(production.has_value());
    EXPECT_FALSE(production->accepted);
    ASSERT_TRUE(invalidNickname.has_value());
    EXPECT_FALSE(invalidNickname->accepted);
}
