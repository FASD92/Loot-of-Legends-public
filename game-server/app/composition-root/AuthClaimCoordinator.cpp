#include "AuthClaimCoordinator.hpp"

namespace lol::app {

AuthClaimCoordinator::AuthClaimCoordinator(
    session::SessionRegistry &sessions) noexcept
    : sessions_(sessions) {}

AuthClaimCoordinator::AuthClaimCoordinator(
    session::SessionRegistry &sessions,
    transport::rudp::RudpBindingRegistry &rudpBindings) noexcept
    : sessions_(sessions), rudpBindings_(&rudpBindings) {}

bool AuthClaimCoordinator::beginClaim(std::uint64_t connectionEpoch,
                                      shared::RequestId requestId) {
  if (connectionEpoch == 0 || requestId.value() == 0 ||
      sessionByConnectionEpoch_.contains(connectionEpoch)) {
    return false;
  }
  return pendingByConnectionEpoch_.emplace(connectionEpoch, requestId).second;
}

bool AuthClaimCoordinator::closeConnection(std::uint64_t connectionEpoch) {
  bool changed = pendingByConnectionEpoch_.erase(connectionEpoch) == 1;
  const auto route = sessionByConnectionEpoch_.find(connectionEpoch);
  if (route == sessionByConnectionEpoch_.end()) {
    return changed;
  }
  changed =
      sessions_.disconnect(route->second.sessionId, route->second.generation) ||
      changed;
  if (rudpBindings_ != nullptr) {
    changed = rudpBindings_->invalidate(route->second.sessionId.value(),
                                        route->second.generation.value()) ||
              changed;
  }
  connectionEpochBySession_.erase(route->second.sessionId);
  sessionByConnectionEpoch_.erase(route);
  return changed;
}

std::optional<RudpSessionClosure>
AuthClaimCoordinator::closeRudpPeer(const RudpPeerFailure &failure) {
  if (rudpBindings_ == nullptr || failure.sessionId.value() == 0 ||
      failure.generation.value() == 0 || failure.transportEpoch == 0 ||
      failure.sequence == 0 ||
      rudpBindings_->status(failure.sessionId.value(),
                            failure.generation.value(), failure.transportEpoch,
                            failure.endpoint) !=
          transport::rudp::RudpPacketStatus::Current) {
    return std::nullopt;
  }

  const auto connection = connectionEpochBySession_.find(failure.sessionId);
  std::optional<std::uint64_t> connectionEpoch;
  bool changed = false;
  if (connection != connectionEpochBySession_.end()) {
    const auto route = sessionByConnectionEpoch_.find(connection->second);
    if (route == sessionByConnectionEpoch_.end() ||
        route->second.generation != failure.generation) {
      return std::nullopt;
    }
    connectionEpoch = connection->second;
    changed = closeConnection(*connectionEpoch);
  } else {
    changed = sessions_.disconnect(failure.sessionId, failure.generation);
    changed = rudpBindings_->invalidate(failure.sessionId.value(),
                                        failure.generation.value()) ||
              changed;
  }
  if (!changed) {
    return std::nullopt;
  }
  return RudpSessionClosure{.connectionEpoch = connectionEpoch,
                            .sessionId = failure.sessionId,
                            .generation = failure.generation};
}

std::optional<transport::rudp::RudpBindCapability>
AuthClaimCoordinator::issueRudpBindCapability(
    std::uint64_t connectionEpoch, shared::RequestId requestId,
    std::chrono::steady_clock::time_point now) {
  if (requestId.value() == 0 || rudpBindings_ == nullptr) {
    return std::nullopt;
  }
  const auto route = sessionByConnectionEpoch_.find(connectionEpoch);
  if (route == sessionByConnectionEpoch_.end()) {
    return std::nullopt;
  }
  return rudpBindings_->requestCapability(
      route->second.sessionId.value(), route->second.generation.value(), now);
}

std::vector<RudpSessionClosure> AuthClaimCoordinator::expireTimedOutRudpPeers(
    std::chrono::steady_clock::time_point now) {
  if (rudpBindings_ == nullptr) {
    return {};
  }
  std::vector<RudpSessionClosure> timedOut;
  for (const auto &peer : rudpBindings_->expireTimedOut(now)) {
    const shared::SessionId sessionId{peer.sessionId};
    const shared::SessionGeneration generation{peer.sessionGeneration};
    const auto connection = connectionEpochBySession_.find(sessionId);
    std::optional<std::uint64_t> connectionEpoch;
    if (connection != connectionEpochBySession_.end()) {
      const auto route = sessionByConnectionEpoch_.find(connection->second);
      if (route != sessionByConnectionEpoch_.end() &&
          route->second.generation == generation) {
        connectionEpoch = connection->second;
        static_cast<void>(closeConnection(*connectionEpoch));
      }
    }
    if (!connectionEpoch.has_value()) {
      static_cast<void>(sessions_.disconnect(sessionId, generation));
    }
    timedOut.push_back(RudpSessionClosure{
        .connectionEpoch = connectionEpoch,
        .sessionId = sessionId,
        .generation = generation,
    });
  }
  return timedOut;
}

AppliedClaim
AuthClaimCoordinator::apply(const meta::ClaimCompletion &completion) {
  const auto pending =
      pendingByConnectionEpoch_.find(completion.connectionEpoch());
  if (pending == pendingByConnectionEpoch_.end() ||
      pending->second.value() != completion.requestId()) {
    return AppliedClaim{
        .kind = AppliedClaimKind::Stale,
        .connectionEpoch = completion.connectionEpoch(),
        .requestId = completion.requestId(),
        .outcome = completion.outcome(),
        .authenticated = std::nullopt,
        .replacedConnectionEpoch = std::nullopt,
        .nickname = {},
    };
  }
  pendingByConnectionEpoch_.erase(pending);
  if (completion.outcome() != meta::ClaimOutcome::Claimed ||
      !completion.identity().has_value()) {
    return AppliedClaim{
        .kind = AppliedClaimKind::Rejected,
        .connectionEpoch = completion.connectionEpoch(),
        .requestId = completion.requestId(),
        .outcome = completion.outcome() == meta::ClaimOutcome::Claimed
                       ? meta::ClaimOutcome::MalformedResponse
                       : completion.outcome(),
        .authenticated = std::nullopt,
        .replacedConnectionEpoch = std::nullopt,
        .nickname = {},
    };
  }
  const auto &identity = *completion.identity();
  const auto authenticated =
      sessions_.authenticate(session::AuthenticateSessionCommand{
          shared::RequestId{completion.requestId()},
          session::ClaimedGameIdentity{shared::AccountId{identity.accountId},
                                       identity.nickname}});

  std::optional<std::uint64_t> replacedConnectionEpoch;
  if (authenticated.replaced.has_value()) {
    if (rudpBindings_ != nullptr) {
      static_cast<void>(rudpBindings_->invalidate(
          authenticated.replaced->sessionId.value(),
          authenticated.replaced->generation.value()));
    }
    const auto oldConnection =
        connectionEpochBySession_.find(authenticated.replaced->sessionId);
    if (oldConnection != connectionEpochBySession_.end()) {
      replacedConnectionEpoch = oldConnection->second;
      sessionByConnectionEpoch_.erase(oldConnection->second);
      connectionEpochBySession_.erase(oldConnection);
    }
  }
  sessionByConnectionEpoch_.insert_or_assign(
      completion.connectionEpoch(),
      ActiveRoute{authenticated.sessionId, authenticated.generation});
  connectionEpochBySession_.insert_or_assign(authenticated.sessionId,
                                             completion.connectionEpoch());

  return AppliedClaim{
      .kind = AppliedClaimKind::Accepted,
      .connectionEpoch = completion.connectionEpoch(),
      .requestId = completion.requestId(),
      .outcome = meta::ClaimOutcome::Claimed,
      .authenticated = authenticated,
      .replacedConnectionEpoch = replacedConnectionEpoch,
      .nickname = identity.nickname,
  };
}

std::size_t AuthClaimCoordinator::pendingClaimCount() const noexcept {
  return pendingByConnectionEpoch_.size();
}

} // namespace lol::app
