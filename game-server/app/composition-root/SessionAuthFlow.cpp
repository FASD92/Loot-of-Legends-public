#include "SessionAuthFlow.hpp"

#include <stdexcept>
#include <utility>

namespace lol::app {
namespace {

using transport::tcp::AuthenticationRejected;
using transport::tcp::AuthenticationRejectedReason;
using transport::tcp::CloseReason;
using transport::tcp::RequestRudpBindCapability;
using transport::tcp::RudpBindCapability;
using transport::tcp::SessionControlMessage;
using transport::tcp::SessionProtocolCodec;
using transport::tcp::SessionReplaced;
using transport::tcp::SessionReplacedReason;
using transport::tcp::Welcome;

constexpr std::uint32_t kRudpBindCapabilityTtlMillis = 15000;

std::vector<std::byte> requiredFrame(SessionControlMessage message) {
  auto frame = SessionProtocolCodec::encodeFrame(message);
  if (!frame.has_value()) {
    throw std::logic_error{"invalid session authentication result"};
  }
  return std::move(*frame);
}

AuthenticationRejectedReason rejectionReason(meta::ClaimOutcome outcome) {
  switch (outcome) {
  case meta::ClaimOutcome::Invalid:
    return AuthenticationRejectedReason::Invalid;
  case meta::ClaimOutcome::Expired:
    return AuthenticationRejectedReason::Expired;
  case meta::ClaimOutcome::AlreadyConsumed:
    return AuthenticationRejectedReason::AlreadyConsumed;
  case meta::ClaimOutcome::WrongAudience:
    return AuthenticationRejectedReason::WrongAudience;
  case meta::ClaimOutcome::Claimed:
  case meta::ClaimOutcome::DependencyUnavailable:
  case meta::ClaimOutcome::MalformedResponse:
    return AuthenticationRejectedReason::DependencyUnavailable;
  }
  return AuthenticationRejectedReason::DependencyUnavailable;
}

RoutedSessionFrame rejected(std::uint64_t connectionEpoch,
                            std::uint64_t requestId,
                            AuthenticationRejectedReason reason) {
  return RoutedSessionFrame{
      .connectionEpoch = connectionEpoch,
      .frame = requiredFrame(AuthenticationRejected{requestId, reason}),
      .transition = ConnectionTransition::CloseAfterWrite,
      .closeReason = CloseReason::AuthenticationRejected,
  };
}

} // namespace

SessionAuthFlow::SessionAuthFlow(AuthClaimCoordinator &correlations,
                                 meta::MetaClaimClient &metaClient) noexcept
    : correlations_(correlations), metaClient_(metaClient) {}

std::vector<RoutedSessionFrame>
SessionAuthFlow::begin(std::uint64_t connectionEpoch,
                       const transport::tcp::NormalizedAuthRequest &request) {
  if (!correlations_.beginClaim(connectionEpoch,
                                shared::RequestId{request.requestId})) {
    return {rejected(connectionEpoch, request.requestId,
                     AuthenticationRejectedReason::Invalid)};
  }

  const meta::SubmitStatus status = metaClient_.submit(meta::ClaimSubmission{
      .connectionEpoch = connectionEpoch,
      .requestId = request.requestId,
      .credential = request.credential,
  });
  if (status == meta::SubmitStatus::Accepted) {
    return {};
  }
  static_cast<void>(correlations_.closeConnection(connectionEpoch));
  const auto reason = status == meta::SubmitStatus::InvalidRequest
                          ? AuthenticationRejectedReason::Invalid
                          : AuthenticationRejectedReason::DependencyUnavailable;
  return {rejected(connectionEpoch, request.requestId, reason)};
}

std::vector<RoutedSessionFrame>
SessionAuthFlow::complete(const meta::ClaimCompletion &completion,
                          std::uint64_t serverTimeUnixMillis) {
  const AppliedClaim applied = correlations_.apply(completion);
  if (applied.kind == AppliedClaimKind::Stale) {
    return {};
  }
  if (applied.kind == AppliedClaimKind::Rejected) {
    return {rejected(applied.connectionEpoch, applied.requestId,
                     rejectionReason(applied.outcome))};
  }
  if (!applied.authenticated.has_value()) {
    throw std::logic_error{"accepted claim has no session result"};
  }

  std::vector<RoutedSessionFrame> frames;
  if (applied.replacedConnectionEpoch.has_value()) {
    frames.push_back(RoutedSessionFrame{
        .connectionEpoch = *applied.replacedConnectionEpoch,
        .frame = requiredFrame(
            SessionReplaced{SessionReplacedReason::SameAccountLogin}),
        .transition = ConnectionTransition::CloseAfterWrite,
        .closeReason = CloseReason::SessionReplaced,
    });
  }
  frames.push_back(RoutedSessionFrame{
      .connectionEpoch = applied.connectionEpoch,
      .frame = requiredFrame(Welcome{
          .requestId = applied.requestId,
          .sessionId = applied.authenticated->sessionId.value(),
          .sessionGeneration = applied.authenticated->generation.value(),
          .serverTimeUnixMillis = serverTimeUnixMillis,
          .nickname = applied.nickname,
      }),
      .transition = ConnectionTransition::MarkAuthenticated,
      .closeReason = CloseReason::None,
  });
  return frames;
}

bool SessionAuthFlow::disconnect(std::uint64_t connectionEpoch) {
  return correlations_.closeConnection(connectionEpoch);
}

std::optional<RoutedSessionFrame> SessionAuthFlow::requestRudpBindCapability(
    std::uint64_t connectionEpoch, const RequestRudpBindCapability &request,
    std::chrono::steady_clock::time_point now) {
  const auto capability = correlations_.issueRudpBindCapability(
      connectionEpoch, shared::RequestId{request.requestId}, now);
  if (!capability.has_value()) {
    return std::nullopt;
  }
  return RoutedSessionFrame{
      .connectionEpoch = connectionEpoch,
      .frame = requiredFrame(RudpBindCapability{
          .requestId = request.requestId,
          .ttlMillis = kRudpBindCapabilityTtlMillis,
          .capability = capability->bytes,
      }),
      .transition = ConnectionTransition::KeepAuthenticated,
      .closeReason = CloseReason::None,
  };
}

} // namespace lol::app
