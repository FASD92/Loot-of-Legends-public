#include "BattleLoadFlow.hpp"

#include <lol/game_flow/GameplayTransportReadinessPort.hpp>
#include <lol/game_flow/RoomCommandGateway.hpp>
#include <lol/runtime/WorkerPool.hpp>
#include <lol/transport/tcp/BattleLoadProtocol.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using lol::app::BattleLoadFlow;
using lol::battle::BattleLoadResultCode;
using lol::game_flow::ArenaGameplayStart;
using lol::game_flow::ArenaLoadEntry;
using lol::game_flow::AuthenticatedRoomSession;
using lol::game_flow::BattleCommandResponse;
using lol::game_flow::CreateRoomRequest;
using lol::game_flow::GameplayTransportReadinessPort;
using lol::game_flow::JoinRoomRequest;
using lol::game_flow::LobbyRoomOutboundIntent;
using lol::game_flow::RoomCommandEnvelope;
using lol::game_flow::RoomCommandGateway;
using lol::game_flow::RoomSubmitResult;
using lol::game_flow::SetReadyRequest;
using lol::runtime::WorkerPool;
using lol::runtime::WorkerPoolConfig;
using lol::shared::AccountId;
using lol::shared::RequestId;
using lol::shared::RoomId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;
using lol::transport::tcp::BattleLoadClientMessage;
using lol::transport::tcp::BattleLoadCodecError;
using lol::transport::tcp::BattleLoadProtocolCodec;
using lol::transport::tcp::BattleLoadServerMessage;
using lol::transport::tcp::BattleParticipant;
using WireArenaGameplayStart = lol::transport::tcp::ArenaGameplayStart;
using WireArenaLoadCancelled = lol::transport::tcp::ArenaLoadCancelled;
using WireArenaLoadComplete = lol::transport::tcp::ArenaLoadComplete;
using WireArenaLoadEntry = lol::transport::tcp::ArenaLoadEntry;
using WireBattleCommandResponse = lol::transport::tcp::BattleCommandResponse;
using WireHostStartRequest = lol::transport::tcp::HostStartRequest;

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

std::optional<std::string> readGoldenContract() {
  std::ifstream input{LOOT_BATTLE_LOAD_GOLDEN_PATH};
  if (!input) {
    return std::nullopt;
  }
  return std::string{std::istreambuf_iterator<char>{input},
                     std::istreambuf_iterator<char>{}};
}

std::optional<std::vector<std::byte>>
goldenFrame(std::string_view contract, std::string_view semanticName) {
  const std::string marker =
      "\"semanticName\": \"" + std::string{semanticName} + "\"";
  const auto message = contract.find(marker);
  if (message == std::string_view::npos) {
    return std::nullopt;
  }
  constexpr std::string_view frameMarker = "\"frameHex\": \"";
  const auto frame = contract.find(frameMarker, message);
  if (frame == std::string_view::npos) {
    return std::nullopt;
  }
  const auto first = frame + frameMarker.size();
  const auto last = contract.find('"', first);
  return last == std::string_view::npos
             ? std::nullopt
             : std::optional{fromHex(contract.substr(first, last - first))};
}

bool codecMatchesFrozenGolden() {
  const auto contract = readGoldenContract();
  if (!contract.has_value()) {
    return false;
  }
  const std::vector<std::pair<std::string_view, BattleLoadClientMessage>>
      clients{
          {"HostStartRequest", WireHostStartRequest{.requestId = 1}},
          {"ArenaLoadComplete", WireArenaLoadComplete{.requestId = 1,
                                                      .roomId = 7,
                                                      .battleInstanceId = 1}},
      };
  for (const auto &[name, message] : clients) {
    const auto expected = goldenFrame(*contract, name);
    const auto encoded = BattleLoadProtocolCodec::encodeClientFrame(message);
    if (!expected.has_value() || !encoded.has_value() ||
        *encoded != *expected) {
      return false;
    }
    const auto decoded = BattleLoadProtocolCodec::decodeClientFrame(*encoded);
    if (decoded.error != BattleLoadCodecError::None ||
        !decoded.message.has_value() || *decoded.message != message) {
      return false;
    }
  }

  const std::vector<std::pair<std::string_view, BattleLoadServerMessage>>
      servers{
          {"BattleCommandResponse",
           WireBattleCommandResponse{.requestId = 1, .resultCode = 0}},
          {"ArenaLoadEntry",
           WireArenaLoadEntry{.roomId = 7, .battleInstanceId = 1}},
          {"ArenaGameplayStart",
           WireArenaGameplayStart{
               .roomId = 7,
               .battleInstanceId = 1,
               .participants = {BattleParticipant{.sessionId = 1,
                                                  .sessionGeneration = 2,
                                                  .nickname = "neo"},
                                BattleParticipant{.sessionId = 3,
                                                  .sessionGeneration = 4,
                                                  .nickname = "trinity"}},
           }},
          {"ArenaLoadCancelled", WireArenaLoadCancelled{.roomId = 7,
                                                        .battleInstanceId = 1,
                                                        .reasonCode = 1}},
      };
  for (const auto &[name, message] : servers) {
    const auto expected = goldenFrame(*contract, name);
    const auto encoded = BattleLoadProtocolCodec::encodeServerFrame(message);
    if (!expected.has_value() || !encoded.has_value() ||
        *encoded != *expected) {
      return false;
    }
  }
  return true;
}

bool malformedPayloadsAreRejected() {
  const auto valid =
      BattleLoadProtocolCodec::encodeClientFrame(WireArenaLoadComplete{
          .requestId = 1, .roomId = 7, .battleInstanceId = 1});
  if (!valid.has_value()) {
    return false;
  }
  auto trailing = *valid;
  trailing.push_back(std::byte{0});
  auto wrongVersion = *valid;
  wrongVersion[4] = std::byte{2};
  const auto serverFrame = BattleLoadProtocolCodec::encodeServerFrame(
      WireArenaLoadEntry{.roomId = 7, .battleInstanceId = 1});
  return serverFrame.has_value() &&
         BattleLoadProtocolCodec::decodeClientFrame({}).error ==
             BattleLoadCodecError::PartialFrame &&
         BattleLoadProtocolCodec::decodeClientFrame(trailing).error ==
             BattleLoadCodecError::FrameLengthMismatch &&
         BattleLoadProtocolCodec::decodeClientFrame(wrongVersion).error ==
             BattleLoadCodecError::UnsupportedVersion &&
         BattleLoadProtocolCodec::decodeClientFrame(*serverFrame).error ==
             BattleLoadCodecError::WrongDirection &&
         !BattleLoadProtocolCodec::encodeClientFrame(
              WireHostStartRequest{.requestId = 0})
              .has_value() &&
         !BattleLoadProtocolCodec::encodeServerFrame(
              WireBattleCommandResponse{.requestId = 1, .resultCode = 13})
              .has_value() &&
         !BattleLoadProtocolCodec::encodeServerFrame(WireArenaGameplayStart{
                                                         .roomId = 7,
                                                         .battleInstanceId = 1,
                                                         .participants = {},
                                                     })
              .has_value();
}

AccountId account(std::uint8_t suffix) {
  AccountId::Bytes bytes{};
  bytes.back() = suffix;
  return AccountId{bytes};
}

AuthenticatedRoomSession session(std::uint64_t id) {
  return AuthenticatedRoomSession{
      .accountId = account(static_cast<std::uint8_t>(id)),
      .sessionId = SessionId{id},
      .generation = SessionGeneration{1},
      .nickname = "player-" + std::to_string(id),
  };
}

class IntentCollector final {
public:
  void add(LobbyRoomOutboundIntent intent) {
    std::lock_guard lock{mutex_};
    intents_.push_back(std::move(intent));
  }

  std::vector<LobbyRoomOutboundIntent> take() {
    std::lock_guard lock{mutex_};
    return std::exchange(intents_, {});
  }

private:
  std::mutex mutex_;
  std::vector<LobbyRoomOutboundIntent> intents_;
};

class AlwaysReadyGameplayTransport final
    : public GameplayTransportReadinessPort {
public:
  [[nodiscard]] bool isReady(SessionId,
                             SessionGeneration) const noexcept override {
    return true;
  }
};

bool createReadyRoom(RoomCommandGateway &gateway,
                     const AuthenticatedRoomSession &host,
                     const AuthenticatedRoomSession &member) {
  return gateway.submit(RoomCommandEnvelope{
             .session = host,
             .command = CreateRoomRequest{.requestId = RequestId{1},
                                          .title = "room",
                                          .capacity = 2},
         }) == RoomSubmitResult::Accepted &&
         gateway.submit(RoomCommandEnvelope{
             .session = member,
             .command = JoinRoomRequest{.requestId = RequestId{2},
                                        .roomId = RoomId{1}},
         }) == RoomSubmitResult::Accepted &&
         gateway.submit(RoomCommandEnvelope{
             .session = host,
             .command =
                 SetReadyRequest{.requestId = RequestId{3}, .ready = true},
         }) == RoomSubmitResult::Accepted &&
         gateway.submit(RoomCommandEnvelope{
             .session = member,
             .command =
                 SetReadyRequest{.requestId = RequestId{4}, .ready = true},
         }) == RoomSubmitResult::Accepted;
}

bool exerciseLoadFlow(RoomCommandGateway &gateway, WorkerPool &workers,
                      IntentCollector &collector,
                      std::size_t expectedGameplayStarts) {
  BattleLoadFlow flow{gateway};
  const auto host = session(1);
  const auto member = session(2);
  if (!createReadyRoom(gateway, host, member) || !workers.waitUntilIdle(2s)) {
    return false;
  }
  static_cast<void>(collector.take());

  const auto hostStart = BattleLoadProtocolCodec::encodeClientFrame(
      WireHostStartRequest{.requestId = 10});
  const auto hostComplete =
      BattleLoadProtocolCodec::encodeClientFrame(WireArenaLoadComplete{
          .requestId = 11, .roomId = 1, .battleInstanceId = 1});
  const auto memberComplete =
      BattleLoadProtocolCodec::encodeClientFrame(WireArenaLoadComplete{
          .requestId = 12, .roomId = 1, .battleInstanceId = 1});
  const auto hostCompleteDuplicate =
      BattleLoadProtocolCodec::encodeClientFrame(WireArenaLoadComplete{
          .requestId = 13, .roomId = 1, .battleInstanceId = 1});
  if (!hostStart.has_value() || !hostComplete.has_value() ||
      !memberComplete.has_value() || !hostCompleteDuplicate.has_value() ||
      flow.submit(host, *hostStart).submitResult !=
          RoomSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s) ||
      flow.submit(host, *hostComplete).submitResult !=
          RoomSubmitResult::Accepted ||
      flow.submit(member, *memberComplete).submitResult !=
          RoomSubmitResult::Accepted ||
      flow.submit(host, *hostCompleteDuplicate).submitResult !=
          RoomSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s)) {
    return false;
  }

  const auto intents = collector.take();
  std::vector<std::uint64_t> responseIds;
  std::size_t entries = 0;
  std::size_t gameplayStarts = 0;
  for (const auto &intent : intents) {
    if (const auto *response =
            std::get_if<BattleCommandResponse>(&intent.message)) {
      if (response->result != BattleLoadResultCode::Ok ||
          !BattleLoadFlow::encode(intent).has_value()) {
        return false;
      }
      responseIds.push_back(response->requestId.value());
    } else if (std::holds_alternative<ArenaLoadEntry>(intent.message)) {
      ++entries;
      if (!BattleLoadFlow::encode(intent).has_value()) {
        return false;
      }
    } else if (std::holds_alternative<ArenaGameplayStart>(intent.message)) {
      ++gameplayStarts;
      const auto &start = std::get<ArenaGameplayStart>(intent.message);
      if (start.roomId != RoomId{1} || start.battleId.value() != 1 ||
          start.participants.size() != 2 ||
          start.participants[0].sessionId != SessionId{1} ||
          start.participants[1].sessionId != SessionId{2} ||
          !BattleLoadFlow::encode(intent).has_value()) {
        return false;
      }
    }
  }
  std::ranges::sort(responseIds);
  return responseIds == std::vector<std::uint64_t>{10, 11, 12, 13} &&
         entries == 1 && gameplayStarts == expectedGameplayStarts;
}

bool releaseFlowBlocksGameplayVisibilityWithoutAdapter() {
  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 32}};
  IntentCollector collector;
  RoomCommandGateway gateway{workers,
                             [&collector](LobbyRoomOutboundIntent intent) {
                               collector.add(std::move(intent));
                             }};
  return exerciseLoadFlow(gateway, workers, collector, 0);
}

bool readyTestAdapterEnablesGameplayVisibility() {
  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 32}};
  IntentCollector collector;
  AlwaysReadyGameplayTransport readiness;
  RoomCommandGateway gateway{workers, readiness,
                             [&collector](LobbyRoomOutboundIntent intent) {
                               collector.add(std::move(intent));
                             }};
  return exerciseLoadFlow(gateway, workers, collector, 1);
}

} // namespace

int main() {
  if (!codecMatchesFrozenGolden() || !malformedPayloadsAreRejected() ||
      !releaseFlowBlocksGameplayVisibilityWithoutAdapter() ||
      !readyTestAdapterEnablesGameplayVisibility()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
