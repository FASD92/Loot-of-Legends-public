#include "AuthClaimCoordinator.hpp"
#include "SessionAuthFlow.hpp"

#include <lol/meta/MetaClaimClient.hpp>
#include <lol/session/SessionRegistry.hpp>
#include <lol/transport/tcp/SessionProtocolCodec.hpp>
#include <lol/transport/tcp/TcpConnection.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
using lol::app::ConnectionTransition;
using lol::app::RoutedSessionFrame;
using lol::app::SessionAuthFlow;
using lol::meta::ClaimCompletion;
using lol::meta::HttpsRequest;
using lol::meta::HttpsResult;
using lol::meta::HttpsStatus;
using lol::meta::MetaClaimClient;
using lol::meta::MetaClaimClientConfig;
using lol::transport::tcp::AuthenticateGameSession;
using lol::transport::tcp::AuthenticationRejected;
using lol::transport::tcp::AuthenticationRejectedReason;
using lol::transport::tcp::BattleResumeDrop;
using lol::transport::tcp::BattleResumeMonster;
using lol::transport::tcp::BattleResumePhase;
using lol::transport::tcp::BattleResumePlayer;
using lol::transport::tcp::BattleResumeSnapshot;
using lol::transport::tcp::BattleResumeSnapshotApplied;
using lol::transport::tcp::CloseReason;
using lol::transport::tcp::CodecError;
using lol::transport::tcp::ConnectionPhase;
using lol::transport::tcp::NormalizedAuthRequest;
using lol::transport::tcp::PreAuthLimits;
using lol::transport::tcp::RequestRudpBindCapability;
using lol::transport::tcp::ResumeBattleSession;
using TcpRudpBindCapability = lol::transport::tcp::RudpBindCapability;
using lol::transport::tcp::SessionControlMessage;
using lol::transport::tcp::SessionProtocolCodec;
using lol::transport::tcp::SessionReplaced;
using lol::transport::tcp::SessionReplacedReason;
using lol::transport::tcp::TcpConnection;
using lol::transport::tcp::Welcome;

constexpr auto kOpenedAt = std::chrono::steady_clock::time_point{};
constexpr auto kCredentialA = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
constexpr auto kCredentialB = "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
constexpr auto kCredentialC = "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC";

std::vector<std::byte> fromHex(std::string_view text) {
  const auto digit = [](char value) -> std::uint8_t {
    if (value >= '0' && value <= '9')
      return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
      return static_cast<std::uint8_t>(value - 'a' + 10);
    return static_cast<std::uint8_t>(value - 'A' + 10);
  };
  std::vector<std::byte> bytes;
  bytes.reserve(text.size() / 2);
  for (std::size_t index = 0; index < text.size(); index += 2) {
    bytes.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(
        (digit(text[index]) << 4U) | digit(text[index + 1]))));
  }
  return bytes;
}

bool codecMatchesAllGoldenVectors() {
  const std::array messages{
      SessionControlMessage{AuthenticateGameSession{
          .requestId = 1,
          .credential = kCredentialA,
      }},
      SessionControlMessage{Welcome{
          .requestId = 1,
          .sessionId = 2,
          .sessionGeneration = 3,
          .serverTimeUnixMillis = 1'700'000'000'000ULL,
          .nickname = "player-one",
      }},
      SessionControlMessage{AuthenticationRejected{
          .requestId = 1,
          .reason = AuthenticationRejectedReason::Expired,
      }},
      SessionControlMessage{SessionReplaced{
          .reason = SessionReplacedReason::SameAccountLogin,
      }},
  };
  const std::array expected{
      fromHex("0000003a01000000010000000000000001002b41414141414141414141414141"
              "414141414141414141414141414141414141414141414141414141414141"),
      fromHex("0000003101000000020000000000000001000000000000000200000000"
              "000000030000018bcfe56800000a706c617965722d6f6e65"),
      fromHex("0000000f010000000300000000000000010002"),
      fromHex("0000000701000000040001"),
  };

  for (std::size_t index = 0; index < messages.size(); ++index) {
    const auto encoded = SessionProtocolCodec::encodeFrame(messages[index]);
    if (!encoded.has_value() || *encoded != expected[index]) {
      return false;
    }
    const auto decoded = SessionProtocolCodec::decodeFrame(*encoded);
    if (decoded.error != CodecError::None || !decoded.message.has_value() ||
        *decoded.message != messages[index]) {
      return false;
    }
  }
  return true;
}

bool resumeControlMessagesMatchGoldenVectors() {
  const SessionControlMessage request{ResumeBattleSession{
      .requestId = 7,
      .previousSessionId = 2,
      .previousSessionGeneration = 3,
      .credential = kCredentialA,
  }};
  const auto encodedRequest = SessionProtocolCodec::encodeFrame(request);
  const auto expectedRequest = fromHex(
      "0000004a0100000028000000000000000700000000000000020000000000000003"
      "002b41414141414141414141414141414141414141414141414141414141414141"
      "414141414141414141414141");
  if (!encodedRequest.has_value() || *encodedRequest != expectedRequest) {
    return false;
  }
  const auto preAuth = SessionProtocolCodec::decodePreAuthPayload(
      std::span{*encodedRequest}.subspan(4));
  if (preAuth.kind !=
          lol::transport::tcp::DecodedPreAuthFrame::Kind::Authenticate ||
      !preAuth.request.has_value() || !preAuth.request->resumeRequested ||
      preAuth.request->previousSessionId != 2 ||
      preAuth.request->previousSessionGeneration != 3 ||
      preAuth.request->credential != kCredentialA) {
    return false;
  }

  const SessionControlMessage snapshot{BattleResumeSnapshot{
      .requestId = 7,
      .snapshotId = 11,
      .roomId = 5,
      .battleInstanceId = 9,
      .playerSessionId = 2,
      .sessionGeneration = 3,
      .phase = BattleResumePhase::Combat,
      .remainingMillis = 12345,
      .serverTick = 99,
      .players = {BattleResumePlayer{.sessionId = 2,
                                     .positionXMillimeters = -100,
                                     .positionYMillimeters = 200,
                                     .healthKnown = false,
                                     .hitPoints = 0,
                                     .maximumHitPoints = 0,
                                     .alive = true},
                  BattleResumePlayer{.sessionId = 4,
                                     .positionXMillimeters = 300,
                                     .positionYMillimeters = -400,
                                     .healthKnown = false,
                                     .hitPoints = 0,
                                     .maximumHitPoints = 0,
                                     .alive = true}},
      .monster = BattleResumeMonster{.monsterId = 1,
                                     .positionXMillimeters = 0,
                                     .positionYMillimeters = 0,
                                     .hitPoints = 1200,
                                     .maximumHitPoints = 1600,
                                     .state = 0},
      .drops = {BattleResumeDrop{.dropId = 1,
                                 .itemId = 2,
                                 .quantity = 1,
                                 .positionXMillimeters = 10,
                                 .positionYMillimeters = 20,
                                 .state = 0,
                                 .ownerSessionId = 0}},
      .score = 0,
      .result = std::nullopt,
  }};
  const auto encodedSnapshot = SessionProtocolCodec::encodeFrame(snapshot);
  if (!encodedSnapshot.has_value()) {
    return false;
  }
  const auto decodedSnapshot =
      SessionProtocolCodec::decodeFrame(*encodedSnapshot);
  if (decodedSnapshot.error != CodecError::None ||
      !decodedSnapshot.message.has_value() ||
      *decodedSnapshot.message != snapshot) {
    return false;
  }

  const SessionControlMessage applied{
      BattleResumeSnapshotApplied{.snapshotId = 11}};
  const auto encodedApplied = SessionProtocolCodec::encodeFrame(applied);
  const auto expectedApplied = fromHex("0000000d0100000029000000000000000b");
  const auto decodedApplied =
      encodedApplied.has_value()
          ? SessionProtocolCodec::decodeFrame(*encodedApplied)
          : lol::transport::tcp::DecodedSessionFrame{};
  return encodedApplied.has_value() && *encodedApplied == expectedApplied &&
         decodedApplied.error == CodecError::None &&
         decodedApplied.message.has_value() &&
         *decodedApplied.message == applied;
}

class CompletionCollector final {
public:
  void add(ClaimCompletion completion) {
    std::lock_guard lock{mutex_};
    completions_.push_back(std::move(completion));
  }

  std::vector<ClaimCompletion> take() {
    std::lock_guard lock{mutex_};
    return std::exchange(completions_, {});
  }

private:
  std::mutex mutex_;
  std::vector<ClaimCompletion> completions_;
};

PreAuthLimits limits() {
  return PreAuthLimits{
      .maxFrameBytes = 256,
      .maxPreAuthBytes = 256,
      .maxPreAuthReadEvents = 4,
      .preAuthTimeout = 100ms,
      .maxOutboundBytes = 512,
  };
}

std::optional<std::vector<RoutedSessionFrame>>
authenticate(std::uint64_t epoch, std::uint64_t requestId,
             std::string credential, std::uint64_t serverTimeUnixMillis,
             TcpConnection &connection, SessionAuthFlow &flow,
             MetaClaimClient &client, CompletionCollector &collector) {
  const auto frame = SessionProtocolCodec::encodeFrame(
      AuthenticateGameSession{requestId, std::move(credential)});
  if (!frame.has_value()) {
    return std::nullopt;
  }
  std::vector<NormalizedAuthRequest> requests;
  connection.onBytes(*frame, kOpenedAt + 1ms,
                     SessionProtocolCodec::decodePreAuthPayload,
                     [&requests](NormalizedAuthRequest request) {
                       requests.push_back(std::move(request));
                     });
  if (requests.size() != 1 || !flow.begin(epoch, requests.front()).empty() ||
      !client.waitUntilIdle(1s)) {
    return std::nullopt;
  }
  auto completions = collector.take();
  if (completions.size() != 1) {
    return std::nullopt;
  }
  return flow.complete(completions.front(), serverTimeUnixMillis);
}

bool applyDispatch(TcpConnection &connection,
                   const RoutedSessionFrame &dispatch) {
  if (!connection.queueOutbound(dispatch.frame)) {
    return false;
  }
  if (dispatch.transition == ConnectionTransition::MarkAuthenticated) {
    return dispatch.closeReason == CloseReason::None &&
           connection.markAuthenticated();
  }
  if (dispatch.transition == ConnectionTransition::KeepAuthenticated) {
    return dispatch.closeReason == CloseReason::None &&
           connection.phase() == ConnectionPhase::Authenticated;
  }
  return dispatch.transition == ConnectionTransition::CloseAfterWrite &&
         dispatch.closeReason != CloseReason::None &&
         connection.beginClose(dispatch.closeReason);
}

bool endToEndCoversReplayExpiryReplacementAndStaleDisconnect() {
  std::vector<HttpsResult> responses{
      HttpsResult{
          .status = HttpsStatus::Response,
          .statusCode = 200,
          .body =
              R"({"accountId":"00000000-0000-4000-8000-000000000001","nickname":"player-one"})"},
      HttpsResult{.status = HttpsStatus::Response,
                  .statusCode = 409,
                  .body = R"({"code":"ALREADY_CONSUMED"})"},
      HttpsResult{.status = HttpsStatus::Response,
                  .statusCode = 410,
                  .body = R"({"code":"EXPIRED"})"},
      HttpsResult{
          .status = HttpsStatus::Response,
          .statusCode = 200,
          .body =
              R"({"accountId":"00000000-0000-4000-8000-000000000001","nickname":"player-one"})"},
  };
  std::size_t nextResponse = 0;
  CompletionCollector collector;
  lol::session::SessionRegistry sessions;
  lol::transport::rudp::RudpBindingRegistry rudpBindings;
  lol::app::AuthClaimCoordinator correlations{sessions, rudpBindings};
  MetaClaimClient client{
      MetaClaimClientConfig{
          .claimUrl = "https://meta.test/internal/v1/game-credentials/claim",
          .serviceCredential = "test-only-service-placeholder",
          .timeout = 50ms,
          .maxOutstanding = 4,
          .maxResponseBytes = 1024,
      },
      [&responses, &nextResponse](HttpsRequest, std::chrono::milliseconds) {
        return responses.at(nextResponse++);
      },
      [&collector](ClaimCompletion completion) {
        collector.add(std::move(completion));
      }};
  SessionAuthFlow flow{correlations, client};

  TcpConnection first{limits(), kOpenedAt};
  auto firstDispatch = authenticate(101, 1, kCredentialA, 1'700'000'000'000ULL,
                                    first, flow, client, collector);
  if (!firstDispatch.has_value() || firstDispatch->size() != 1 ||
      firstDispatch->front().connectionEpoch != 101 ||
      !applyDispatch(first, firstDispatch->front()) ||
      first.phase() != ConnectionPhase::Authenticated ||
      sessions.activeSessionCount() != 1) {
    return false;
  }
  const auto firstWelcome =
      SessionProtocolCodec::decodeFrame(firstDispatch->front().frame);
  if (firstWelcome.error != CodecError::None ||
      !std::holds_alternative<Welcome>(*firstWelcome.message)) {
    return false;
  }
  const auto capabilityDispatch = flow.requestRudpBindCapability(
      101, RequestRudpBindCapability{.requestId = 5}, kOpenedAt + 2ms);
  if (!capabilityDispatch.has_value() ||
      !applyDispatch(first, *capabilityDispatch)) {
    return false;
  }
  const auto capability =
      SessionProtocolCodec::decodeFrame(capabilityDispatch->frame);
  const auto *capabilityMessage =
      capability.message.has_value()
          ? std::get_if<TcpRudpBindCapability>(&*capability.message)
          : nullptr;
  if (capabilityMessage == nullptr || capabilityMessage->requestId != 5 ||
      capabilityMessage->ttlMillis != 15000) {
    return false;
  }

  TcpConnection replay{limits(), kOpenedAt};
  auto replayDispatch = authenticate(102, 2, kCredentialA, 1'700'000'000'001ULL,
                                     replay, flow, client, collector);
  if (!replayDispatch.has_value() || replayDispatch->size() != 1 ||
      !applyDispatch(replay, replayDispatch->front()) ||
      replay.closeReason() != CloseReason::AuthenticationRejected) {
    return false;
  }
  const auto replayRejected =
      SessionProtocolCodec::decodeFrame(replayDispatch->front().frame);
  const auto *replayMessage =
      replayRejected.message.has_value()
          ? std::get_if<AuthenticationRejected>(&*replayRejected.message)
          : nullptr;
  if (replayMessage == nullptr ||
      replayMessage->reason != AuthenticationRejectedReason::AlreadyConsumed ||
      sessions.activeSessionCount() != 1) {
    return false;
  }

  TcpConnection expired{limits(), kOpenedAt};
  auto expiredDispatch =
      authenticate(103, 3, kCredentialB, 1'700'000'000'002ULL, expired, flow,
                   client, collector);
  if (!expiredDispatch.has_value() || expiredDispatch->size() != 1 ||
      !applyDispatch(expired, expiredDispatch->front())) {
    return false;
  }
  const auto expiredRejected =
      SessionProtocolCodec::decodeFrame(expiredDispatch->front().frame);
  const auto *expiredMessage =
      expiredRejected.message.has_value()
          ? std::get_if<AuthenticationRejected>(&*expiredRejected.message)
          : nullptr;
  if (expiredMessage == nullptr ||
      expiredMessage->reason != AuthenticationRejectedReason::Expired ||
      sessions.activeSessionCount() != 1) {
    return false;
  }

  TcpConnection replacement{limits(), kOpenedAt};
  auto replacementDispatch =
      authenticate(104, 4, kCredentialC, 1'700'000'000'003ULL, replacement,
                   flow, client, collector);
  if (!replacementDispatch.has_value() || replacementDispatch->size() != 2 ||
      replacementDispatch->at(0).connectionEpoch != 101 ||
      replacementDispatch->at(1).connectionEpoch != 104 ||
      !applyDispatch(first, replacementDispatch->at(0)) ||
      !applyDispatch(replacement, replacementDispatch->at(1)) ||
      first.closeReason() != CloseReason::SessionReplaced ||
      replacement.phase() != ConnectionPhase::Authenticated ||
      sessions.activeSessionCount() != 1) {
    return false;
  }
  const auto replaced =
      SessionProtocolCodec::decodeFrame(replacementDispatch->at(0).frame);
  if (!replaced.message.has_value() ||
      !std::holds_alternative<SessionReplaced>(*replaced.message)) {
    return false;
  }

  return !flow.disconnect(101) && sessions.activeSessionCount() == 1 &&
         flow.disconnect(104) && !flow.disconnect(104) &&
         sessions.activeSessionCount() == 0;
}

bool freshCredentialResumeHasOneOwnerAndKeepsSessionIdentity() {
  std::size_t nextResponse = 0;
  const HttpsResult claimed{
      .status = HttpsStatus::Response,
      .statusCode = 200,
      .body =
          R"({"accountId":"00000000-0000-4000-8000-000000000001","nickname":"player-one"})"};
  CompletionCollector collector;
  lol::session::SessionRegistry sessions;
  lol::transport::rudp::RudpBindingRegistry rudpBindings;
  lol::app::AuthClaimCoordinator correlations{sessions, rudpBindings};
  MetaClaimClient client{
      MetaClaimClientConfig{
          .claimUrl = "https://meta.test/internal/v1/game-credentials/claim",
          .serviceCredential = "test-only-service-placeholder",
          .timeout = 50ms,
          .maxOutstanding = 4,
          .maxResponseBytes = 1024,
      },
      [&nextResponse, claimed](HttpsRequest, std::chrono::milliseconds) {
        ++nextResponse;
        return claimed;
      },
      [&collector](ClaimCompletion completion) {
        collector.add(std::move(completion));
      }};
  SessionAuthFlow flow{correlations, client};

  TcpConnection first{limits(), kOpenedAt};
  auto firstDispatch =
      authenticate(201, 10, kCredentialA, 1000, first, flow, client, collector);
  if (!firstDispatch.has_value() || firstDispatch->size() != 1) {
    return false;
  }
  const auto decodedWelcome =
      SessionProtocolCodec::decodeFrame(firstDispatch->front().frame);
  const auto *welcome = decodedWelcome.message.has_value()
                            ? std::get_if<Welcome>(&*decodedWelcome.message)
                            : nullptr;
  if (welcome == nullptr ||
      !flow.detach(201, kOpenedAt + std::chrono::seconds{30})) {
    return false;
  }

  TcpConnection winner{limits(), kOpenedAt};
  TcpConnection loser{limits(), kOpenedAt};
  const auto beginResume = [&](TcpConnection &connection, std::uint64_t epoch,
                               std::uint64_t requestId,
                               const char *credential) {
    const auto frame = SessionProtocolCodec::encodeFrame(ResumeBattleSession{
        .requestId = requestId,
        .previousSessionId = welcome->sessionId,
        .previousSessionGeneration = welcome->sessionGeneration,
        .credential = credential,
    });
    if (!frame.has_value()) {
      return false;
    }
    std::vector<NormalizedAuthRequest> requests;
    connection.onBytes(*frame, kOpenedAt + 1ms,
                       SessionProtocolCodec::decodePreAuthPayload,
                       [&requests](NormalizedAuthRequest request) {
                         requests.push_back(std::move(request));
                       });
    return requests.size() == 1 && flow.begin(epoch, requests.front()).empty();
  };
  if (!beginResume(winner, 202, 11, kCredentialB) ||
      !beginResume(loser, 203, 12, kCredentialC) || !client.waitUntilIdle(1s)) {
    return false;
  }
  auto completions = collector.take();
  if (completions.size() != 2 || nextResponse != 3) {
    return false;
  }
  auto winnerDispatch = flow.complete(completions[0], 1001, kOpenedAt + 29s);
  auto loserDispatch = flow.complete(completions[1], 1002, kOpenedAt + 29s);
  if (winnerDispatch.size() != 1 || loserDispatch.size() != 1 ||
      winnerDispatch[0].transition != ConnectionTransition::MarkAuthenticated ||
      loserDispatch[0].transition != ConnectionTransition::CloseAfterWrite) {
    return false;
  }
  const auto resumed =
      SessionProtocolCodec::decodeFrame(winnerDispatch[0].frame);
  const auto rejected =
      SessionProtocolCodec::decodeFrame(loserDispatch[0].frame);
  const auto *resumedWelcome = resumed.message.has_value()
                                   ? std::get_if<Welcome>(&*resumed.message)
                                   : nullptr;
  const auto *resumeRejected =
      rejected.message.has_value()
          ? std::get_if<AuthenticationRejected>(&*rejected.message)
          : nullptr;
  return resumedWelcome != nullptr &&
         resumedWelcome->sessionId == welcome->sessionId &&
         resumedWelcome->sessionGeneration == welcome->sessionGeneration &&
         resumeRejected != nullptr &&
         resumeRejected->reason ==
             AuthenticationRejectedReason::ResumeUnavailable &&
         sessions.activeSessionCount() == 1;
}

bool unsupportedMessageRemainsRejected() {
  static_assert(std::variant_size_v<SessionControlMessage> == 9);
  const auto unknownFrame = fromHex("000000050100000005");
  const auto unknown = SessionProtocolCodec::decodeFrame(unknownFrame);
  const auto unknownPreAuth = SessionProtocolCodec::decodePreAuthPayload(
      std::span{unknownFrame}.subspan(4));
  const auto wrongVersion =
      SessionProtocolCodec::decodeFrame(fromHex("0000000702000000040001"));
  const auto trailingByte =
      SessionProtocolCodec::decodeFrame(fromHex("000000070100000004000100"));
  const auto invalidCredential = SessionProtocolCodec::encodeFrame(
      AuthenticateGameSession{1, std::string(42, 'A')});
  return unknown.error == CodecError::UnsupportedMessage &&
         !unknown.message.has_value() &&
         unknownPreAuth.kind ==
             lol::transport::tcp::DecodedPreAuthFrame::Kind::OtherMessage &&
         wrongVersion.error == CodecError::UnsupportedVersion &&
         trailingByte.error == CodecError::FrameLengthMismatch &&
         !invalidCredential.has_value();
}

} // namespace

int main() {
  if (!codecMatchesAllGoldenVectors()) {
    return 1;
  }
  if (!endToEndCoversReplayExpiryReplacementAndStaleDisconnect()) {
    return 2;
  }
  if (!resumeControlMessagesMatchGoldenVectors()) {
    return 3;
  }
  if (!freshCredentialResumeHasOneOwnerAndKeepsSessionIdentity()) {
    return 4;
  }
  if (!unsupportedMessageRemainsRejected()) {
    return 5;
  }
  return EXIT_SUCCESS;
}
