#include <lol/session/SessionRegistry.hpp>

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

namespace lol::session {

SessionRegistry::SessionRegistry()
    : SessionRegistry(shared::SessionGeneration{1}) {}

SessionRegistry::SessionRegistry(shared::SessionGeneration firstGeneration)
    : nextGeneration_(firstGeneration.value()) {
  if (firstGeneration.value() == 0U) {
    throw std::invalid_argument{
        "SessionRegistry requires a nonzero first generation"};
  }
}

AuthenticateSessionResult
SessionRegistry::authenticate(AuthenticateSessionCommand command) {
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (nextSessionId_ == 0U || nextSessionId_ == maximum ||
      nextGeneration_ == 0U || nextGeneration_ == maximum) {
    throw std::overflow_error{"SessionRegistry identifier exhausted"};
  }

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
      accountId,
      ActiveSession{sessionId, generation, std::move(command.identity.nickname),
                    std::nullopt, std::nullopt});
  accountBySession_.emplace(sessionId, accountId);

  return AuthenticateSessionResult{sessionId, generation, replaced};
}

std::optional<std::vector<AuthenticateSessionResult>>
SessionRegistry::installRecoveredDetached(
    std::vector<RecoveredDetachedSession> sessions,
    std::chrono::steady_clock::time_point expiresAt) {
  if (sessions.empty()) {
    return std::vector<AuthenticateSessionResult>{};
  }

  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  if (nextSessionId_ == 0 || nextGeneration_ == 0 ||
      nextSessionId_ == maximum || nextGeneration_ == maximum) {
    return std::nullopt;
  }

  const shared::AccountId zeroAccount{shared::AccountId::Bytes{}};
  std::set<shared::AccountId> batchAccounts;
  std::set<shared::SessionId> batchSessionIds;
  std::uint64_t nextSessionId = nextSessionId_;
  std::uint64_t firstGeneration = nextGeneration_;

  for (const auto &candidate : sessions) {
    const auto accountId = candidate.identity.accountId;
    const auto sessionId = candidate.sessionId.value();
    const auto previousGeneration = candidate.previousGeneration.value();
    if (accountId == zeroAccount || sessionId == 0 || previousGeneration == 0 ||
        sessionId >= maximum - 1U || previousGeneration == maximum ||
        !batchAccounts.emplace(accountId).second ||
        !batchSessionIds.emplace(candidate.sessionId).second ||
        currentByAccount_.contains(accountId) ||
        accountBySession_.contains(candidate.sessionId)) {
      return std::nullopt;
    }

    if (sessionId >= nextSessionId) {
      nextSessionId = sessionId + 1;
    }
    if (previousGeneration >= firstGeneration) {
      if (previousGeneration == maximum) {
        return std::nullopt;
      }
      firstGeneration = previousGeneration + 1;
    }
  }

  for (const auto &[accountId, current] : currentByAccount_) {
    static_cast<void>(accountId);
    if (current.generation.value() == maximum) {
      return std::nullopt;
    }
    firstGeneration = std::max(firstGeneration, current.generation.value() + 1);
  }

  const auto count = static_cast<std::uint64_t>(sessions.size());
  if (count >= maximum - firstGeneration) {
    return std::nullopt;
  }
  const auto nextGeneration = firstGeneration + count;

  std::vector<AuthenticateSessionResult> authenticated;
  authenticated.reserve(sessions.size());
  for (std::size_t index = 0; index < sessions.size(); ++index) {
    auto &candidate = sessions[index];
    const shared::SessionGeneration generation{
        firstGeneration + static_cast<std::uint64_t>(index)};
    currentByAccount_.emplace(
        candidate.identity.accountId,
        ActiveSession{candidate.sessionId, generation,
                      std::move(candidate.identity.nickname), expiresAt,
                      candidate.previousGeneration});
    accountBySession_.emplace(candidate.sessionId,
                              candidate.identity.accountId);
    authenticated.push_back(AuthenticateSessionResult{
        candidate.sessionId, generation, std::nullopt});
  }
  nextSessionId_ = nextSessionId;
  nextGeneration_ = nextGeneration;
  return authenticated;
}

bool SessionRegistry::detach(shared::SessionId sessionId,
                             shared::SessionGeneration generation,
                             std::chrono::steady_clock::time_point expiresAt) {
  const auto route = accountBySession_.find(sessionId);
  if (route == accountBySession_.end()) {
    return false;
  }
  const auto current = currentByAccount_.find(route->second);
  if (current == currentByAccount_.end() ||
      current->second.sessionId != sessionId ||
      current->second.generation != generation ||
      current->second.detachedUntil.has_value()) {
    return false;
  }
  current->second.detachedUntil = expiresAt;
  current->second.resumeProofGeneration = generation;
  return true;
}

ResumeSessionResult SessionRegistry::resume(ResumeSessionCommand command) {
  const auto route = accountBySession_.find(command.sessionId);
  if (route == accountBySession_.end()) {
    return {ResumeSessionCode::StaleSession, std::nullopt};
  }
  if (route->second != command.identity.accountId) {
    return {ResumeSessionCode::IdentityMismatch, std::nullopt};
  }
  const auto current = currentByAccount_.find(route->second);
  if (current == currentByAccount_.end() ||
      current->second.sessionId != command.sessionId) {
    return {ResumeSessionCode::StaleSession, std::nullopt};
  }
  const bool currentGenerationMatches =
      current->second.generation == command.generation;
  const bool proofMatches =
      current->second.resumeProofGeneration.has_value() &&
      *current->second.resumeProofGeneration == command.generation;
  if (!currentGenerationMatches && !proofMatches) {
    return {ResumeSessionCode::StaleSession, std::nullopt};
  }
  if (!current->second.detachedUntil.has_value()) {
    return {ResumeSessionCode::NotDetached, std::nullopt};
  }
  if (command.now >= *current->second.detachedUntil) {
    return {ResumeSessionCode::Expired, std::nullopt};
  }
  if (!proofMatches) {
    return {ResumeSessionCode::StaleSession, std::nullopt};
  }
  current->second.detachedUntil.reset();
  current->second.resumeProofGeneration.reset();
  return {ResumeSessionCode::Ok,
          AuthenticateSessionResult{current->second.sessionId,
                                    current->second.generation, std::nullopt}};
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
