#pragma once

#include <lol/shared/Identifiers.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace lol::session {

struct ClaimedGameIdentity final {
  shared::AccountId accountId;
  std::string nickname;
};

struct AuthenticateSessionCommand final {
  shared::RequestId requestId;
  ClaimedGameIdentity identity;
};

struct ReplacedSession final {
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
};

struct AuthenticateSessionResult final {
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
  std::optional<ReplacedSession> replaced;
};

class SessionRegistry final {
public:
  [[nodiscard]] AuthenticateSessionResult
  authenticate(AuthenticateSessionCommand command);
  [[nodiscard]] bool disconnect(shared::SessionId sessionId,
                                shared::SessionGeneration generation);
  [[nodiscard]] std::size_t activeSessionCount() const noexcept;

private:
  struct ActiveSession final {
    shared::SessionId sessionId;
    shared::SessionGeneration generation;
    std::string nickname;
  };

  std::map<shared::AccountId, ActiveSession> currentByAccount_;
  std::map<shared::SessionId, shared::AccountId> accountBySession_;
  std::uint64_t nextSessionId_{1};
  std::uint64_t nextGeneration_{1};
};

} // namespace lol::session
