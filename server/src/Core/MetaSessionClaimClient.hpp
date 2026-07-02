#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "Core/GameSessionAuthState.hpp"

namespace Core {

struct MetaSessionClaimRequest {
    uint64_t connectionId{0};
    std::string gameSessionToken;
};

struct MetaSessionClaimResult {
    bool accepted{false};
    AuthenticatedPlayerProfile profile;
    // Required on accepted results. The server rejects accepted callbacks that
    // arrive at or after this Unix epoch millisecond deadline.
    uint64_t reservationExpiresAtUnixMs{0};
};

struct MetaSessionReleaseRequest {
    uint64_t accountId{0};
    // Pre-Welcome accepted-then-disconnected races have no server session yet,
    // so serverSessionId may be 0 while connectionId still identifies the claim.
    uint64_t serverSessionId{0};
    uint64_t connectionId{0};
};

struct MetaSessionRenewRequest {
    uint64_t accountId{0};
    uint64_t connectionId{0};
};

class IMetaSessionClaimClient {
public:
    using ClaimCallback = std::function<void(MetaSessionClaimResult)>;

    virtual ~IMetaSessionClaimClient() = default;
    // Implementations must perform network/Meta I/O asynchronously from the
    // server event loop and complete with accepted or rejected no later than
    // reservationExpiresAt or the implementation's Meta-side transport timeout.
    virtual void claimGameSessionAsync(
        const MetaSessionClaimRequest& request,
        ClaimCallback callback) = 0;
    virtual void releaseGameSessionAsync(const MetaSessionReleaseRequest& request) = 0;
    virtual void renewGameSessionAsync(const MetaSessionRenewRequest& request) = 0;
};

}  // namespace Core
