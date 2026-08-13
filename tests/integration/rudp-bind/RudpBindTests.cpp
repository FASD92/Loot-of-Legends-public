#include "AuthClaimCoordinator.hpp"

#include <lol/meta/MetaClaimClient.hpp>
#include <lol/session/SessionRegistry.hpp>
#include <lol/transport/rudp/RudpBindingRegistry.hpp>
#include <lol/transport/rudp/RudpCodec.hpp>
#include <lol/transport/tcp/SessionProtocolCodec.hpp>

#include <array>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;
using lol::app::AppliedClaimKind;
using lol::app::AuthClaimCoordinator;
using lol::meta::ClaimCompletion;
using lol::meta::ClaimedIdentity;
using lol::meta::ClaimOutcome;
using lol::session::SessionRegistry;
using lol::shared::RequestId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;
using lol::transport::rudp::RudpBindAccepted;
using lol::transport::rudp::RudpBindCapability;
using lol::transport::rudp::RudpBindHello;
using lol::transport::rudp::RudpBindingRegistry;
using lol::transport::rudp::RudpBindStatus;
using lol::transport::rudp::RudpControlCodec;
using lol::transport::rudp::RudpControlCodecError;
using lol::transport::rudp::RudpControlMessage;
using lol::transport::rudp::RudpEndpoint;
using lol::transport::rudp::RudpFlag;
using lol::transport::rudp::RudpHeader;
using lol::transport::rudp::RudpHeartbeat;
using lol::transport::rudp::RudpPacketStatus;
using TcpBindCapability = lol::transport::tcp::RudpBindCapability;
using lol::transport::tcp::CodecError;
using lol::transport::tcp::RequestRudpBindCapability;
using lol::transport::tcp::SessionProtocolCodec;

constexpr auto kStart = std::chrono::steady_clock::time_point{};

std::vector<std::byte> fromHex(std::string_view text) {
  const auto digit = [](char value) -> std::uint8_t {
    if (value >= '0' && value <= '9') {
      return static_cast<std::uint8_t>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
      return static_cast<std::uint8_t>(value - 'a' + 10);
    }
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

std::optional<std::string> readGolden() {
  std::ifstream input{LOOT_RUDP_BIND_GOLDEN_PATH};
  if (!input) {
    return std::nullopt;
  }
  return std::string{std::istreambuf_iterator<char>{input},
                     std::istreambuf_iterator<char>{}};
}

std::optional<std::vector<std::byte>> goldenBytes(std::string_view contract,
                                                  std::string_view semanticName,
                                                  std::string_view field) {
  const std::string semanticMarker =
      "\"semanticName\": \"" + std::string{semanticName} + "\"";
  const auto message = contract.find(semanticMarker);
  const std::string fieldMarker = "\"" + std::string{field} + "\": \"";
  const auto encoded = message == std::string_view::npos
                           ? std::string_view::npos
                           : contract.find(fieldMarker, message);
  if (encoded == std::string_view::npos) {
    return std::nullopt;
  }
  const auto first = encoded + fieldMarker.size();
  const auto last = contract.find('"', first);
  return last == std::string_view::npos
             ? std::nullopt
             : std::optional{fromHex(contract.substr(first, last - first))};
}

RudpBindCapability syntheticCapability() {
  RudpBindCapability::Bytes bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::byte>(index);
  }
  return RudpBindCapability{bytes};
}

bool codecsMatchFrozenBindGolden() {
  const auto contract = readGolden();
  if (!contract.has_value()) {
    return false;
  }
  const auto capability = syntheticCapability();
  const std::array tcpMessages{
      lol::transport::tcp::SessionControlMessage{
          RequestRudpBindCapability{.requestId = 1}},
      lol::transport::tcp::SessionControlMessage{TcpBindCapability{
          .requestId = 1,
          .ttlMillis = 15000,
          .capability = capability.bytes,
      }},
  };
  const std::array<std::string_view, 2> tcpNames{"RequestRudpBindCapability",
                                                 "RudpBindCapability"};
  for (std::size_t index = 0; index < tcpMessages.size(); ++index) {
    const auto expected = goldenBytes(*contract, tcpNames[index], "frameHex");
    const auto encoded = SessionProtocolCodec::encodeFrame(tcpMessages[index]);
    const auto decoded = expected.has_value()
                             ? SessionProtocolCodec::decodeFrame(*expected)
                             : lol::transport::tcp::DecodedSessionFrame{};
    if (!expected.has_value() || !encoded.has_value() ||
        *encoded != *expected || decoded.error != CodecError::None ||
        !decoded.message.has_value() ||
        *decoded.message != tcpMessages[index]) {
      return false;
    }
  }

  struct RudpGolden final {
    std::string_view name;
    RudpHeader header;
    RudpControlMessage message;
  };
  const std::array rudpMessages{
      RudpGolden{"RudpBindHello",
                 RudpHeader{.flag = RudpFlag::Reliable,
                            .sessionId = 1,
                            .sessionGeneration = 2,
                            .transportEpoch = 0,
                            .sequence = 1,
                            .ack = 0,
                            .ackBits = 0,
                            .messageId = 22},
                 RudpBindHello{capability}},
      RudpGolden{"RudpBindAccepted",
                 RudpHeader{.flag = RudpFlag::Reliable,
                            .sessionId = 1,
                            .sessionGeneration = 2,
                            .transportEpoch = 3,
                            .sequence = 1,
                            .ack = 1,
                            .ackBits = 0,
                            .messageId = 23},
                 RudpBindAccepted{}},
      RudpGolden{"RudpHeartbeat",
                 RudpHeader{.flag = RudpFlag::Heartbeat,
                            .sessionId = 1,
                            .sessionGeneration = 2,
                            .transportEpoch = 3,
                            .sequence = 2,
                            .ack = 1,
                            .ackBits = 0,
                            .messageId = 24},
                 RudpHeartbeat{}},
  };
  for (const auto &golden : rudpMessages) {
    const auto expected = goldenBytes(*contract, golden.name, "datagramHex");
    const auto encoded =
        RudpControlCodec::encode(golden.header, golden.message);
    const auto decoded = expected.has_value()
                             ? RudpControlCodec::decode(*expected)
                             : lol::transport::rudp::DecodedRudpControl{};
    if (!expected.has_value() || !encoded.has_value() ||
        *encoded != *expected || decoded.error != RudpControlCodecError::None ||
        decoded.header != std::optional{golden.header} ||
        decoded.message != std::optional{golden.message}) {
      return false;
    }
  }
  return true;
}

lol::shared::AccountId account(std::uint8_t suffix) {
  lol::shared::AccountId::Bytes bytes{};
  bytes.back() = suffix;
  return lol::shared::AccountId{bytes};
}

RudpEndpoint endpoint(std::uint8_t suffix, std::uint16_t port) {
  RudpEndpoint value{.address = {}, .port = port, .scopeId = 0};
  value.address.back() = static_cast<std::byte>(suffix);
  return value;
}

RudpHeader helloHeader(SessionId sessionId, SessionGeneration generation,
                       std::uint32_t sequence = 1) {
  return RudpHeader{.flag = RudpFlag::Reliable,
                    .sessionId = sessionId.value(),
                    .sessionGeneration = generation.value(),
                    .transportEpoch = 0,
                    .sequence = sequence,
                    .ack = 0,
                    .ackBits = 0,
                    .messageId = 22};
}

bool capabilityLifecycleAndRebindAreBounded() {
  SessionRegistry sessions;
  const auto authenticated =
      sessions.authenticate({RequestId{1}, {account(1), "player-one"}});
  RudpBindingRegistry bindings;
  const auto firstCapability =
      bindings.requestCapability(authenticated.sessionId.value(),
                                 authenticated.generation.value(), kStart);
  if (!firstCapability.has_value()) {
    return false;
  }

  auto wrongCapability = *firstCapability;
  wrongCapability.bytes.front() ^= std::byte{1};
  if (bindings.bind(helloHeader(authenticated.sessionId,
                                authenticated.generation),
                    RudpBindHello{wrongCapability}, endpoint(1, 4000), kStart)
              .status != RudpBindStatus::CapabilityMismatch ||
      bindings.bind(helloHeader(authenticated.sessionId,
                                SessionGeneration{
                                    authenticated.generation.value() + 1}),
                    RudpBindHello{*firstCapability}, endpoint(1, 4000), kStart)
              .status != RudpBindStatus::WrongGeneration) {
    return false;
  }

  const auto first = bindings.bind(
      helloHeader(authenticated.sessionId, authenticated.generation),
      RudpBindHello{*firstCapability}, endpoint(1, 4000), kStart);
  const auto replayed = bindings.bind(
      helloHeader(authenticated.sessionId, authenticated.generation),
      RudpBindHello{*firstCapability}, endpoint(1, 4000), kStart + 1ms);
  if (first.status != RudpBindStatus::Accepted || first.transportEpoch == 0 ||
      first.sequence != 1 || first.ack.ack != 1 || first.ack.ackBits != 0 ||
      replayed.status != RudpBindStatus::Accepted ||
      replayed.transportEpoch != first.transportEpoch ||
      replayed.sequence != first.sequence || replayed.ack != first.ack ||
      bindings.bind(helloHeader(authenticated.sessionId,
                                authenticated.generation, 2),
                    RudpBindHello{*firstCapability}, endpoint(1, 4000),
                    kStart + 2ms)
              .status != RudpBindStatus::CapabilityMissing ||
      bindings.bind(helloHeader(authenticated.sessionId,
                                authenticated.generation),
                    RudpBindHello{*firstCapability}, endpoint(2, 4001),
                    kStart + 2ms)
              .status != RudpBindStatus::CapabilityMissing) {
    return false;
  }

  const auto expiring =
      bindings.requestCapability(authenticated.sessionId.value(),
                                 authenticated.generation.value(), kStart);
  if (!expiring.has_value() ||
      bindings.bind(helloHeader(authenticated.sessionId,
                                authenticated.generation),
                    RudpBindHello{*expiring}, endpoint(2, 4001), kStart + 15s)
              .status != RudpBindStatus::CapabilityExpired) {
    return false;
  }

  const auto rebindCapability = bindings.requestCapability(
      authenticated.sessionId.value(), authenticated.generation.value(),
      kStart + 15001ms);
  if (!rebindCapability.has_value()) {
    return false;
  }
  const auto rebound = bindings.bind(
      helloHeader(authenticated.sessionId, authenticated.generation),
      RudpBindHello{*rebindCapability}, endpoint(2, 4001), kStart + 15001ms);
  return rebound.status == RudpBindStatus::Accepted &&
         rebound.transportEpoch != 0 &&
         rebound.transportEpoch != first.transportEpoch &&
         bindings.status(authenticated.sessionId.value(),
                         authenticated.generation.value(), first.transportEpoch,
                         endpoint(1, 4000)) == RudpPacketStatus::StaleEpoch &&
         bindings.status(authenticated.sessionId.value(),
                         authenticated.generation.value(),
                         rebound.transportEpoch, endpoint(1, 4000)) ==
             RudpPacketStatus::WrongEndpoint &&
         bindings.status(authenticated.sessionId.value(),
                         authenticated.generation.value(),
                         rebound.transportEpoch,
                         endpoint(2, 4001)) == RudpPacketStatus::Current;
}

lol::app::AppliedClaim authenticate(AuthClaimCoordinator &coordinator,
                                    std::uint64_t connectionEpoch,
                                    std::uint64_t requestId,
                                    std::uint8_t accountSuffix) {
  if (!coordinator.beginClaim(connectionEpoch, RequestId{requestId})) {
    return {};
  }
  ClaimedIdentity identity{
      .accountId = {}, .nickname = "player-" + std::to_string(accountSuffix)};
  identity.accountId.back() = accountSuffix;
  return coordinator.apply(ClaimCompletion{connectionEpoch, requestId,
                                           ClaimOutcome::Claimed, identity});
}

bool sessionReplacementAndCloseInvalidatePeer() {
  SessionRegistry sessions;
  RudpBindingRegistry bindings;
  AuthClaimCoordinator coordinator{sessions, bindings};

  if (coordinator.issueRudpBindCapability(999, RequestId{1}, kStart)
          .has_value()) {
    return false;
  }

  const auto first = authenticate(coordinator, 101, 1, 7);
  if (first.kind != AppliedClaimKind::Accepted ||
      !first.authenticated.has_value()) {
    return false;
  }
  const auto firstCapability =
      coordinator.issueRudpBindCapability(101, RequestId{3}, kStart);
  if (!firstCapability.has_value()) {
    return false;
  }
  const auto firstBind =
      bindings.bind(helloHeader(first.authenticated->sessionId,
                                first.authenticated->generation),
                    RudpBindHello{*firstCapability}, endpoint(1, 4000), kStart);
  if (firstBind.status != RudpBindStatus::Accepted) {
    return false;
  }

  const auto replacement = authenticate(coordinator, 102, 2, 7);
  if (replacement.kind != AppliedClaimKind::Accepted ||
      !replacement.authenticated.has_value() ||
      bindings.status(first.authenticated->sessionId.value(),
                      first.authenticated->generation.value(),
                      firstBind.transportEpoch,
                      endpoint(1, 4000)) != RudpPacketStatus::NotBound) {
    return false;
  }

  const auto secondCapability =
      coordinator.issueRudpBindCapability(102, RequestId{4}, kStart);
  if (!secondCapability.has_value()) {
    return false;
  }
  const auto secondBind = bindings.bind(
      helloHeader(replacement.authenticated->sessionId,
                  replacement.authenticated->generation),
      RudpBindHello{*secondCapability}, endpoint(2, 4001), kStart);
  return secondBind.status == RudpBindStatus::Accepted &&
         !coordinator.closeConnection(101) &&
         coordinator.closeConnection(102) &&
         bindings.status(replacement.authenticated->sessionId.value(),
                         replacement.authenticated->generation.value(),
                         secondBind.transportEpoch,
                         endpoint(2, 4001)) == RudpPacketStatus::NotBound &&
         bindings.boundCount() == 0;
}

bool heartbeatRefreshesPeerAndTimeoutClosesSession() {
  SessionRegistry sessions;
  RudpBindingRegistry bindings;
  AuthClaimCoordinator coordinator{sessions, bindings};

  const auto authenticated = authenticate(coordinator, 201, 1, 8);
  if (authenticated.kind != AppliedClaimKind::Accepted ||
      !authenticated.authenticated.has_value()) {
    return false;
  }
  const auto capability =
      coordinator.issueRudpBindCapability(201, RequestId{2}, kStart);
  if (!capability.has_value()) {
    return false;
  }
  const auto peer = endpoint(8, 4008);
  const auto bound =
      bindings.bind(helloHeader(authenticated.authenticated->sessionId,
                                authenticated.authenticated->generation),
                    RudpBindHello{*capability}, peer, kStart);
  if (bound.status != RudpBindStatus::Accepted ||
      !coordinator.expireTimedOutRudpPeers(kStart + 4999ms).empty()) {
    return false;
  }

  const auto heartbeat = RudpControlCodec::encode(
      RudpHeader{.flag = RudpFlag::Heartbeat,
                 .sessionId = authenticated.authenticated->sessionId.value(),
                 .sessionGeneration =
                     authenticated.authenticated->generation.value(),
                 .transportEpoch = bound.transportEpoch,
                 .sequence = 2,
                 .ack = 1,
                 .ackBits = 0,
                 .messageId = 24},
      RudpControlMessage{RudpHeartbeat{}});
  const auto decoded = heartbeat.has_value()
                           ? RudpControlCodec::decode(*heartbeat)
                           : lol::transport::rudp::DecodedRudpControl{};
  if (decoded.error != RudpControlCodecError::None ||
      !decoded.header.has_value() ||
      bindings.receive(*decoded.header, peer, kStart + 4999ms).status !=
          RudpPacketStatus::Current ||
      bindings.receive(*decoded.header, endpoint(9, 4009), kStart + 9998ms)
              .status != RudpPacketStatus::WrongEndpoint ||
      !coordinator.expireTimedOutRudpPeers(kStart + 9998ms).empty()) {
    return false;
  }

  const auto expired = coordinator.expireTimedOutRudpPeers(kStart + 9999ms);
  return expired.size() == 1 &&
         expired.front().connectionEpoch == std::optional<std::uint64_t>{201} &&
         expired.front().sessionId == authenticated.authenticated->sessionId &&
         expired.front().generation ==
             authenticated.authenticated->generation &&
         sessions.activeSessionCount() == 0 && bindings.boundCount() == 0 &&
         !coordinator.closeConnection(201);
}

bool concurrentIngressAndOutboundShareABoundedOwner() {
  constexpr std::uint64_t kSessionId = 91;
  constexpr std::uint64_t kSessionGeneration = 7;
  constexpr std::size_t kIterations = 4096;

  RudpBindingRegistry bindings;
  const auto capability =
      bindings.requestCapability(kSessionId, kSessionGeneration, kStart);
  if (!capability.has_value()) {
    return false;
  }
  const auto peer = endpoint(9, 4091);
  const auto bound = bindings.bind(
      helloHeader(SessionId{kSessionId}, SessionGeneration{kSessionGeneration}),
      RudpBindHello{*capability}, peer, kStart);
  if (bound.status != RudpBindStatus::Accepted) {
    return false;
  }

  std::barrier start{2};
  bool ingressOk = true;
  bool outboundOk = true;
  std::thread ingress{[&] {
    start.arrive_and_wait();
    for (std::size_t index = 0; index < kIterations; ++index) {
      const auto received = bindings.receive(
          RudpHeader{.flag = RudpFlag::Reliable,
                     .sessionId = kSessionId,
                     .sessionGeneration = kSessionGeneration,
                     .transportEpoch = bound.transportEpoch,
                     .sequence = static_cast<std::uint32_t>(index + 2),
                     .ack = 0,
                     .ackBits = 0,
                     .messageId = 32},
          peer, kStart + std::chrono::milliseconds{index});
      if (received.status != RudpPacketStatus::Current ||
          !received.disposition.has_value()) {
        ingressOk = false;
        return;
      }
    }
  }};
  std::thread outbound{[&] {
    start.arrive_and_wait();
    for (std::size_t index = 0; index < kIterations; ++index) {
      const auto route = bindings.nextOutbound(kSessionId);
      if (!route.has_value() ||
          route->sessionGeneration != kSessionGeneration ||
          route->transportEpoch != bound.transportEpoch ||
          route->endpoint != peer) {
        outboundOk = false;
        return;
      }
    }
  }};
  ingress.join();
  outbound.join();

  return ingressOk && outboundOk &&
         bindings.isBound(kSessionId, kSessionGeneration);
}

} // namespace

int main() {
  static_assert(RudpBindCapability::Bytes{}.size() == 32);
  return codecsMatchFrozenBindGolden() &&
                 capabilityLifecycleAndRebindAreBounded() &&
                 sessionReplacementAndCloseInvalidatePeer() &&
                 heartbeatRefreshesPeerAndTimeoutClosesSession() &&
                 concurrentIngressAndOutboundShareABoundedOwner()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
