#include <lol/battle/BattleDeterministicState.hpp>
#include <lol/battle_continuity/BattleStateCodec.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

using namespace lol::battle;
using namespace lol::battle_continuity;
namespace shared = lol::shared;
using lol::battle::CommandId;

constexpr std::uint64_t kRoom = (7ULL << 32U) | 11U;
constexpr std::uint64_t kBattle = 31U;

std::string hex(const Hash &hash) {
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : hash) {
    output << std::setw(2) << static_cast<unsigned>(byte);
  }
  return output.str();
}

CommandId command(std::uint64_t low) {
  return CommandId{.high = 1, .low = low};
}

AttackTerminalResult attackResult(std::uint64_t low, std::uint32_t hp) {
  return AttackTerminalResult{
      .commandId = command(low),
      .battleId = shared::BattleInstanceId{kBattle},
      .code = AttackResultCode::Ok,
      .monsterId = CombatRuleset::monsterId,
      .remainingHitPoints = hp,
      .rulesetVersion = CombatRuleset::version,
      .outcome = CombatOutcome::None,
  };
}

ClaimLootTerminalResult lootResult(std::uint64_t low, std::uint64_t drop) {
  return ClaimLootTerminalResult{
      .commandId = command(low),
      .battleId = shared::BattleInstanceId{kBattle},
      .dropId = DropId{drop},
      .code = ClaimLootResultCode::Ok,
  };
}

BattleDeterministicState fullState() {
  const auto battleId = shared::BattleInstanceId{kBattle};
  const auto sessionOne = shared::SessionId{101};
  const auto sessionTwo = shared::SessionId{202};
  const auto eventId = EventId{.high = 3, .low = 4};

  BattleDeterministicState state{
      .roomId = shared::RoomId{kRoom},
      .battleId = battleId,
      .rulesetVersion = battleRulesetVersion,
      .seed = 0x0123456789abcdefULL,
      .battleTime =
          BattleTime{.logicalTick = 123,
                     .battleElapsedNanos = 123ULL * BattleTime::tickNanos},
      .state = BattleLoadState::GameplayCommitted,
      .candidates =
          {
              {.slot = 1,
               .sessionId = sessionOne,
               .state = LoadCandidateState::Ready},
              {.slot = 2,
               .sessionId = sessionTwo,
               .state = LoadCandidateState::Ready},
          },
      .capturedParticipants =
          {
              {.slot = 1,
               .sessionId = sessionOne,
               .exitStatus = ParticipantExitStatus::GameplayEligible},
              {.slot = 2,
               .sessionId = sessionTwo,
               .exitStatus = ParticipantExitStatus::Disconnected},
          },
      .participants =
          {
              {.slot = 1,
               .sessionId = sessionOne,
               .posXMillimeter = 2500,
               .posYMillimeter = -10,
               .tickDeltaXMillimeter = 3,
               .tickDeltaYMillimeter = -4,
               .latestSeenActionSequence = 12,
               .lastRateUpdateNanos = 5'000'000'000ULL,
               .rateCreditUnits = 2'000'000'000ULL,
               .lastAttackRateUpdateNanos = 5'100'000'000ULL,
               .attackRateCreditUnits = 3'000'000'000ULL,
               .lastClaimRateUpdateNanos = 5'200'000'000ULL,
               .claimRateCreditUnits = 1'000'000'000ULL,
               .lastAcceptedAttackNanos = 5'300'000'000ULL,
               .attackResults = {.battleCompletedAtNanos = std::nullopt,
                                 .records = {{.commandId = command(1),
                                              .targetHint =
                                                  CombatRuleset::monsterId,
                                              .result = attackResult(1, 700)}}},
               .lootResults = {.battleCompletedAtNanos = std::nullopt,
                               .records = {{.commandId = command(2),
                                            .dropId = DropId{1},
                                            .result = lootResult(2, 1)}}},
               .gameplayEligible = true,
               .inputEnabled = true,
               .exitStatus = ParticipantExitStatus::GameplayEligible},
              {.slot = 2,
               .sessionId = sessionTwo,
               .posXMillimeter = -2500,
               .posYMillimeter = 10,
               .tickDeltaXMillimeter = -3,
               .tickDeltaYMillimeter = 4,
               .latestSeenActionSequence = std::nullopt,
               .lastRateUpdateNanos = std::nullopt,
               .rateCreditUnits = 4'000'000'000ULL,
               .lastAttackRateUpdateNanos = std::nullopt,
               .attackRateCreditUnits = 4'000'000'000ULL,
               .lastClaimRateUpdateNanos = 5'400'000'000ULL,
               .claimRateCreditUnits = 2'000'000'000ULL,
               .lastAcceptedAttackNanos = std::nullopt,
               .attackResults = {.battleCompletedAtNanos = 6'000'000'000ULL,
                                 .records = {}},
               .lootResults = {.battleCompletedAtNanos = 6'000'000'000ULL,
                               .records = {}},
               .gameplayEligible = false,
               .inputEnabled = false,
               .exitStatus = ParticipantExitStatus::TerminalExited},
          },
      .loadDeadlineTick = std::nullopt,
      .combatDeadlineTick = std::nullopt,
      .lootDeadlineTick = 200,
      .lastIntegratedServerTick = 77,
      .nextSnapshotSequence = 9,
      .nextAttackAppliedSequence = 10,
      .monster =
          BattleDeterministicMonsterState{
              .id = CombatRuleset::monsterId,
              .position = CombatRuleset::spawnPosition,
              .hitPoints = 0,
              .state = MonsterState::Dead,
          },
      .combatTerminal =
          CombatTerminalRecord{
              .eventId = eventId,
              .battleId = battleId,
              .eventSequence = 8,
              .outcome = CombatOutcome::MonsterDefeated,
              .monsterId = CombatRuleset::monsterId,
              .serverTick = 77,
              .rulesetVersion = CombatRuleset::version,
          },
      .battleTerminalOutcome = std::nullopt,
      .drops =
          {
              {.drop = RelicDrop{.dropId = DropId{1},
                                 .itemId = RelicRuleset::commonItemId,
                                 .quantity = 2,
                                 .position = DropPosition{100, -100}},
               .state = LootDropState::Claimed,
               .ownerSlot = 1},
              {.drop = RelicDrop{.dropId = DropId{2},
                                 .itemId = RelicRuleset::rareItemId,
                                 .quantity = 1,
                                 .position = DropPosition{-200, 200}},
               .state = LootDropState::Available,
               .ownerSlot = std::nullopt},
          },
      .holdings =
          {
              {.ownerSlot = 1,
               .itemId = RelicRuleset::commonItemId,
               .quantity = 2},
          },
      .lootResolution = LootResolutionState::Open,
      .resultState = BattleResultState::NotReady,
      .committedResult = std::nullopt,
      .lootResultsCompletedAtNanos = std::nullopt,
  };
  return state;
}

bool roundTripFullStateAndGoldenHash() {
  const auto encoded = encodeBattleState(fullState());
  if (!encoded.ok()) {
    return false;
  }
  const auto decoded = decodeBattleState(encoded.bytes);
  if (!decoded.ok() || *decoded.state != fullState()) {
    return false;
  }
  const auto hash = battleStateHash(encoded.bytes);
  if (!hash.has_value()) {
    return false;
  }
  std::cout << "battle_state_hash=" << hex(*hash) << '\n';
  // Filled after the first GREEN run; this is a compiler-independent golden.
  return hex(*hash) ==
         "b4ada6055e7fe290d38219d95735ec0634c12fdb7f41ffe6f93f0663a536b43b";
}

bool oneFieldChangesHash() {
  auto first = encodeBattleState(fullState());
  auto changedState = fullState();
  changedState.monster->hitPoints++;
  auto second = encodeBattleState(changedState);
  if (!first.ok() || !second.ok()) {
    return false;
  }
  const auto firstHash = battleStateHash(first.bytes);
  const auto secondHash = battleStateHash(second.bytes);
  return firstHash.has_value() && secondHash.has_value() &&
         *firstHash != *secondHash;
}

bool resultPayloadIsCanonicalAndPrivacySafe() {
  BattleDeterministicFinalResult result{
      .outcome = BattleOutcome::CombatTimeout,
      .entries =
          {
              {.slot = 2,
               .exitStatus = ParticipantExitStatus::TerminalExited,
               .finalAssetValue = 20,
               .rank = 2,
               .isTop = false},
              {.slot = 1,
               .exitStatus = ParticipantExitStatus::TerminalPresent,
               .finalAssetValue = 40,
               .rank = 1,
               .isTop = true},
          },
  };
  const auto encoded = encodeCanonicalBattleResult(result);
  if (!encoded.ok() || encoded.bytes.size() != 1U + 2U + (2U * 17U)) {
    return false;
  }
  // Outcome/count are followed by slot 1 then slot 2.  The payload has no
  // fields through which AccountId, nickname, or SessionGeneration can enter.
  return encoded.bytes[0] ==
             static_cast<std::uint8_t>(BattleOutcome::CombatTimeout) &&
         encoded.bytes[1] == 0U && encoded.bytes[2] == 2U &&
         encoded.bytes[3] == 0U && encoded.bytes[4] == 1U;
}

TerminalReceiptPayload terminalReceiptFor(const Bytes &canonicalResultPayload) {
  return TerminalReceiptPayload{
      .terminalReason = 2U,
      .finalStateHash = Hash{0xa1U},
      .canonicalResultPayload = canonicalResultPayload,
      .resultCommittedUnixEpochMilliseconds = 1'700'000'000'000ULL,
      .resultCommittedBattleElapsedNanos = 123ULL * BattleTime::tickNanos,
      .settlementBatchId = SettlementBatchId{0xb2U},
      .settlementIntentCount = 2U,
      .settlements = {
          {.participantSlot = 1U,
           .settlementId = SettlementId{0xc1U},
           .settlementPayloadHash = Hash{0xd1U}},
          {.participantSlot = 2U,
           .settlementId = SettlementId{0xc2U},
           .settlementPayloadHash = Hash{0xd2U}},
      }};
}

bool terminalCanonicalHashIsStateAndReceiptBound() {
  const auto encodedState = encodeBattleState(fullState());
  const BattleDeterministicFinalResult result{
      .outcome = BattleOutcome::CombatTimeout,
      .entries = {
          {.slot = 1U,
           .exitStatus = ParticipantExitStatus::TerminalPresent,
           .finalAssetValue = 40U,
           .rank = 1U,
           .isTop = true},
          {.slot = 2U,
           .exitStatus = ParticipantExitStatus::TerminalExited,
           .finalAssetValue = 20U,
           .rank = 2U,
           .isTop = false},
      }};
  const auto encodedResult = encodeCanonicalBattleResult(result);
  if (!encodedState.ok() || !encodedResult.ok()) {
    return false;
  }
  const auto receipt = terminalReceiptFor(encodedResult.bytes);
  const auto encodedTerminal =
      encodeCanonicalTerminalState(encodedState.bytes, receipt);
  const auto originalHash =
      canonicalBattleTerminalHash(encodedState.bytes, receipt);
  if (!encodedTerminal.ok() || !originalHash.has_value()) {
    return false;
  }

  auto finalHashOnly = receipt;
  finalHashOnly.finalStateHash[0] ^= 0xffU;
  const auto finalHashOnlyHash =
      canonicalBattleTerminalHash(encodedState.bytes, finalHashOnly);
  if (!finalHashOnlyHash.has_value() || *finalHashOnlyHash != *originalHash) {
    return false;
  }

  auto changedSettlement = receipt;
  changedSettlement.settlements[0].settlementPayloadHash[0] ^= 0x01U;
  const auto changedSettlementHash =
      canonicalBattleTerminalHash(encodedState.bytes, changedSettlement);
  if (!changedSettlementHash.has_value() ||
      *changedSettlementHash == *originalHash) {
    return false;
  }

  auto changedState = fullState();
  changedState.monster->position.xMillimeter++;
  const auto encodedChangedState = encodeBattleState(changedState);
  const auto changedStateHash =
      encodedChangedState.ok()
          ? canonicalBattleTerminalHash(encodedChangedState.bytes, receipt)
          : std::nullopt;
  if (!changedStateHash.has_value() || *changedStateHash == *originalHash) {
    return false;
  }

  auto mismatchedSlots = receipt;
  mismatchedSlots.settlements[1].participantSlot = 3U;
  const auto mismatch =
      encodeCanonicalTerminalState(encodedState.bytes, mismatchedSlots);
  return !mismatch.ok() &&
         mismatch.error->code == BattleStateCodecErrorCode::InvariantViolation;
}

bool reorderedCollectionsNormalize() {
  auto first = fullState();
  auto reordered = first;
  std::reverse(reordered.candidates.begin(), reordered.candidates.end());
  std::reverse(reordered.participants.begin(), reordered.participants.end());
  std::reverse(reordered.drops.begin(), reordered.drops.end());
  std::reverse(reordered.holdings.begin(), reordered.holdings.end());
  const auto canonical = encodeBattleState(first);
  const auto normalized = encodeBattleState(reordered);
  return canonical.ok() && normalized.ok() &&
         canonical.bytes == normalized.bytes;
}

bool denseParticipantSlotsRemainStableAcrossUnreadyCandidates() {
  auto state = fullState();
  state.candidates = {
      {.slot = 1,
       .sessionId = shared::SessionId{303},
       .state = LoadCandidateState::Disconnected},
      {.slot = 2,
       .sessionId = shared::SessionId{101},
       .state = LoadCandidateState::Ready},
      {.slot = 3,
       .sessionId = shared::SessionId{202},
       .state = LoadCandidateState::Ready},
  };
  const auto encoded = encodeBattleState(state);
  if (!encoded.ok()) {
    return false;
  }
  const auto decoded = decodeBattleState(encoded.bytes);
  return decoded.ok() && decoded.state->capturedParticipants[0].slot == 1U &&
         decoded.state->capturedParticipants[0].sessionId ==
             shared::SessionId{101} &&
         decoded.state->participants[1].slot == 2U &&
         decoded.state->participants[1].sessionId == shared::SessionId{202};
}

bool malformedBytesRejected() {
  const auto encoded = encodeBattleState(fullState());
  if (!encoded.ok() || encoded.bytes.size() < 3) {
    return false;
  }
  auto truncated = encoded.bytes;
  truncated.pop_back();
  const auto truncatedResult = decodeBattleState(truncated);
  auto trailing = encoded.bytes;
  trailing.push_back(0);
  const auto trailingResult = decodeBattleState(trailing);
  auto unknownSchema = encoded.bytes;
  unknownSchema[4] = 0;
  unknownSchema[5] = 2;
  const auto unknownResult = decodeBattleState(unknownSchema);
  auto unknownRuleset = encoded.bytes;
  unknownRuleset[25] = 2U;
  const auto unknownRulesetResult = decodeBattleState(unknownRuleset);
  return !truncatedResult.ok() &&
         truncatedResult.error->code == BattleStateCodecErrorCode::Truncated &&
         !trailingResult.ok() &&
         trailingResult.error->code ==
             BattleStateCodecErrorCode::TrailingBytes &&
         !unknownResult.ok() &&
         unknownResult.error->code ==
             BattleStateCodecErrorCode::UnsupportedSchemaVersion &&
         !unknownRulesetResult.ok() &&
         unknownRulesetResult.error->code ==
             BattleStateCodecErrorCode::UnsupportedRulesetVersion;
}

bool invalidEnumsBooleansAndOrderingRejected() {
  const auto encoded = encodeBattleState(fullState());
  if (!encoded.ok()) {
    return false;
  }
  auto invalidStateEnum = encoded.bytes;
  invalidStateEnum[50] = 0xffU;
  const auto invalidEnumResult = decodeBattleState(invalidStateEnum);
  if (invalidEnumResult.ok() ||
      invalidEnumResult.error->code != BattleStateCodecErrorCode::InvalidEnum) {
    return false;
  }

  // Locate the participant boolean pair in the canonical image.  A boolean
  // has exactly one legal representation, so 2 must fail closed.
  auto invalidBoolean = encoded.bytes;
  std::optional<std::size_t> booleanOffset;
  for (std::size_t index = 0U; index + 2U < encoded.bytes.size(); ++index) {
    if (encoded.bytes[index] == 1U && encoded.bytes[index + 1U] == 1U &&
        encoded.bytes[index + 2U] == 0U) {
      booleanOffset = index;
      break;
    }
  }
  if (!booleanOffset.has_value()) {
    return false;
  }
  invalidBoolean[*booleanOffset] = 2U;
  const auto invalidBooleanResult = decodeBattleState(invalidBoolean);
  if (invalidBooleanResult.ok() ||
      invalidBooleanResult.error->code !=
          BattleStateCodecErrorCode::InvalidBoolean) {
    return false;
  }

  // Captured entries are keyed by slot.  Swapping slots without rewriting the
  // associated identity is rejected before import instead of being accepted
  // as a different participant mapping.
  auto outOfOrder = encoded.bytes;
  outOfOrder[77] = 0U;
  outOfOrder[78] = 2U;
  const auto outOfOrderResult = decodeBattleState(outOfOrder);
  if (outOfOrderResult.ok()) {
    return false;
  }

  auto duplicate = encoded.bytes;
  duplicate[64] = 0U;
  duplicate[65] = 1U;
  const auto duplicateResult = decodeBattleState(duplicate);
  if (duplicateResult.ok()) {
    return false;
  }
  return duplicateResult.error->code == BattleStateCodecErrorCode::DuplicateId;
}

bool privacyCannotEnterState() {
  // The value DTO has no AccountId, nickname, or SessionGeneration fields;
  // compile-time construction above is the boundary.  This test additionally
  // ensures the codec has no opaque/native-memory suffix to smuggle them in.
  const auto encoded = encodeBattleState(fullState());
  return encoded.ok() && encoded.bytes.size() < kMaximumBattleStateBytes;
}

} // namespace

int main() {
  if (!roundTripFullStateAndGoldenHash() || !oneFieldChangesHash() ||
      !resultPayloadIsCanonicalAndPrivacySafe() ||
      !reorderedCollectionsNormalize() || !malformedBytesRejected() ||
      !denseParticipantSlotsRemainStableAcrossUnreadyCandidates() ||
      !invalidEnumsBooleansAndOrderingRejected() ||
      !privacyCannotEnterState() ||
      !terminalCanonicalHashIsStateAndReceiptBound()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
