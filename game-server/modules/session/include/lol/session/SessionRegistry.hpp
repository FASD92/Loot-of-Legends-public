#pragma once

#include <lol/shared/Identifiers.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lol::session {

struct ClaimedGameIdentity final {
  shared::AccountId accountId;
  std::string nickname;
};

struct RecoveredDetachedSession final {
  ClaimedGameIdentity identity;
  shared::SessionId sessionId;
  shared::SessionGeneration previousGeneration;
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

struct ResumeSessionCommand final {
  shared::RequestId requestId;
  ClaimedGameIdentity identity;
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
  std::chrono::steady_clock::time_point now;
};

enum class ResumeSessionCode : std::uint8_t {
  Ok,
  NotDetached,
  IdentityMismatch,
  StaleSession,
  Expired,
};

struct ResumeSessionResult final {
  ResumeSessionCode code;
  std::optional<AuthenticateSessionResult> authenticated;
};

class SessionRegistry final {
public:
  SessionRegistry();
  explicit SessionRegistry(shared::SessionGeneration firstGeneration);

  [[nodiscard]] AuthenticateSessionResult
  authenticate(AuthenticateSessionCommand command);
  [[nodiscard]] std::optional<std::vector<AuthenticateSessionResult>>
  installRecoveredDetached(std::vector<RecoveredDetachedSession> sessions,
                           std::chrono::steady_clock::time_point expiresAt);
  [[nodiscard]] bool detach(shared::SessionId sessionId,
                            shared::SessionGeneration generation,
                            std::chrono::steady_clock::time_point expiresAt);
  [[nodiscard]] ResumeSessionResult resume(ResumeSessionCommand command);
  [[nodiscard]] bool disconnect(shared::SessionId sessionId,
                                shared::SessionGeneration generation);
  [[nodiscard]] std::size_t activeSessionCount() const noexcept;

private:
  struct ActiveSession final {
    shared::SessionId sessionId;
    shared::SessionGeneration generation;
    std::string nickname;
    std::optional<std::chrono::steady_clock::time_point> detachedUntil;
    std::optional<shared::SessionGeneration> resumeProofGeneration;
  };

  std::map<shared::AccountId, ActiveSession> currentByAccount_;
  std::map<shared::SessionId, shared::AccountId> accountBySession_;
  std::uint64_t nextSessionId_{1};
  std::uint64_t nextGeneration_{1};
};

} // namespace lol::session
