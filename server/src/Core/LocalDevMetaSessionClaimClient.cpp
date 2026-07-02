#include "Core/LocalDevMetaSessionClaimClient.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace {
constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;
constexpr uint64_t kLocalDevAccountIdSalt = 0x4C4F4C4455560000ull;
constexpr std::chrono::milliseconds kLocalDevReservationTtl(60000);

bool isValidLocalDevNickname(std::string_view nickname) {
    if (nickname.size() < 2 || nickname.size() > 12) {
        return false;
    }

    for (const char ch : nickname) {
        const bool digit = ch >= '0' && ch <= '9';
        const bool lower = ch >= 'a' && ch <= 'z';
        const bool upper = ch >= 'A' && ch <= 'Z';
        if (!digit && !lower && !upper) {
            return false;
        }
    }

    return true;
}

uint64_t stableAccountIdForNickname(std::string_view nickname) {
    uint64_t hash = kFnvOffsetBasis;
    for (const unsigned char ch : nickname) {
        hash ^= ch;
        hash *= kFnvPrime;
    }

    const uint64_t accountId = (hash ^ kLocalDevAccountIdSalt) & 0x7FFFFFFFFFFFFFFFull;
    return accountId == 0 ? 1 : accountId;
}

uint64_t currentUnixTimeMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}
}  // namespace

namespace Core {

void LocalDevMetaSessionClaimClient::claimGameSessionAsync(
    const MetaSessionClaimRequest& request,
    ClaimCallback callback) {
    MetaSessionClaimResult result;

    const std::string_view token(request.gameSessionToken);
    if (token.rfind(kTokenPrefix, 0) == 0) {
        const std::string_view nickname = token.substr(kTokenPrefix.size());
        if (isValidLocalDevNickname(nickname)) {
            result.accepted = true;
            result.profile.accountId = stableAccountIdForNickname(nickname);
            result.profile.nickname = std::string(nickname);
            result.reservationExpiresAtUnixMs =
                currentUnixTimeMs() +
                static_cast<uint64_t>(kLocalDevReservationTtl.count());
        }
    }

    callback(std::move(result));
}

void LocalDevMetaSessionClaimClient::releaseGameSessionAsync(
    const MetaSessionReleaseRequest& /*request*/) {}

void LocalDevMetaSessionClaimClient::renewGameSessionAsync(
    const MetaSessionRenewRequest& /*request*/) {}

}  // namespace Core
