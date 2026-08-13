#include <lol/battle/CombatResultStore.hpp>
#include <lol/transport/rudp/ReliableQueue.hpp>
#include <lol/transport/rudp/RudpCombatCodec.hpp>
#include <lol/transport/rudp/RudpPeer.hpp>

#include <array>
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
#include <utility>
#include <vector>

namespace {

using lol::battle::AttackCommand;
using lol::battle::AttackResultCode;
using lol::battle::AttackResultStore;
using lol::battle::AttackResultStoreDecision;
using lol::battle::AttackTerminalResult;
using lol::battle::CombatOutcome;
using lol::battle::CommandId;
using lol::shared::BattleInstanceId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;
using lol::transport::rudp::AckState;
using lol::transport::rudp::DecodedRudpCombat;
using lol::transport::rudp::isAcknowledged;
using lol::transport::rudp::ReliableLane;
using lol::transport::rudp::ReliableQueue;
using lol::transport::rudp::ReliableQueueAdmission;
using lol::transport::rudp::RudpAttackIntent;
using lol::transport::rudp::RudpAttackResultCode;
using lol::transport::rudp::RudpAttackTerminalResult;
using lol::transport::rudp::RudpCombatCodec;
using lol::transport::rudp::RudpCombatCodecError;
using lol::transport::rudp::RudpCombatMessage;
using lol::transport::rudp::RudpCombatOutcome;
using lol::transport::rudp::RudpCombatTerminalEvent;
using lol::transport::rudp::RudpCommandId;
using lol::transport::rudp::RudpEventId;
using lol::transport::rudp::RudpEventStreamKind;
using lol::transport::rudp::RudpFlag;
using lol::transport::rudp::RudpHeader;
using lol::transport::rudp::RudpHeaderCodec;
using lol::transport::rudp::RudpMonsterSpawned;
using lol::transport::rudp::RudpMonsterState;
using lol::transport::rudp::RudpMonsterStateSnapshot;

using namespace std::chrono_literals;

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
  std::ifstream input{LOOT_COMBAT_GOLDEN_PATH};
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

RudpHeader header(RudpFlag flag, std::uint32_t sequence, std::uint32_t ack,
                  std::uint32_t ackBits, std::uint16_t messageId) {
  return RudpHeader{.flag = flag,
                    .sessionId = 1,
                    .sessionGeneration = 2,
                    .transportEpoch = 3,
                    .sequence = sequence,
                    .ack = ack,
                    .ackBits = ackBits,
                    .messageId = messageId};
}

bool matchesGoldenVectors() {
  const auto contract = readGolden();
  if (!contract.has_value()) {
    return false;
  }
  const std::array headers{
      header(RudpFlag::Reliable, 4, 3, 3, 27),
      header(RudpFlag::Reliable, 5, 4, 7, 28),
      header(RudpFlag::Reliable, 6, 4, 7, 29),
      header(RudpFlag::Reliable, 7, 4, 7, 30),
      header(RudpFlag::Unreliable, 8, 4, 7, 31),
  };
  const std::array<RudpCombatMessage, 5> messages{
      RudpAttackIntent{
          .commandId = RudpCommandId{.high = 0x0102030405060708ULL,
                                     .low = 0x1112131415161718ULL},
          .battleInstanceId = 7,
          .targetHint = 1,
      },
      RudpAttackTerminalResult{
          .commandId = RudpCommandId{.high = 0x0102030405060708ULL,
                                     .low = 0x1112131415161718ULL},
          .battleInstanceId = 7,
          .resultCode = RudpAttackResultCode::Ok,
          .monsterId = 1,
          .remainingHitPoints = 1580,
          .rulesetVersion = 1,
          .combatOutcome = RudpCombatOutcome::None,
      },
      RudpMonsterSpawned{
          .eventId = RudpEventId{.high = 0x2122232425262728ULL,
                                 .low = 0x3132333435363738ULL},
          .battleInstanceId = 7,
          .eventStreamKind = RudpEventStreamKind::CombatLifecycle,
          .eventSequence = 1,
          .monsterId = 1,
          .posXMillimeter = 0,
          .posYMillimeter = 0,
          .maximumHitPoints = 1600,
          .rulesetVersion = 1,
      },
      RudpCombatTerminalEvent{
          .eventId = RudpEventId{.high = 0x4142434445464748ULL,
                                 .low = 0x5152535455565758ULL},
          .battleInstanceId = 7,
          .eventStreamKind = RudpEventStreamKind::CombatLifecycle,
          .eventSequence = 2,
          .combatOutcome = RudpCombatOutcome::MonsterDefeated,
          .monsterId = 1,
          .serverTick = 600,
          .rulesetVersion = 1,
      },
      RudpMonsterStateSnapshot{
          .battleInstanceId = 7,
          .snapshotSequence = 9,
          .serverTick = 601,
          .monsterId = 1,
          .hitPoints = 0,
          .monsterState = RudpMonsterState::Dead,
      },
  };
  const std::array names{std::string_view{"AttackIntent"},
                         std::string_view{"AttackTerminalResult"},
                         std::string_view{"MonsterSpawned"},
                         std::string_view{"CombatTerminalEvent"},
                         std::string_view{"MonsterStateSnapshot"}};
  for (std::size_t index = 0; index < messages.size(); ++index) {
    const auto expected = goldenDatagram(*contract, names[index]);
    const auto encoded =
        RudpCombatCodec::encode(headers[index], messages[index]);
    const auto decoded = expected.has_value()
                             ? RudpCombatCodec::decode(*expected)
                             : DecodedRudpCombat{};
    if (!expected.has_value() || !encoded.has_value() ||
        *encoded != *expected || encoded->size() > 1200 ||
        decoded.error != RudpCombatCodecError::None ||
        decoded.header != std::optional{headers[index]} ||
        decoded.message != std::optional{messages[index]}) {
      return false;
    }
  }
  return true;
}

bool rejectsWrongReliabilityAndScope() {
  const RudpAttackIntent attack{
      .commandId = RudpCommandId{.high = 1, .low = 2},
      .battleInstanceId = 7,
      .targetHint = 1,
  };
  const RudpMonsterSpawned spawn{
      .eventId = RudpEventId{.high = 3, .low = 4},
      .battleInstanceId = 7,
      .eventStreamKind = RudpEventStreamKind::CombatLifecycle,
      .eventSequence = 1,
      .monsterId = 1,
      .posXMillimeter = 0,
      .posYMillimeter = 0,
      .maximumHitPoints = 1600,
      .rulesetVersion = 1,
  };
  auto wrongStream = spawn;
  wrongStream.eventSequence = 0;
  auto wrongBattle = attack;
  wrongBattle.battleInstanceId = 0;
  return !RudpCombatCodec::encode(header(RudpFlag::Unreliable, 1, 0, 0, 27),
                                  RudpCombatMessage{attack})
              .has_value() &&
         !RudpCombatCodec::encode(header(RudpFlag::Reliable, 1, 0, 0, 31),
                                  RudpCombatMessage{RudpMonsterStateSnapshot{
                                      .battleInstanceId = 7,
                                      .snapshotSequence = 1,
                                      .serverTick = 1,
                                      .monsterId = 1,
                                      .hitPoints = 1600,
                                      .monsterState = RudpMonsterState::Alive,
                                  }})
              .has_value() &&
         !RudpCombatCodec::encode(header(RudpFlag::Reliable, 1, 0, 0, 29),
                                  RudpCombatMessage{wrongStream})
              .has_value() &&
         !RudpCombatCodec::encode(header(RudpFlag::Reliable, 1, 0, 0, 27),
                                  RudpCombatMessage{wrongBattle})
              .has_value();
}

bool rejectsMalformedPayloadAndEnums() {
  const auto malformedDatagram = RudpHeaderCodec::encode(
      header(RudpFlag::Reliable, 1, 0, 0, 27), std::array{std::byte{0}});
  const auto unsupportedDatagram = RudpHeaderCodec::encode(
      header(RudpFlag::Reliable, 1, 0, 0, 32), std::array{std::byte{0}});
  const auto invalidState =
      RudpCombatCodec::encode(header(RudpFlag::Unreliable, 1, 0, 0, 31),
                              RudpCombatMessage{RudpMonsterStateSnapshot{
                                  .battleInstanceId = 7,
                                  .snapshotSequence = 1,
                                  .serverTick = 1,
                                  .monsterId = 1,
                                  .hitPoints = 0,
                                  .monsterState = RudpMonsterState::Alive,
                              }});
  const auto validState =
      RudpCombatCodec::encode(header(RudpFlag::Unreliable, 1, 0, 0, 31),
                              RudpCombatMessage{RudpMonsterStateSnapshot{
                                  .battleInstanceId = 7,
                                  .snapshotSequence = 1,
                                  .serverTick = 1,
                                  .monsterId = 1,
                                  .hitPoints = 1600,
                                  .monsterState = RudpMonsterState::Alive,
                              }});
  if (!validState.has_value()) {
    return false;
  }
  const auto decodedState = RudpHeaderCodec::decode(*validState);
  if (!decodedState.header.has_value() || decodedState.payload.empty()) {
    return false;
  }
  std::vector<std::byte> invalidStatePayload{decodedState.payload.begin(),
                                             decodedState.payload.end()};
  invalidStatePayload.back() = std::byte{4};
  const auto unknownState =
      RudpHeaderCodec::encode(*decodedState.header, invalidStatePayload);
  return malformedDatagram.has_value() && unsupportedDatagram.has_value() &&
         RudpCombatCodec::decode(*malformedDatagram).error ==
             RudpCombatCodecError::MalformedPayload &&
         RudpCombatCodec::decode(*unsupportedDatagram).error ==
             RudpCombatCodecError::UnsupportedMessage &&
         !invalidState.has_value() && unknownState.has_value() &&
         RudpCombatCodec::decode(*unknownState).error ==
             RudpCombatCodecError::MalformedPayload;
}

bool reliableLifecycleStreamKeepsBattleScope() {
  const RudpCombatMessage spawn = RudpMonsterSpawned{
      .eventId = RudpEventId{.high = 1, .low = 1},
      .battleInstanceId = 7,
      .eventStreamKind = RudpEventStreamKind::CombatLifecycle,
      .eventSequence = 1,
      .monsterId = 1,
      .posXMillimeter = 0,
      .posYMillimeter = 0,
      .maximumHitPoints = 1600,
      .rulesetVersion = 1,
  };
  const RudpCombatMessage terminal = RudpCombatTerminalEvent{
      .eventId = RudpEventId{.high = 1, .low = 2},
      .battleInstanceId = 7,
      .eventStreamKind = RudpEventStreamKind::CombatLifecycle,
      .eventSequence = 2,
      .combatOutcome = RudpCombatOutcome::MonsterDefeated,
      .monsterId = 1,
      .serverTick = 600,
      .rulesetVersion = 1,
  };
  auto encodedSpawn =
      RudpCombatCodec::encode(header(RudpFlag::Reliable, 6, 4, 7, 29), spawn);
  auto encodedTerminal = RudpCombatCodec::encode(
      header(RudpFlag::Reliable, 7, 4, 7, 30), terminal);
  if (!encodedSpawn.has_value() || !encodedTerminal.has_value()) {
    return false;
  }
  ReliableQueue queue;
  if (queue.enqueue(6, std::move(*encodedSpawn), ReliableLane::Application,
                    kStart) != ReliableQueueAdmission::Accepted ||
      queue.enqueue(7, std::move(*encodedTerminal), ReliableLane::Application,
                    kStart) != ReliableQueueAdmission::Accepted) {
    return false;
  }
  const auto first = queue.poll(kStart);
  if (first.transmissions.size() != 2 ||
      RudpCombatCodec::decode(first.transmissions[0].datagram).message !=
          std::optional{spawn} ||
      RudpCombatCodec::decode(first.transmissions[1].datagram).message !=
          std::optional{terminal} ||
      queue.discardAcknowledged(6, 0) != 1 || !queue.contains(7)) {
    return false;
  }
  const auto retransmit = queue.poll(kStart + 200ms);
  return retransmit.transmissions.size() == 1 &&
         retransmit.transmissions.front().sequence == 7;
}

bool expiredDeliveryStillReplaysRetainedResult() {
  const AttackCommand command{
      .commandId = CommandId{.high = 1, .low = 9},
      .sessionId = SessionId{1},
      .generation = SessionGeneration{2},
      .battleId = BattleInstanceId{7},
      .targetHint = 1,
  };
  const AttackTerminalResult retained{
      .commandId = command.commandId,
      .battleId = command.battleId,
      .code = AttackResultCode::Ok,
      .monsterId = 1,
      .remainingHitPoints = 1580,
      .rulesetVersion = 1,
      .outcome = CombatOutcome::None,
  };
  AttackResultStore results{command.sessionId, command.generation,
                            command.battleId};
  if (!results.retain(command, retained)) {
    return false;
  }
  results.markBattleCompleted(kStart);

  auto datagram = RudpCombatCodec::encode(
      header(RudpFlag::Reliable, 5, 4, 7, 28),
      RudpCombatMessage{RudpAttackTerminalResult{
          .commandId = RudpCommandId{.high = 1, .low = 9},
          .battleInstanceId = 7,
          .resultCode = RudpAttackResultCode::Ok,
          .monsterId = 1,
          .remainingHitPoints = 1580,
          .rulesetVersion = 1,
          .combatOutcome = RudpCombatOutcome::None,
      }});
  ReliableQueue delivery;
  if (!datagram.has_value() ||
      delivery.enqueue(5, std::move(*datagram), ReliableLane::Application,
                       kStart) != ReliableQueueAdmission::Accepted ||
      delivery.poll(kStart).transmissions.size() != 1) {
    return false;
  }
  const auto expired = delivery.poll(kStart + 5000ms);
  const auto replay = results.inspect(command);
  return expired.expiredSequences == std::vector<std::uint32_t>{5} &&
         delivery.empty() &&
         replay.decision == AttackResultStoreDecision::Replay &&
         replay.result == retained &&
         results.evictExpired(kStart + 29999ms) == 0 &&
         results.evictExpired(kStart + 30000ms) == 1;
}

bool transportAckNeverFabricatesApplicationResult() {
  const AttackCommand command{
      .commandId = CommandId{.high = 1, .low = 10},
      .sessionId = SessionId{1},
      .generation = SessionGeneration{2},
      .battleId = BattleInstanceId{7},
      .targetHint = 1,
  };
  AttackResultStore results{command.sessionId, command.generation,
                            command.battleId};
  const auto inspection = results.inspect(command);
  return isAcknowledged(9, AckState{.ack = 9, .ackBits = 0}) &&
         inspection.decision == AttackResultStoreDecision::Available &&
         !inspection.result.has_value() && results.size() == 0;
}

} // namespace

int main() {
  return matchesGoldenVectors() && rejectsWrongReliabilityAndScope() &&
                 rejectsMalformedPayloadAndEnums() &&
                 reliableLifecycleStreamKeepsBattleScope() &&
                 expiredDeliveryStillReplaysRetainedResult() &&
                 transportAckNeverFabricatesApplicationResult()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
