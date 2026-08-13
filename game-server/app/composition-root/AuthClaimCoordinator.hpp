#pragma once

#include <lol/meta/MetaClaimClient.hpp>
#include <lol/session/SessionRegistry.hpp>
#include <lol/shared/Identifiers.hpp>
#include <lol/transport/rudp/RudpBindingRegistry.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lol::app {

enum class AppliedClaimKind { Accepted, Rejected, Stale };

enum class RudpPeerFailureReason : std::uint8_t {
  RoomAdmissionRejected,
  ReliableAdmissionRejected,
  ReliableExpired,
};

struct RudpPeerFailure final {
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
  std::uint32_t transportEpoch;
  std::uint32_t sequence;
  RudpPeerFailureReason reason;
  transport::rudp::RudpEndpoint endpoint;
};

struct AppliedClaim final {
  AppliedClaimKind kind;
  std::uint64_t connectionEpoch;
  std::uint64_t requestId;
  meta::ClaimOutcome outcome;
  std::optional<session::AuthenticateSessionResult> authenticated;
  std::optional<std::uint64_t> replacedConnectionEpoch;
  std::string nickname;
};

struct RudpSessionClosure final {
  std::optional<std::uint64_t> connectionEpoch;
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
};

class AuthClaimCoordinator final {
public:
  explicit AuthClaimCoordinator(session::SessionRegistry &sessions) noexcept;
  AuthClaimCoordinator(
      session::SessionRegistry &sessions,
      transport::rudp::RudpBindingRegistry &rudpBindings) noexcept;

  [[nodiscard]] bool beginClaim(std::uint64_t connectionEpoch,
                                shared::RequestId requestId);
  [[nodiscard]] bool closeConnection(std::uint64_t connectionEpoch);
  [[nodiscard]] std::optional<RudpSessionClosure>
  closeRudpPeer(const RudpPeerFailure &failure);
  [[nodiscard]] std::optional<transport::rudp::RudpBindCapability>
  issueRudpBindCapability(std::uint64_t connectionEpoch,
                          shared::RequestId requestId,
                          std::chrono::steady_clock::time_point now);
  [[nodiscard]] std::vector<RudpSessionClosure>
  expireTimedOutRudpPeers(std::chrono::steady_clock::time_point now);
  [[nodiscard]] AppliedClaim apply(const meta::ClaimCompletion &completion);
  [[nodiscard]] std::size_t pendingClaimCount() const noexcept;

private:
  struct ActiveRoute final {
    shared::SessionId sessionId;
    shared::SessionGeneration generation;
  };

  session::SessionRegistry &sessions_;
  transport::rudp::RudpBindingRegistry *rudpBindings_{nullptr};
  std::map<std::uint64_t, shared::RequestId> pendingByConnectionEpoch_;
  std::map<std::uint64_t, ActiveRoute> sessionByConnectionEpoch_;
  std::map<shared::SessionId, std::uint64_t> connectionEpochBySession_;
};

} // namespace lol::app
