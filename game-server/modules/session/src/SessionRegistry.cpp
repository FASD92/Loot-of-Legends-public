#include <lol/session/SessionRegistry.hpp>

#include <utility>

namespace lol::session {

AuthenticateSessionResult
SessionRegistry::authenticate(AuthenticateSessionCommand command) {
  const shared::AccountId accountId = command.identity.accountId;
  std::optional<ReplacedSession> replaced;

  const auto current = currentByAccount_.find(accountId);
  if (current != currentByAccount_.end()) {
    replaced =
        ReplacedSession{current->second.sessionId, current->second.generation};
    accountBySession_.erase(current->second.sessionId);
  }

  const shared::SessionId sessionId{nextSessionId_++};
  const shared::SessionGeneration generation{nextGeneration_++};
  currentByAccount_.insert_or_assign(
      accountId, ActiveSession{sessionId, generation,
                               std::move(command.identity.nickname)});
  accountBySession_.emplace(sessionId, accountId);

  return AuthenticateSessionResult{sessionId, generation, replaced};
}

bool SessionRegistry::disconnect(shared::SessionId sessionId,
                                 shared::SessionGeneration generation) {
  const auto route = accountBySession_.find(sessionId);
  if (route == accountBySession_.end()) {
    return false;
  }

  const auto current = currentByAccount_.find(route->second);
  if (current == currentByAccount_.end() ||
      current->second.sessionId != sessionId ||
      current->second.generation != generation) {
    return false;
  }

  currentByAccount_.erase(current);
  accountBySession_.erase(route);
  return true;
}

std::size_t SessionRegistry::activeSessionCount() const noexcept {
  return currentByAccount_.size();
}

} // namespace lol::session
