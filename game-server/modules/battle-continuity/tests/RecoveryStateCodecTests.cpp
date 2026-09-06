#include <lol/battle/BattleAdmission.hpp>
#include <lol/battle/BattleDeterministicState.hpp>
#include <lol/battle/BattleLoadApi.hpp>
#include <lol/battle_continuity/RecoveryStateCodec.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>

namespace {

using namespace lol::battle;
using namespace lol::battle_continuity;
namespace shared = lol::shared;

constexpr std::uint64_t kRoom = (7ULL << 32U) | 11U;
constexpr std::uint64_t kBattle = 31U;

std::optional<BattleInstance> battle() {
  std::array<std::uint8_t, 16> accountOne{};
  accountOne[15] = 1U;
  std::array<std::uint8_t, 16> accountTwo{};
  accountTwo[15] = 2U;
  auto created = BattleInstance::create(BattleAdmissionSnapshot{
      .roomId = shared::RoomId{kRoom},
      .battleId = shared::BattleInstanceId{kBattle},
      .candidates =
          {{.accountId = shared::AccountId{accountOne},
            .sessionId = shared::SessionId{101U},
            .generation = shared::SessionGeneration{1U},
            .nickname = "one"},
           {.accountId = shared::AccountId{accountTwo},
            .sessionId = shared::SessionId{202U},
            .generation = shared::SessionGeneration{1U},
            .nickname = "two"}},
      .rulesetVersion = battleRulesetVersion,
      .seed = 0x0123456789abcdefULL});
  if (created.code != BattleLoadResultCode::Ok || !created.battle.has_value() ||
      created.battle->openLoadBarrier() != BattleLoadResultCode::Ok) {
    return std::nullopt;
  }
  return std::move(created.battle);
}

RoomRecoveryState roomState() {
  return RoomRecoveryState{
      .roomId = shared::RoomId{kRoom},
      .capacity = 2U,
      .hostParticipantSlot = 1U,
      .memberSlots = {1U, 2U},
      .phase = RoomRecoveryPhase::Loading,
      .nextBattleOrdinal = kBattle + 1U,
  };
}

bool roundTrip() {
  const auto created = battle();
  if (!created.has_value()) {
    return false;
  }
  const auto encoded = encodeRecoveryState(created->exportDeterministicState(),
                                           roomState());
  if (!encoded.ok()) {
    return false;
  }
  const auto decoded = decodeRecoveryState(encoded.bytes);
  return decoded.ok() && decoded.state.has_value() &&
         decoded.state->battle == created->exportDeterministicState() &&
         decoded.state->room == roomState() &&
         recoveryStateHash(encoded.bytes).has_value();
}

bool invalidSchemaRejected() {
  const auto created = battle();
  if (!created.has_value()) {
    return false;
  }
  auto encoded = encodeRecoveryState(created->exportDeterministicState(),
                                     roomState());
  if (!encoded.ok() || encoded.bytes.size() < 6U) {
    return false;
  }
  encoded.bytes[4] = 0U;
  encoded.bytes[5] = 0U;
  const auto decoded = decodeRecoveryState(encoded.bytes);
  return !decoded.ok() &&
         decoded.error->code ==
             RecoveryStateCodecErrorCode::UnsupportedSchemaVersion;
}

bool invalidRoomStateRejected() {
  const auto created = battle();
  if (!created.has_value()) {
    return false;
  }
  const auto state = created->exportDeterministicState();
  auto unordered = roomState();
  unordered.memberSlots = {2U, 1U};
  auto invalidHost = roomState();
  invalidHost.hostParticipantSlot = 2U;
  auto invalidPhase = roomState();
  invalidPhase.phase = static_cast<RoomRecoveryPhase>(99U);
  auto invalidOrdinal = roomState();
  invalidOrdinal.nextBattleOrdinal = 0U;
  return encodeRecoveryState(state, unordered).error->code ==
             RecoveryStateCodecErrorCode::InvalidMemberOrder &&
         encodeRecoveryState(state, invalidHost).error->code ==
             RecoveryStateCodecErrorCode::InvalidHostParticipantSlot &&
         encodeRecoveryState(state, invalidPhase).error->code ==
             RecoveryStateCodecErrorCode::InvalidPhase &&
         encodeRecoveryState(state, invalidOrdinal).error->code ==
             RecoveryStateCodecErrorCode::InvalidNextBattleOrdinal;
}

bool activeBattleOrdinalBoundRejected() {
  const auto created = battle();
  if (!created.has_value()) {
    return false;
  }
  const auto state = created->exportDeterministicState();
  auto colliding = roomState();
  colliding.nextBattleOrdinal = state.battleId.value();
  const auto collision = encodeRecoveryState(state, colliding);

  auto encoded = encodeRecoveryState(state, roomState());
  if (!encoded.ok()) {
    return false;
  }
  for (std::size_t index = 0U; index < sizeof(std::uint64_t); ++index) {
    encoded.bytes[encoded.bytes.size() - sizeof(std::uint64_t) + index] =
        static_cast<std::uint8_t>(state.battleId.value() >>
                                  (56U - index * 8U));
  }
  const auto decodedCollision = decodeRecoveryState(encoded.bytes);

  auto maxBattle = state;
  maxBattle.battleId = shared::BattleInstanceId{
      std::numeric_limits<std::uint64_t>::max()};
  auto maxOrdinal = roomState();
  maxOrdinal.nextBattleOrdinal = std::numeric_limits<std::uint64_t>::max();
  const auto wrapped = encodeRecoveryState(maxBattle, maxOrdinal);
  return !collision.ok() && collision.error->code ==
             RecoveryStateCodecErrorCode::InvalidNextBattleOrdinal &&
         !decodedCollision.ok() &&
         decodedCollision.error->code ==
             RecoveryStateCodecErrorCode::InvalidNextBattleOrdinal &&
         !wrapped.ok() && wrapped.error->code ==
             RecoveryStateCodecErrorCode::InvalidNextBattleOrdinal;
}

bool roomStateChangesHash() {
  const auto created = battle();
  if (!created.has_value()) {
    return false;
  }
  const auto state = created->exportDeterministicState();
  auto phase = roomState();
  phase.phase = RoomRecoveryPhase::InProgress;
  auto member = roomState();
  member.memberSlots = {1U};
  member.capacity = 3U;
  auto ordinal = roomState();
  ordinal.nextBattleOrdinal = 3U;
  const auto first = encodeRecoveryState(state, roomState());
  const auto second = encodeRecoveryState(state, phase);
  const auto third = encodeRecoveryState(state, member);
  if (!first.ok() || !second.ok() || !third.ok()) {
    return false;
  }
  auto fourthBytes = first.bytes;
  for (std::size_t index = 0U; index < sizeof(std::uint64_t); ++index) {
    fourthBytes[fourthBytes.size() - sizeof(std::uint64_t) + index] =
        static_cast<std::uint8_t>(ordinal.nextBattleOrdinal >>
                                  (56U - index * 8U));
  }
  const auto firstHash = recoveryStateHash(first.bytes);
  const auto secondHash = recoveryStateHash(second.bytes);
  const auto thirdHash = recoveryStateHash(third.bytes);
  const auto fourthHash = recoveryStateHash(fourthBytes);
  return firstHash.has_value() && secondHash.has_value() &&
         thirdHash.has_value() && fourthHash.has_value() &&
         *firstHash != *secondHash && *firstHash != *thirdHash &&
         *firstHash != *fourthHash;
}

bool run(const char *name, bool (*test)()) {
  if (!test()) {
    std::cerr << "FAIL: " << name << '\n';
    return false;
  }
  return true;
}

} // namespace

int main() {
  return run("round trip", roundTrip) &&
                 run("invalid schema", invalidSchemaRejected) &&
                 run("invalid room state", invalidRoomStateRejected) &&
                 run("active battle ordinal bound",
                     activeBattleOrdinalBoundRejected) &&
                 run("room state hash binding", roomStateChangesHash)
             ? 0
             : 1;
}
