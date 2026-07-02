#pragma once

#include <cstdint>
#include <string>

namespace Core {

enum class GameSessionAuthPhase : uint8_t {
    kUnauthenticated,
    kClaimPending,
    kAuthenticated,
    kClosing,
};

struct AuthenticatedPlayerProfile {
    uint64_t accountId{0};
    std::string nickname;
};

struct GameSessionAuthState {
    GameSessionAuthPhase phase{GameSessionAuthPhase::kUnauthenticated};
    uint64_t connectionId{0};
    uint64_t serverSessionId{0};
    std::string pendingToken;
    AuthenticatedPlayerProfile profile;
    bool releaseOnDisconnect{true};
};

}  // namespace Core
