#include "RudpMovementFlow.hpp"

#include <lol/game_flow/RoomCommandGateway.hpp>
#include <lol/runtime/WorkerPool.hpp>
#include <lol/transport/rudp/RudpBindingRegistry.hpp>
#include <lol/transport/rudp/RudpMovementCodec.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
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
using lol::app::EncodedRudpDatagram;
using lol::app::RudpGameplayReadiness;
using lol::app::RudpMovementFlow;
using lol::app::RudpMovementSubmitResult;
using lol::battle::StateSnapshotProjection;
using lol::game_flow::ArenaGameplayStart;
using lol::game_flow::AuthenticatedRoomSession;
using lol::game_flow::CreateRoomRequest;
using lol::game_flow::HostStartRequest;
using lol::game_flow::JoinRoomRequest;
using lol::game_flow::LobbyRoomOutboundIntent;
using lol::game_flow::RoomCommandEnvelope;
using lol::game_flow::RoomCommandGateway;
using lol::game_flow::RoomSubmitResult;
using lol::game_flow::SetReadyRequest;
using lol::runtime::WorkerPool;
using lol::runtime::WorkerPoolConfig;
using lol::shared::AccountId;
using lol::shared::BattleInstanceId;
using lol::shared::RequestId;
using lol::shared::RoomId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;
using lol::transport::rudp::RudpBindHello;
using lol::transport::rudp::RudpBindingRegistry;
using lol::transport::rudp::RudpBindStatus;
using lol::transport::rudp::RudpEndpoint;
using lol::transport::rudp::RudpFlag;
using lol::transport::rudp::RudpHeader;
using lol::transport::rudp::RudpMoveIntent;
using lol::transport::rudp::RudpMovementCodec;
using lol::transport::rudp::RudpMovementCodecError;
using lol::transport::rudp::RudpMovementMessage;
using lol::transport::rudp::RudpSnapshotPlayer;
using lol::transport::rudp::RudpStateSnapshot;

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
  std::ifstream input{LOOT_MOVEMENT_GOLDEN_PATH};
  if (!input) {
    return std::nullopt;
  }
  return std::string{std::istreambuf_iterator<char>{input},
                     std::istreambuf_iterator<char>{}};
}

std::optional<std::vector<std::byte>>
goldenDatagram(std::string_view contract, std::string_view semanticName) {
  const std::string semanticMarker =
      "\"semanticName\": \"" + std::string{semanticName} + "\"";
  const auto message = contract.find(semanticMarker);
  constexpr std::string_view fieldMarker = "\"datagramHex\": \"";
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

bool codecMatchesGoldenAndTenPlayerCeiling() {
  const auto contract = readGolden();
  if (!contract.has_value()) {
    return false;
  }
  const RudpHeader moveHeader{.flag = RudpFlag::Unreliable,
                              .sessionId = 1,
                              .sessionGeneration = 2,
                              .transportEpoch = 3,
                              .sequence = 2,
                              .ack = 1,
                              .ackBits = 0,
                              .messageId = 25};
  const RudpMovementMessage move = RudpMoveIntent{
      .battleInstanceId = 7,
      .actionSequence = 9,
      .desiredX = 32767,
      .desiredY = -32767,
      .inputFlags = 0,
  };
  const RudpHeader snapshotHeader{.flag = RudpFlag::Unreliable,
                                  .sessionId = 1,
                                  .sessionGeneration = 2,
                                  .transportEpoch = 3,
                                  .sequence = 3,
                                  .ack = 2,
                                  .ackBits = 1,
                                  .messageId = 26};
  const RudpMovementMessage snapshot = RudpStateSnapshot{
      .battleInstanceId = 7,
      .snapshotSequence = 11,
      .serverTick = 20,
      .players =
          {{.sessionId = 1, .posXMillimeter = 1000, .posYMillimeter = -2000},
           {.sessionId = 3, .posXMillimeter = -10000, .posYMillimeter = 10000}},
  };
  const std::array headers{moveHeader, snapshotHeader};
  const std::array messages{move, snapshot};
  const std::array names{std::string_view{"MoveIntent"},
                         std::string_view{"StateSnapshot"}};
  for (std::size_t index = 0; index < headers.size(); ++index) {
    const auto expected = goldenDatagram(*contract, names[index]);
    const auto encoded =
        RudpMovementCodec::encode(headers[index], messages[index]);
    const auto decoded = expected.has_value()
                             ? RudpMovementCodec::decode(*expected)
                             : lol::transport::rudp::DecodedRudpMovement{};
    if (!expected.has_value() || !encoded.has_value() ||
        *encoded != *expected ||
        decoded.error != RudpMovementCodecError::None ||
        decoded.header != std::optional{headers[index]} ||
        decoded.message != std::optional{messages[index]}) {
      return false;
    }
  }

  RudpStateSnapshot tenPlayers{
      .battleInstanceId = 7,
      .snapshotSequence = 1,
      .serverTick = 2,
      .players = {},
  };
  for (std::uint64_t session = 1; session <= 10; ++session) {
    tenPlayers.players.push_back(RudpSnapshotPlayer{
        .sessionId = session, .posXMillimeter = 0, .posYMillimeter = 0});
  }
  const auto maximum =
      RudpMovementCodec::encode(RudpHeader{.flag = RudpFlag::Unreliable,
                                           .sessionId = 1,
                                           .sessionGeneration = 1,
                                           .transportEpoch = 1,
                                           .sequence = 1,
                                           .ack = 0,
                                           .ackBits = 0,
                                           .messageId = 26},
                                RudpMovementMessage{tenPlayers});
  const auto stateGolden = goldenDatagram(*contract, "StateSnapshot");
  if (!stateGolden.has_value()) {
    return false;
  }
  const auto decodedState =
      lol::transport::rudp::RudpHeaderCodec::decode(*stateGolden);
  std::vector<std::byte> malformedPayload(decodedState.payload.begin(),
                                          decodedState.payload.end());
  if (malformedPayload.size() == 50) {
    malformedPayload[17] = std::byte{3};
  }
  const auto malformedDatagram =
      malformedPayload.size() == 50
          ? lol::transport::rudp::RudpHeaderCodec::encode(snapshotHeader,
                                                          malformedPayload)
          : std::nullopt;
  tenPlayers.players.push_back(RudpSnapshotPlayer{
      .sessionId = 11, .posXMillimeter = 0, .posYMillimeter = 0});
  return maximum.has_value() && maximum->size() == 226 &&
         !RudpMovementCodec::encode(
              moveHeader,
              RudpMovementMessage{RudpMoveIntent{.battleInstanceId = 7,
                                                 .actionSequence = 10,
                                                 .desiredX = 1,
                                                 .desiredY = 0,
                                                 .inputFlags = 1}})
              .has_value() &&
         !RudpMovementCodec::encode(snapshotHeader,
                                    RudpMovementMessage{tenPlayers})
              .has_value() &&
         malformedDatagram.has_value() &&
         RudpMovementCodec::decode(*malformedDatagram).error ==
             RudpMovementCodecError::MalformedPayload;
}

AccountId account(std::uint8_t suffix) {
  AccountId::Bytes bytes{};
  bytes.back() = suffix;
  return AccountId{bytes};
}

AuthenticatedRoomSession session(std::uint64_t id) {
  return {.accountId = account(static_cast<std::uint8_t>(id)),
          .sessionId = SessionId{id},
          .generation = SessionGeneration{1},
          .nickname = "player-" + std::to_string(id)};
}

RudpEndpoint endpoint(std::uint8_t suffix) {
  RudpEndpoint value{.address = {},
                     .port = static_cast<std::uint16_t>(4000 + suffix),
                     .scopeId = 0};
  value.address.back() = static_cast<std::byte>(suffix);
  return value;
}

std::optional<std::uint32_t> bind(RudpBindingRegistry &bindings,
                                  std::uint64_t sessionId) {
  const auto capability = bindings.requestCapability(sessionId, 1, kStart);
  if (!capability.has_value()) {
    return std::nullopt;
  }
  const auto result =
      bindings.bind(RudpHeader{.flag = RudpFlag::Reliable,
                               .sessionId = sessionId,
                               .sessionGeneration = 1,
                               .transportEpoch = 0,
                               .sequence = 1,
                               .ack = 0,
                               .ackBits = 0,
                               .messageId = 22},
                    RudpBindHello{*capability},
                    endpoint(static_cast<std::uint8_t>(sessionId)), kStart);
  return result.status == RudpBindStatus::Accepted
             ? std::optional{result.transportEpoch}
             : std::nullopt;
}

class IntentCollector final {
public:
  void add(LobbyRoomOutboundIntent intent) {
    std::lock_guard lock{mutex_};
    intents_.push_back(std::move(intent));
  }

  std::size_t gameplayStarts() const {
    std::lock_guard lock{mutex_};
    return static_cast<std::size_t>(
        std::count_if(intents_.begin(), intents_.end(), [](const auto &intent) {
          return std::holds_alternative<ArenaGameplayStart>(intent.message);
        }));
  }

private:
  mutable std::mutex mutex_;
  std::vector<LobbyRoomOutboundIntent> intents_;
};

class SnapshotCollector final {
public:
  void add(StateSnapshotProjection snapshot) {
    std::lock_guard lock{mutex_};
    snapshots_.push_back(std::move(snapshot));
  }

  std::vector<StateSnapshotProjection> copy() const {
    std::lock_guard lock{mutex_};
    return snapshots_;
  }

private:
  mutable std::mutex mutex_;
  std::vector<StateSnapshotProjection> snapshots_;
};

bool createCommittedRoom(RoomCommandGateway &gateway, WorkerPool &workers) {
  const auto host = session(1);
  const auto member = session(2);
  return gateway.submit(RoomCommandEnvelope{
             .session = host,
             .command = CreateRoomRequest{.requestId = RequestId{1},
                                          .title = "room",
                                          .capacity = 2}}) ==
             RoomSubmitResult::Accepted &&
         gateway.submit(RoomCommandEnvelope{
             .session = member,
             .command = JoinRoomRequest{.requestId = RequestId{2},
                                        .roomId = RoomId{1}}}) ==
             RoomSubmitResult::Accepted &&
         gateway.submit(RoomCommandEnvelope{
             .session = host,
             .command =
                 SetReadyRequest{.requestId = RequestId{3}, .ready = true}}) ==
             RoomSubmitResult::Accepted &&
         gateway.submit(RoomCommandEnvelope{
             .session = member,
             .command =
                 SetReadyRequest{.requestId = RequestId{4}, .ready = true}}) ==
             RoomSubmitResult::Accepted &&
         workers.waitUntilIdle(2s) &&
         gateway.submit(RoomCommandEnvelope{
             .session = host,
             .command = HostStartRequest{.requestId = RequestId{5}}}) ==
             RoomSubmitResult::Accepted &&
         workers.waitUntilIdle(2s) &&
         gateway.submit(
             RoomCommandEnvelope{.session = host,
                                 .command =
                                     lol::game_flow::ArenaLoadCompleteRequest{
                                         .requestId = RequestId{6},
                                         .roomId = RoomId{1},
                                         .battleId = BattleInstanceId{1}}}) ==
             RoomSubmitResult::Accepted &&
         gateway.submit(
             RoomCommandEnvelope{.session = member,
                                 .command =
                                     lol::game_flow::ArenaLoadCompleteRequest{
                                         .requestId = RequestId{7},
                                         .roomId = RoomId{1},
                                         .battleId = BattleInstanceId{1}}}) ==
             RoomSubmitResult::Accepted &&
         workers.waitUntilIdle(2s);
}

std::optional<std::vector<std::byte>>
moveDatagram(std::uint64_t sessionId, std::uint32_t epoch,
             std::uint32_t transportSeq, std::uint32_t actionSeq,
             std::int16_t x, std::int16_t y) {
  return RudpMovementCodec::encode(
      RudpHeader{.flag = RudpFlag::Unreliable,
                 .sessionId = sessionId,
                 .sessionGeneration = 1,
                 .transportEpoch = epoch,
                 .sequence = transportSeq,
                 .ack = 1,
                 .ackBits = 0,
                 .messageId = 25},
      RudpMovementMessage{RudpMoveIntent{.battleInstanceId = 1,
                                         .actionSequence = actionSeq,
                                         .desiredX = x,
                                         .desiredY = y,
                                         .inputFlags = 0}});
}

bool realBindingDrivesGameplayAndLatestSnapshots() {
  RudpBindingRegistry bindings;
  RudpGameplayReadiness readiness{bindings};
  if (readiness.isReady(SessionId{1}, SessionGeneration{1})) {
    return false;
  }
  const auto hostEpoch = bind(bindings, 1);
  const auto memberEpoch = bind(bindings, 2);
  if (!hostEpoch.has_value() || !memberEpoch.has_value() ||
      !readiness.isReady(SessionId{1}, SessionGeneration{1}) ||
      !readiness.isReady(SessionId{2}, SessionGeneration{1})) {
    return false;
  }

  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 64}};
  IntentCollector intents;
  SnapshotCollector snapshots;
  RoomCommandGateway gateway{workers, readiness,
                             [&intents](LobbyRoomOutboundIntent intent) {
                               intents.add(std::move(intent));
                             },
                             [&snapshots](StateSnapshotProjection snapshot) {
                               snapshots.add(std::move(snapshot));
                             }};
  RudpMovementFlow flow{bindings, gateway};
  if (!createCommittedRoom(gateway, workers) || intents.gameplayStarts() != 1) {
    return false;
  }

  const auto firstMove = moveDatagram(1, *hostEpoch, 2, 1, 32767, 0);
  if (!firstMove.has_value() ||
      flow.submitMove(*firstMove, endpoint(2), kStart) !=
          RudpMovementSubmitResult::PeerRejected ||
      flow.submitMove(*firstMove, endpoint(1), kStart) !=
          RudpMovementSubmitResult::Accepted ||
      flow.submitMove(*firstMove, endpoint(1), kStart) !=
          RudpMovementSubmitResult::StaleTransport ||
      gateway.submitMovementTick(RoomId{1}, BattleInstanceId{1}, 1) !=
          RoomSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s) || !snapshots.copy().empty() ||
      gateway.submitMovementTick(RoomId{1}, BattleInstanceId{1}, 2) !=
          RoomSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }
  auto captured = snapshots.copy();
  if (captured.size() != 1 || captured[0].snapshotSequence != 1 ||
      captured[0].serverTick != 2) {
    return false;
  }
  const auto firstBatch = flow.encodeSnapshot(captured[0]);

  const auto secondMove = moveDatagram(1, *hostEpoch, 3, 2, 0, 32767);
  if (!firstBatch.has_value() || firstBatch->size() != 2 ||
      !secondMove.has_value() ||
      flow.submitMove(*secondMove, endpoint(1), kStart + 40ms) !=
          RudpMovementSubmitResult::Accepted ||
      gateway.submitMovementTick(RoomId{1}, BattleInstanceId{1}, 3) !=
          RoomSubmitResult::Accepted ||
      gateway.submitMovementTick(RoomId{1}, BattleInstanceId{1}, 4) !=
          RoomSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }
  captured = snapshots.copy();
  if (captured.size() != 2 || captured[1].snapshotSequence != 2 ||
      captured[1].serverTick != 4 || captured[1].players.size() != 2 ||
      captured[1].players[0].posXMillimeter != 500 ||
      captured[1].players[0].posYMillimeter != 500) {
    return false;
  }
  const auto latestBatch = flow.encodeSnapshot(captured[1]);
  if (!latestBatch.has_value() || latestBatch->size() != 2) {
    return false;
  }
  for (const EncodedRudpDatagram &outbound : *latestBatch) {
    const auto decoded = RudpMovementCodec::decode(outbound.datagram);
    const auto *snapshot =
        decoded.message.has_value()
            ? std::get_if<RudpStateSnapshot>(&*decoded.message)
            : nullptr;
    if (decoded.error != RudpMovementCodecError::None || snapshot == nullptr ||
        snapshot->snapshotSequence != 2 || snapshot->serverTick != 4) {
      return false;
    }
  }
  return true;
}

} // namespace

int main() {
  return codecMatchesGoldenAndTenPlayerCeiling() &&
                 realBindingDrivesGameplayAndLatestSnapshots()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
