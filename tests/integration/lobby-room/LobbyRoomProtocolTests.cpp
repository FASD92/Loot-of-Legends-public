#include "LobbyRoomFlow.hpp"

#include <lol/game_flow/RoomCommandGateway.hpp>
#include <lol/lobby_room/RoomProjections.hpp>
#include <lol/runtime/WorkerPool.hpp>
#include <lol/transport/tcp/LobbyRoomProtocol.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
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
using lol::app::LobbyRoomFlow;
using lol::battle::BattleFinalResult;
using lol::battle::BattleOutcome;
using lol::battle::BattleResultEntry;
using lol::battle::ParticipantExitStatus;
using lol::game_flow::AuthenticatedRoomSession;
using lol::game_flow::LobbyRoomOutboundIntent;
using lol::game_flow::RoomAudience;
using lol::game_flow::RoomCommandGateway;
using lol::game_flow::RoomSubmitResult;
using lol::game_flow::SessionAudience;
using lol::runtime::WorkerPool;
using lol::runtime::WorkerPoolConfig;
using lol::shared::AccountId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;
using lol::transport::tcp::LobbyRoomCodecError;
using lol::transport::tcp::LobbyRoomProtocolCodec;

AccountId account(std::uint8_t suffix) {
  AccountId::Bytes bytes{};
  bytes.back() = suffix;
  return AccountId{bytes};
}

using FlowLobbyEntry = lol::game_flow::LobbyEntrySnapshot;
using FlowLobbyRoomList = lol::game_flow::LobbyRoomListUpdate;
using FlowRoomCommandResponse = lol::game_flow::RoomCommandResponse;
using FlowRoomDetail = lol::lobby_room::RoomDetailProjection;

using WireClientMessage = lol::transport::tcp::LobbyRoomClientMessage;
using WireCreateRoom = lol::transport::tcp::CreateRoom;
using WireJoinRoom = lol::transport::tcp::JoinRoom;
using WireKickRoomMember = lol::transport::tcp::KickRoomMember;
using WireLeaveRoom = lol::transport::tcp::LeaveRoom;
using WireLobbyEntry = lol::transport::tcp::LobbyEntrySnapshot;
using WireLobbyRoomList = lol::transport::tcp::LobbyRoomListUpdate;
using WireRoomCommandResponse = lol::transport::tcp::RoomCommandResponse;
using WireRoomDetail = lol::transport::tcp::RoomDetailProjection;
using WireRoomMember = lol::transport::tcp::RoomMember;
using WireRoomSummary = lol::transport::tcp::RoomSummary;
using WireServerMessage = lol::transport::tcp::LobbyRoomServerMessage;
using WireSetReady = lol::transport::tcp::SetReady;

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
  std::ifstream input{LOOT_LOBBY_ROOM_GOLDEN_PATH};
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
  if (last == std::string_view::npos) {
    return std::nullopt;
  }
  return fromHex(contract.substr(first, last - first));
}

WireClientMessage createRequest() {
  return WireCreateRoom{
      .requestId = 1,
      .title = "room",
      .capacity = 2,
  };
}

WireRoomSummary summary() {
  return WireRoomSummary{
      .roomId = 7,
      .title = "room",
      .memberCount = 1,
      .capacity = 2,
  };
}

WireRoomDetail detail() {
  return WireRoomDetail{
      .roomId = 7,
      .title = "room",
      .capacity = 2,
      .hostSessionId = 1,
      .hostSessionGeneration = 2,
      .members = {WireRoomMember{
          .sessionId = 1,
          .sessionGeneration = 2,
          .nickname = "neo",
          .ready = false,
      }},
  };
}

bool codecMatchesFrozenGolden() {
  const auto contract = readGoldenContract();
  if (!contract.has_value()) {
    return false;
  }
  const std::vector<std::pair<std::string_view, WireClientMessage>>
      clientMessages{
          {"CreateRoom", createRequest()},
          {"JoinRoom", WireJoinRoom{.requestId = 1, .roomId = 7}},
          {"LeaveRoom", WireLeaveRoom{.requestId = 1}},
          {"SetReady", WireSetReady{.requestId = 1, .ready = true}},
          {"KickRoomMember",
           WireKickRoomMember{
               .requestId = 1,
               .targetSessionId = 2,
               .targetSessionGeneration = 3,
           }},
      };
  for (const auto &[name, message] : clientMessages) {
    const auto expected = goldenFrame(*contract, name);
    const auto encoded = LobbyRoomProtocolCodec::encodeClientFrame(message);
    if (!expected.has_value() || !encoded.has_value() ||
        *encoded != *expected) {
      return false;
    }
    const auto decoded = LobbyRoomProtocolCodec::decodeClientFrame(*encoded);
    if (decoded.error != LobbyRoomCodecError::None ||
        !decoded.message.has_value() || *decoded.message != message) {
      return false;
    }
  }

  const std::vector<std::pair<std::string_view, WireServerMessage>>
      serverMessages{
          {"LobbyEntrySnapshot",
           WireLobbyEntry{
               .sessionId = 1,
               .sessionGeneration = 2,
               .nickname = "neo",
               .rooms = {summary()},
           }},
          {"LobbyRoomListUpdate", WireLobbyRoomList{.rooms = {summary()}}},
          {"RoomCommandResponse",
           WireRoomCommandResponse{
               .requestId = 1,
               .resultCode = 0,
           }},
          {"RoomDetailProjection", detail()},
      };
  for (const auto &[name, message] : serverMessages) {
    const auto expected = goldenFrame(*contract, name);
    const auto encoded = LobbyRoomProtocolCodec::encodeServerFrame(message);
    if (!expected.has_value() || !encoded.has_value() ||
        *encoded != *expected) {
      return false;
    }
  }

  const auto hostStart = goldenFrame(*contract, "HostStartRequest");
  if (!hostStart.has_value() ||
      LobbyRoomProtocolCodec::decodeClientFrame(*hostStart).error !=
          LobbyRoomCodecError::ReservedMessage) {
    return false;
  }
  return true;
}

bool malformedPayloadsAreRejected() {
  const auto encoded =
      LobbyRoomProtocolCodec::encodeClientFrame(createRequest());
  if (!encoded.has_value()) {
    return false;
  }
  auto invalidTitle = *encoded;
  invalidTitle.at(18) = std::byte{0};
  auto trailing = *encoded;
  trailing.push_back(std::byte{0});
  const auto ready = LobbyRoomProtocolCodec::encodeClientFrame(WireSetReady{
      .requestId = 1,
      .ready = true,
  });
  if (!ready.has_value()) {
    return false;
  }
  auto invalidReady = *ready;
  invalidReady.back() = std::byte{2};
  const auto trimmed = LobbyRoomProtocolCodec::encodeClientFrame(
      WireCreateRoom{.requestId = 2, .title = " room ", .capacity = 2});
  const auto decodedTrimmed =
      trimmed.has_value() ? LobbyRoomProtocolCodec::decodeClientFrame(*trimmed)
                          : lol::transport::tcp::DecodedLobbyRoomFrame{
                                .error = LobbyRoomCodecError::MalformedPayload,
                                .message = std::nullopt,
                            };
  const auto *trimmedCreate =
      decodedTrimmed.message.has_value()
          ? std::get_if<WireCreateRoom>(&*decodedTrimmed.message)
          : nullptr;

  return trimmedCreate != nullptr && trimmedCreate->title == "room" &&
         LobbyRoomProtocolCodec::decodeClientFrame(invalidTitle).error ==
             LobbyRoomCodecError::MalformedPayload &&
         LobbyRoomProtocolCodec::decodeClientFrame(trailing).error ==
             LobbyRoomCodecError::FrameLengthMismatch &&
         LobbyRoomProtocolCodec::decodeClientFrame(invalidReady).error ==
             LobbyRoomCodecError::MalformedPayload &&
         !LobbyRoomProtocolCodec::encodeClientFrame(WireCreateRoom{
                                                        .requestId = 1,
                                                        .title = "   ",
                                                        .capacity = 2,
                                                    })
              .has_value() &&
         !LobbyRoomProtocolCodec::encodeServerFrame(
              WireLobbyRoomList{
                  .rooms = {WireRoomSummary{
                      .roomId = 7,
                      .title = "room",
                      .memberCount = 3,
                      .capacity = 2,
                  }},
              })
              .has_value();
}

class IntentCollector final {
public:
  void add(LobbyRoomOutboundIntent intent) {
    std::lock_guard lock{mutex_};
    intents_.push_back(std::move(intent));
    changed_.notify_all();
  }

  bool waitFor(std::size_t count) {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(
        lock, 2s, [this, count] { return intents_.size() >= count; });
  }

  std::vector<LobbyRoomOutboundIntent> take() {
    std::lock_guard lock{mutex_};
    return std::exchange(intents_, {});
  }

private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<LobbyRoomOutboundIntent> intents_;
};

bool decodedCommandsReachGatewayAndOutboundCodec() {
  WorkerPool workers{WorkerPoolConfig{.threadCount = 2, .queueCapacity = 32}};
  IntentCollector collector;
  RoomCommandGateway gateway{workers,
                             [&collector](LobbyRoomOutboundIntent intent) {
                               collector.add(std::move(intent));
                             }};
  LobbyRoomFlow flow{gateway};
  const AuthenticatedRoomSession host{
      .accountId = account(1),
      .sessionId = SessionId{1},
      .generation = SessionGeneration{2},
      .nickname = "neo",
  };
  if (!gateway.enterLobby(host)) {
    return false;
  }

  const auto createFrame =
      LobbyRoomProtocolCodec::encodeClientFrame(createRequest());
  if (!createFrame.has_value()) {
    return false;
  }
  const auto createDispatch = flow.submit(host, *createFrame);
  if (createDispatch.codecError != LobbyRoomCodecError::None ||
      createDispatch.submitResult != RoomSubmitResult::Accepted) {
    return false;
  }

  const AuthenticatedRoomSession member{
      .accountId = account(2),
      .sessionId = SessionId{2},
      .generation = SessionGeneration{3},
      .nickname = "trinity",
  };
  const auto joinFrame = LobbyRoomProtocolCodec::encodeClientFrame(
      WireJoinRoom{.requestId = 2, .roomId = 1});
  if (!joinFrame.has_value()) {
    return false;
  }
  const auto joinDispatch = flow.submit(member, *joinFrame);
  if (joinDispatch.codecError != LobbyRoomCodecError::None ||
      joinDispatch.submitResult != RoomSubmitResult::Accepted ||
      !workers.waitUntilIdle(2s) || !collector.waitFor(7)) {
    return false;
  }
  const auto reserved =
      flow.submit(host, fromHex("0000000d010000000e0000000000000001"));
  if (reserved.codecError != LobbyRoomCodecError::ReservedMessage ||
      reserved.submitResult.has_value()) {
    return false;
  }

  const auto intents = collector.take();
  std::size_t responses = 0;
  std::size_t details = 0;
  std::size_t roomLists = 0;
  std::size_t lobbyEntries = 0;
  for (const auto &intent : intents) {
    const auto encoded = LobbyRoomFlow::encode(intent);
    if (!encoded.has_value() || encoded->audience != intent.audience) {
      return false;
    }
    responses += std::holds_alternative<FlowRoomCommandResponse>(intent.message)
                     ? 1U
                     : 0U;
    details += std::holds_alternative<FlowRoomDetail>(intent.message) ? 1U : 0U;
    roomLists +=
        std::holds_alternative<FlowLobbyRoomList>(intent.message) ? 1U : 0U;
    lobbyEntries +=
        std::holds_alternative<FlowLobbyEntry>(intent.message) ? 1U : 0U;
  }
  if (intents.size() != 7 || responses != 2 || details != 2 || roomLists != 2 ||
      lobbyEntries != 1) {
    return false;
  }
  return std::holds_alternative<SessionAudience>(intents.front().audience) &&
         std::ranges::any_of(intents, [](const auto &intent) {
           return std::holds_alternative<RoomAudience>(intent.audience) &&
                  std::holds_alternative<FlowRoomDetail>(intent.message);
         });
}

bool finalResultUsesDedicatedTcpCodec() {
  std::ifstream input{LOOT_FINAL_RESULT_GOLDEN_PATH};
  const std::string document{std::istreambuf_iterator<char>{input},
                             std::istreambuf_iterator<char>{}};
  const auto expected = goldenFrame(document, "MonsterDefeatedFinalResult");
  const auto encoded = LobbyRoomFlow::encode(LobbyRoomOutboundIntent{
      .audience = RoomAudience{lol::shared::RoomId{7}},
      .message =
          BattleFinalResult{
              .roomId = lol::shared::RoomId{7},
              .battleId = lol::shared::BattleInstanceId{9},
              .outcome = BattleOutcome::MonsterDefeated,
              .entries =
                  {BattleResultEntry{
                       .sessionId = SessionId{1},
                       .nickname = "neo",
                       .exitStatus = ParticipantExitStatus::TerminalPresent,
                       .finalAssetValue = 300,
                       .rank = 1,
                       .isTop = true,
                   },
                   BattleResultEntry{
                       .sessionId = SessionId{2},
                       .nickname = "trinity",
                       .exitStatus = ParticipantExitStatus::TerminalExited,
                       .finalAssetValue = 100,
                       .rank = 2,
                       .isTop = false,
                   }},
          },
  });
  return expected.has_value() && encoded.has_value() &&
         encoded->frame == *expected &&
         std::holds_alternative<RoomAudience>(encoded->audience);
}

} // namespace

int main() {
  if (!codecMatchesFrozenGolden()) {
    return 1;
  }
  if (!malformedPayloadsAreRejected()) {
    return 2;
  }
  if (!decodedCommandsReachGatewayAndOutboundCodec()) {
    return 3;
  }
  if (!finalResultUsesDedicatedTcpCodec()) {
    return 4;
  }
  return EXIT_SUCCESS;
}
