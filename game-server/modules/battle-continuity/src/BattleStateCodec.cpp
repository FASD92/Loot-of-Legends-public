#include <lol/battle_continuity/BattleStateCodec.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace lol::battle_continuity {
namespace {

using battle::AttackResultCode;
using battle::AttackResultRecordState;
using battle::AttackResultStoreState;
using battle::AttackTerminalResult;
using battle::BattleDeterministicCandidateState;
using battle::BattleDeterministicCapturedParticipantState;
using battle::BattleDeterministicDropState;
using battle::BattleDeterministicFinalResult;
using battle::BattleDeterministicHoldingState;
using battle::BattleDeterministicMonsterState;
using battle::BattleDeterministicParticipantState;
using battle::BattleDeterministicResultEntry;
using battle::BattleDeterministicState;
using battle::BattleLoadState;
using battle::BattleOutcome;
using battle::BattleResultState;
using battle::ClaimLootResultCode;
using battle::ClaimLootTerminalResult;
using battle::CombatOutcome;
using battle::CombatTerminalRecord;
using battle::DropId;
using battle::EventId;
using battle::LoadCandidateState;
using battle::LootDropState;
using battle::LootResolutionState;
using battle::LootResultRecordState;
using battle::LootResultStoreState;
using battle::MonsterState;
using battle::ParticipantExitStatus;
using battle::ParticipantSlot;

using ByteView = std::span<const std::uint8_t>;

struct Writer final {
  Bytes bytes;

  void u8(std::uint8_t value) { bytes.push_back(value); }

  void u16(std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value));
  }

  void u32(std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value));
  }

  void u64(std::uint64_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 56U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 48U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 40U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 32U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value));
  }

  void i32(std::int32_t value) { u32(std::bit_cast<std::uint32_t>(value)); }

  void bytesFrom(ByteView value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
  }

  template <std::size_t N>
  void array(const std::array<std::uint8_t, N> &value) {
    bytesFrom(value);
  }
};

struct Reader final {
  ByteView view;
  std::size_t position{0};
  std::optional<BattleStateCodecError> error;

  [[nodiscard]] std::size_t remaining() const noexcept {
    return view.size() - position;
  }

  void fail(BattleStateCodecErrorCode code, std::size_t at) noexcept {
    if (!error.has_value()) {
      error = BattleStateCodecError{.code = code, .offset = at};
    }
  }

  [[nodiscard]] bool take(std::size_t count, ByteView &out) noexcept {
    if (count > remaining()) {
      fail(BattleStateCodecErrorCode::Truncated, position);
      return false;
    }
    out = view.subspan(position, count);
    position += count;
    return true;
  }

  [[nodiscard]] bool u8(std::uint8_t &value) noexcept {
    ByteView bytes;
    if (!take(1U, bytes)) {
      return false;
    }
    value = bytes[0];
    return true;
  }

  [[nodiscard]] bool u16(std::uint16_t &value) noexcept {
    ByteView bytes;
    if (!take(2U, bytes)) {
      return false;
    }
    value = static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(bytes[0]) << 8U) |
        static_cast<std::uint32_t>(bytes[1]));
    return true;
  }

  [[nodiscard]] bool u32(std::uint32_t &value) noexcept {
    ByteView bytes;
    if (!take(4U, bytes)) {
      return false;
    }
    value = (static_cast<std::uint32_t>(bytes[0]) << 24U) |
            (static_cast<std::uint32_t>(bytes[1]) << 16U) |
            (static_cast<std::uint32_t>(bytes[2]) << 8U) |
            static_cast<std::uint32_t>(bytes[3]);
    return true;
  }

  [[nodiscard]] bool u64(std::uint64_t &value) noexcept {
    ByteView bytes;
    if (!take(8U, bytes)) {
      return false;
    }
    value = (static_cast<std::uint64_t>(bytes[0]) << 56U) |
            (static_cast<std::uint64_t>(bytes[1]) << 48U) |
            (static_cast<std::uint64_t>(bytes[2]) << 40U) |
            (static_cast<std::uint64_t>(bytes[3]) << 32U) |
            (static_cast<std::uint64_t>(bytes[4]) << 24U) |
            (static_cast<std::uint64_t>(bytes[5]) << 16U) |
            (static_cast<std::uint64_t>(bytes[6]) << 8U) |
            static_cast<std::uint64_t>(bytes[7]);
    return true;
  }

  [[nodiscard]] bool i32(std::int32_t &value) noexcept {
    std::uint32_t bits = 0U;
    if (!u32(bits)) {
      return false;
    }
    value = std::bit_cast<std::int32_t>(bits);
    return true;
  }

  [[nodiscard]] bool bytesTo(std::size_t count, Bytes &value) noexcept {
    ByteView bytes;
    if (!take(count, bytes)) {
      return false;
    }
    value.assign(bytes.begin(), bytes.end());
    return true;
  }

  template <std::size_t N>
  [[nodiscard]] bool array(std::array<std::uint8_t, N> &value) noexcept {
    ByteView bytes;
    if (!take(N, bytes)) {
      return false;
    }
    std::copy(bytes.begin(), bytes.end(), value.begin());
    return true;
  }

  [[nodiscard]] bool boolean(bool &value) noexcept {
    std::uint8_t raw = 0U;
    if (!u8(raw)) {
      return false;
    }
    if (raw > 1U) {
      fail(BattleStateCodecErrorCode::InvalidBoolean, position - 1U);
      return false;
    }
    value = raw == 1U;
    return true;
  }

  [[nodiscard]] bool optionalU64(std::optional<std::uint64_t> &value) noexcept {
    std::uint8_t present = 0U;
    if (!u8(present)) {
      return false;
    }
    if (present > 1U) {
      fail(BattleStateCodecErrorCode::InvalidBoolean, position - 1U);
      return false;
    }
    if (present == 0U) {
      value.reset();
      return true;
    }
    std::uint64_t decoded = 0U;
    if (!u64(decoded)) {
      return false;
    }
    value = decoded;
    return true;
  }

  [[nodiscard]] bool optionalU32(std::optional<std::uint32_t> &value) noexcept {
    std::uint8_t present = 0U;
    if (!u8(present)) {
      return false;
    }
    if (present > 1U) {
      fail(BattleStateCodecErrorCode::InvalidBoolean, position - 1U);
      return false;
    }
    if (present == 0U) {
      value.reset();
      return true;
    }
    std::uint32_t decoded = 0U;
    if (!u32(decoded)) {
      return false;
    }
    value = decoded;
    return true;
  }

  [[nodiscard]] bool
  optionalSlot(std::optional<ParticipantSlot> &value) noexcept {
    std::uint8_t present = 0U;
    if (!u8(present)) {
      return false;
    }
    if (present > 1U) {
      fail(BattleStateCodecErrorCode::InvalidBoolean, position - 1U);
      return false;
    }
    if (present == 0U) {
      value.reset();
      return true;
    }
    std::uint16_t decoded = 0U;
    if (!u16(decoded)) {
      return false;
    }
    value = decoded;
    return true;
  }
};

template <typename T>
void putOptionalU64(Writer &writer, const std::optional<T> &value) {
  writer.u8(value.has_value() ? 1U : 0U);
  if (value.has_value()) {
    writer.u64(static_cast<std::uint64_t>(*value));
  }
}

void putOptionalU32(Writer &writer, const std::optional<std::uint32_t> &value) {
  writer.u8(value.has_value() ? 1U : 0U);
  if (value.has_value()) {
    writer.u32(*value);
  }
}

void putOptionalSlot(Writer &writer,
                     const std::optional<ParticipantSlot> &value) {
  writer.u8(value.has_value() ? 1U : 0U);
  if (value.has_value()) {
    writer.u16(*value);
  }
}

void putCommandId(Writer &writer, const battle::CommandId &id) {
  writer.u64(id.high);
  writer.u64(id.low);
}

bool getCommandId(Reader &reader, battle::CommandId &id) {
  return reader.u64(id.high) && reader.u64(id.low);
}

void putEventId(Writer &writer, const EventId &id) {
  writer.u64(id.high);
  writer.u64(id.low);
}

bool getEventId(Reader &reader, EventId &id) {
  return reader.u64(id.high) && reader.u64(id.low);
}

bool commandIdLess(const battle::CommandId &lhs,
                   const battle::CommandId &rhs) noexcept {
  return lhs.high < rhs.high || (lhs.high == rhs.high && lhs.low < rhs.low);
}

bool commandIdEqual(const battle::CommandId &lhs,
                    const battle::CommandId &rhs) noexcept {
  return lhs.high == rhs.high && lhs.low == rhs.low;
}

bool validLoadState(BattleLoadState value) noexcept {
  switch (value) {
  case BattleLoadState::Created:
  case BattleLoadState::LoadBarrierOpen:
  case BattleLoadState::GameplayCommitted:
  case BattleLoadState::LoadCancelled:
    return true;
  }
  return false;
}

bool validCandidateState(LoadCandidateState value) noexcept {
  switch (value) {
  case LoadCandidateState::PendingLoad:
  case LoadCandidateState::Ready:
  case LoadCandidateState::Disconnected:
  case LoadCandidateState::TimedOut:
    return true;
  }
  return false;
}

bool validExitStatus(ParticipantExitStatus value) noexcept {
  switch (value) {
  case ParticipantExitStatus::GameplayEligible:
  case ParticipantExitStatus::VoluntaryLeft:
  case ParticipantExitStatus::Disconnected:
  case ParticipantExitStatus::TerminalPresent:
  case ParticipantExitStatus::TerminalExited:
    return true;
  }
  return false;
}

bool validAttackResultCode(AttackResultCode value) noexcept {
  switch (value) {
  case AttackResultCode::Ok:
  case AttackResultCode::NotEligible:
  case AttackResultCode::StaleSession:
  case AttackResultCode::StaleBattle:
  case AttackResultCode::InvalidTarget:
  case AttackResultCode::OutOfRange:
  case AttackResultCode::Cooldown:
  case AttackResultCode::Overloaded:
  case AttackResultCode::CommandConflict:
  case AttackResultCode::TerminalAlreadyDecided:
    return true;
  }
  return false;
}

bool validCombatOutcome(CombatOutcome value) noexcept {
  switch (value) {
  case CombatOutcome::None:
  case CombatOutcome::MonsterDefeated:
  case CombatOutcome::CombatTimeout:
    return true;
  }
  return false;
}

bool validMonsterState(MonsterState value) noexcept {
  switch (value) {
  case MonsterState::Alive:
  case MonsterState::Dying:
  case MonsterState::Dead:
  case MonsterState::TimedOut:
    return true;
  }
  return false;
}

bool validLootDropState(LootDropState value) noexcept {
  switch (value) {
  case LootDropState::Available:
  case LootDropState::Claimed:
  case LootDropState::Unclaimed:
    return true;
  }
  return false;
}

bool validLootResolutionState(LootResolutionState value) noexcept {
  switch (value) {
  case LootResolutionState::NotStarted:
  case LootResolutionState::Open:
  case LootResolutionState::Resolved:
    return true;
  }
  return false;
}

bool validClaimLootResultCode(ClaimLootResultCode value) noexcept {
  switch (value) {
  case ClaimLootResultCode::Ok:
  case ClaimLootResultCode::NotEligible:
  case ClaimLootResultCode::StaleSession:
  case ClaimLootResultCode::StaleBattle:
  case ClaimLootResultCode::InvalidDrop:
  case ClaimLootResultCode::UnknownDrop:
  case ClaimLootResultCode::OutOfRange:
  case ClaimLootResultCode::AlreadyClaimed:
  case ClaimLootResultCode::Overloaded:
  case ClaimLootResultCode::CommandConflict:
  case ClaimLootResultCode::CatalogRejected:
  case ClaimLootResultCode::ResolutionClosed:
    return true;
  }
  return false;
}

bool validBattleOutcome(BattleOutcome value) noexcept {
  switch (value) {
  case BattleOutcome::MonsterDefeated:
  case BattleOutcome::CombatTimeout:
  case BattleOutcome::CancelledNoActiveParticipants:
    return true;
  }
  return false;
}

bool validBattleResultState(BattleResultState value) noexcept {
  switch (value) {
  case BattleResultState::NotReady:
  case BattleResultState::Committed:
  case BattleResultState::ResultGenerationFailed:
    return true;
  }
  return false;
}

bool validResultRuleset(std::uint16_t value) noexcept {
  return value == battle::CombatRuleset::version;
}

bool validParticipantSlot(ParticipantSlot slot,
                          std::size_t candidateCount) noexcept {
  return slot != battle::systemParticipantSlot &&
         static_cast<std::size_t>(slot) <= candidateCount;
}

bool duplicateSessionId(
    const std::vector<BattleDeterministicCandidateState> &values,
    std::size_t index) noexcept {
  for (std::size_t prior = 0U; prior < index; ++prior) {
    if (values[prior].sessionId == values[index].sessionId) {
      return true;
    }
  }
  return false;
}

bool duplicateSlot(
    const std::vector<BattleDeterministicCapturedParticipantState> &values,
    std::size_t index) noexcept {
  for (std::size_t prior = 0U; prior < index; ++prior) {
    if (values[prior].slot == values[index].slot) {
      return true;
    }
  }
  return false;
}

bool duplicateParticipantSlot(
    const std::vector<BattleDeterministicParticipantState> &values,
    std::size_t index) noexcept {
  for (std::size_t prior = 0U; prior < index; ++prior) {
    if (values[prior].slot == values[index].slot) {
      return true;
    }
  }
  return false;
}

bool capturedHasSlot(
    const std::vector<BattleDeterministicCapturedParticipantState> &values,
    ParticipantSlot slot) noexcept {
  return std::ranges::any_of(
      values, [slot](const auto &entry) { return entry.slot == slot; });
}

std::optional<BattleStateCodecErrorCode>
validateAttackStore(const AttackResultStoreState &store,
                    shared::BattleInstanceId battleId, std::uint64_t elapsed,
                    bool requireCanonicalOrder) noexcept {
  if (store.records.size() > kMaximumBattleResultRecords) {
    return BattleStateCodecErrorCode::InvalidLength;
  }
  if (store.battleCompletedAtNanos.has_value() &&
      *store.battleCompletedAtNanos > elapsed) {
    return BattleStateCodecErrorCode::InvariantViolation;
  }
  for (std::size_t index = 0U; index < store.records.size(); ++index) {
    const auto &entry = store.records[index];
    if (index != 0U) {
      if (commandIdEqual(store.records[index - 1U].commandId,
                         entry.commandId)) {
        return BattleStateCodecErrorCode::DuplicateId;
      }
      if (requireCanonicalOrder &&
          commandIdLess(entry.commandId, store.records[index - 1U].commandId)) {
        return BattleStateCodecErrorCode::InvalidOrdering;
      }
    }
    const auto &result = entry.result;
    if (!validAttackResultCode(result.code) ||
        !validCombatOutcome(result.outcome) ||
        !validResultRuleset(result.rulesetVersion) ||
        result.commandId != entry.commandId || result.battleId != battleId ||
        result.monsterId != battle::CombatRuleset::monsterId) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
  }
  return std::nullopt;
}

std::optional<BattleStateCodecErrorCode>
validateLootStore(const LootResultStoreState &store,
                  shared::BattleInstanceId battleId, std::uint64_t elapsed,
                  bool requireCanonicalOrder) noexcept {
  if (store.records.size() > kMaximumBattleResultRecords) {
    return BattleStateCodecErrorCode::InvalidLength;
  }
  if (store.battleCompletedAtNanos.has_value() &&
      *store.battleCompletedAtNanos > elapsed) {
    return BattleStateCodecErrorCode::InvariantViolation;
  }
  for (std::size_t index = 0U; index < store.records.size(); ++index) {
    const auto &entry = store.records[index];
    if (index != 0U) {
      if (commandIdEqual(store.records[index - 1U].commandId,
                         entry.commandId)) {
        return BattleStateCodecErrorCode::DuplicateId;
      }
      if (requireCanonicalOrder &&
          commandIdLess(entry.commandId, store.records[index - 1U].commandId)) {
        return BattleStateCodecErrorCode::InvalidOrdering;
      }
    }
    const auto &result = entry.result;
    if (!validClaimLootResultCode(result.code) ||
        result.commandId != entry.commandId || result.battleId != battleId ||
        result.dropId != entry.dropId ||
        (result.code == battle::ClaimLootResultCode::Ok &&
         result.dropId.value == 0U)) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
  }
  return std::nullopt;
}

std::optional<BattleStateCodecErrorCode>
validateFinalResultBasic(const BattleDeterministicFinalResult &result,
                         bool requireCanonicalOrder) noexcept {
  if (!validBattleOutcome(result.outcome) || result.entries.size() < 2U ||
      result.entries.size() > kMaximumBattleParticipants) {
    return BattleStateCodecErrorCode::InvariantViolation;
  }
  for (std::size_t index = 0U; index < result.entries.size(); ++index) {
    const auto &entry = result.entries[index];
    if (entry.slot == battle::systemParticipantSlot ||
        static_cast<std::size_t>(entry.slot) > kMaximumBattleParticipants ||
        !validExitStatus(entry.exitStatus) ||
        (entry.rank.has_value() && *entry.rank == 0U)) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    if (index != 0U) {
      if (entry.slot == result.entries[index - 1U].slot) {
        return BattleStateCodecErrorCode::DuplicateId;
      }
      if (requireCanonicalOrder &&
          entry.slot < result.entries[index - 1U].slot) {
        return BattleStateCodecErrorCode::InvalidOrdering;
      }
    }
  }
  return std::nullopt;
}

std::optional<BattleStateCodecErrorCode>
validateState(const BattleDeterministicState &state,
              bool requireCanonicalOrder) noexcept {
  if (state.roomId.value() == 0U || state.battleId.value() == 0U ||
      state.seed == 0U) {
    return BattleStateCodecErrorCode::InvalidIdentity;
  }
  if (state.rulesetVersion != kSupportedBattleRulesetVersion) {
    return BattleStateCodecErrorCode::UnsupportedRulesetVersion;
  }
  if (!state.battleTime.valid()) {
    return BattleStateCodecErrorCode::InvalidLogicalTime;
  }
  if (!validLoadState(state.state) ||
      !validLootResolutionState(state.lootResolution) ||
      !validBattleResultState(state.resultState)) {
    return BattleStateCodecErrorCode::InvalidEnum;
  }
  if (state.candidates.size() < 2U ||
      state.candidates.size() > kMaximumBattleParticipants ||
      state.capturedParticipants.size() > kMaximumBattleParticipants ||
      state.participants.size() > kMaximumBattleParticipants ||
      state.drops.size() > kMaximumBattleDrops ||
      state.holdings.size() > kMaximumBattleHoldings) {
    return BattleStateCodecErrorCode::InvalidLength;
  }
  if (state.nextSnapshotSequence == 0U ||
      state.nextAttackAppliedSequence == 0U) {
    return BattleStateCodecErrorCode::InvariantViolation;
  }

  for (std::size_t index = 0U; index < state.candidates.size(); ++index) {
    const auto &entry = state.candidates[index];
    if (entry.slot == battle::systemParticipantSlot ||
        static_cast<std::size_t>(entry.slot) > state.candidates.size() ||
        !validCandidateState(entry.state) || entry.sessionId.value() == 0U) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    if (index != 0U) {
      if (entry.slot == state.candidates[index - 1U].slot ||
          duplicateSessionId(state.candidates, index)) {
        return BattleStateCodecErrorCode::DuplicateId;
      }
      if (requireCanonicalOrder &&
          entry.slot < state.candidates[index - 1U].slot) {
        return BattleStateCodecErrorCode::InvalidOrdering;
      }
    }
    if (entry.slot != static_cast<ParticipantSlot>(index + 1U)) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
  }
  const auto allCandidatesTerminal =
      std::ranges::all_of(state.candidates, [](const auto &entry) {
        return entry.state != LoadCandidateState::PendingLoad;
      });
  const auto readyCandidateCount = static_cast<std::size_t>(std::count_if(
      state.candidates.begin(), state.candidates.end(), [](const auto &entry) {
        return entry.state == LoadCandidateState::Ready;
      }));
  switch (state.state) {
  case BattleLoadState::Created:
    if (allCandidatesTerminal || readyCandidateCount != 0U) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    break;
  case BattleLoadState::LoadBarrierOpen:
    if (allCandidatesTerminal) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    break;
  case BattleLoadState::GameplayCommitted:
    if (!allCandidatesTerminal || readyCandidateCount < 2U) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    break;
  case BattleLoadState::LoadCancelled:
    if (!allCandidatesTerminal || readyCandidateCount >= 2U) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    break;
  }

  std::vector<bool> capturedCandidateSeen(state.candidates.size(), false);
  for (std::size_t index = 0U; index < state.capturedParticipants.size();
       ++index) {
    const auto &entry = state.capturedParticipants[index];
    if (!validParticipantSlot(entry.slot, state.capturedParticipants.size()) ||
        !validExitStatus(entry.exitStatus) ||
        entry.slot != static_cast<ParticipantSlot>(index + 1U)) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    if (index != 0U) {
      if (duplicateSlot(state.capturedParticipants, index)) {
        return BattleStateCodecErrorCode::DuplicateId;
      }
      if (requireCanonicalOrder &&
          entry.slot < state.capturedParticipants[index - 1U].slot) {
        return BattleStateCodecErrorCode::InvalidOrdering;
      }
    }
    const auto candidate =
        std::find_if(state.candidates.begin(), state.candidates.end(),
                     [&entry](const auto &candidateEntry) {
                       return candidateEntry.sessionId == entry.sessionId;
                     });
    if (candidate == state.candidates.end() ||
        candidate->state != LoadCandidateState::Ready) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    const auto candidateIndex = static_cast<std::size_t>(
        std::distance(state.candidates.begin(), candidate));
    if (capturedCandidateSeen[candidateIndex]) {
      return BattleStateCodecErrorCode::DuplicateId;
    }
    capturedCandidateSeen[candidateIndex] = true;
  }

  for (std::size_t index = 0U; index < state.participants.size(); ++index) {
    const auto &entry = state.participants[index];
    if (!validParticipantSlot(entry.slot, state.capturedParticipants.size()) ||
        entry.sessionId.value() == 0U || !validExitStatus(entry.exitStatus)) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    if (entry.slot != static_cast<ParticipantSlot>(index + 1U)) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    if (index != 0U) {
      if (duplicateParticipantSlot(state.participants, index)) {
        return BattleStateCodecErrorCode::DuplicateId;
      }
      if (requireCanonicalOrder &&
          entry.slot < state.participants[index - 1U].slot) {
        return BattleStateCodecErrorCode::InvalidOrdering;
      }
    }
    if (state.capturedParticipants[index].sessionId != entry.sessionId) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    const auto withinElapsed = [&state](const auto &value) noexcept {
      return !value.has_value() ||
             *value <= state.battleTime.battleElapsedNanos;
    };
    if (!withinElapsed(entry.lastRateUpdateNanos) ||
        !withinElapsed(entry.lastAttackRateUpdateNanos) ||
        !withinElapsed(entry.lastClaimRateUpdateNanos) ||
        !withinElapsed(entry.lastAcceptedAttackNanos)) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    if (const auto error = validateAttackStore(
            entry.attackResults, state.battleId,
            state.battleTime.battleElapsedNanos, requireCanonicalOrder);
        error.has_value()) {
      return error;
    }
    if (const auto error = validateLootStore(
            entry.lootResults, state.battleId,
            state.battleTime.battleElapsedNanos, requireCanonicalOrder);
        error.has_value()) {
      return error;
    }
  }
  if (state.state == BattleLoadState::GameplayCommitted) {
    if (state.capturedParticipants.size() != readyCandidateCount ||
        state.capturedParticipants.size() != state.participants.size() ||
        state.participants.empty()) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    for (std::size_t index = 0U; index < state.candidates.size(); ++index) {
      if (state.candidates[index].state == LoadCandidateState::Ready &&
          !capturedCandidateSeen[index]) {
        return BattleStateCodecErrorCode::InvariantViolation;
      }
    }
  } else if (!state.capturedParticipants.empty()) {
    return BattleStateCodecErrorCode::InvariantViolation;
  } else if (!state.participants.empty()) {
    return BattleStateCodecErrorCode::InvariantViolation;
  }
  if (state.state == BattleLoadState::LoadBarrierOpen &&
      !state.loadDeadlineTick.has_value()) {
    return BattleStateCodecErrorCode::InvariantViolation;
  }
  if (state.loadDeadlineTick.has_value() !=
      (state.state == BattleLoadState::LoadBarrierOpen)) {
    return BattleStateCodecErrorCode::InvariantViolation;
  }

  if (state.monster.has_value()) {
    const auto &monster = *state.monster;
    if (monster.id != battle::CombatRuleset::monsterId ||
        !validMonsterState(monster.state) ||
        (monster.state == MonsterState::Alive && monster.hitPoints == 0U)) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
  }
  if ((state.state == BattleLoadState::GameplayCommitted) !=
      state.monster.has_value()) {
    return BattleStateCodecErrorCode::InvariantViolation;
  }

  if (state.combatTerminal.has_value()) {
    const auto &terminal = *state.combatTerminal;
    if (terminal.battleId != state.battleId ||
        terminal.monsterId != battle::CombatRuleset::monsterId ||
        !validCombatOutcome(terminal.outcome) ||
        terminal.outcome == CombatOutcome::None ||
        !validResultRuleset(terminal.rulesetVersion)) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
  }
  if (state.combatTerminal.has_value()) {
    if (!state.monster.has_value()) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    const auto outcome = state.combatTerminal->outcome;
    if ((outcome == CombatOutcome::MonsterDefeated &&
         state.monster->state != MonsterState::Dead) ||
        (outcome == CombatOutcome::CombatTimeout &&
         state.monster->state != MonsterState::TimedOut)) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
  } else if (state.state == BattleLoadState::GameplayCommitted &&
             state.monster->state != MonsterState::Alive) {
    return BattleStateCodecErrorCode::InvariantViolation;
  }
  if (state.battleTerminalOutcome.has_value() &&
      !validBattleOutcome(*state.battleTerminalOutcome)) {
    return BattleStateCodecErrorCode::InvalidEnum;
  }
  if (state.battleTerminalOutcome.has_value()) {
    const auto outcome = *state.battleTerminalOutcome;
    if ((outcome == BattleOutcome::CancelledNoActiveParticipants &&
         state.combatTerminal.has_value()) ||
        (outcome == BattleOutcome::MonsterDefeated &&
         (!state.combatTerminal.has_value() ||
          state.combatTerminal->outcome != CombatOutcome::MonsterDefeated)) ||
        (outcome == BattleOutcome::CombatTimeout &&
         (!state.combatTerminal.has_value() ||
          state.combatTerminal->outcome != CombatOutcome::CombatTimeout))) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    if (state.resultState != BattleResultState::Committed) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
  }

  for (std::size_t index = 0U; index < state.drops.size(); ++index) {
    const auto &entry = state.drops[index];
    if (entry.drop.dropId.value == 0U || entry.drop.itemId.value == 0U ||
        entry.drop.quantity == 0U || !validLootDropState(entry.state)) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    if (index != 0U) {
      if (entry.drop.dropId == state.drops[index - 1U].drop.dropId) {
        return BattleStateCodecErrorCode::DuplicateId;
      }
      if (requireCanonicalOrder &&
          entry.drop.dropId.value < state.drops[index - 1U].drop.dropId.value) {
        return BattleStateCodecErrorCode::InvalidOrdering;
      }
    }
    if ((entry.state == LootDropState::Claimed) !=
        entry.ownerSlot.has_value()) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    if (entry.state != LootDropState::Claimed && entry.ownerSlot.has_value()) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    if (entry.ownerSlot.has_value() &&
        (!validParticipantSlot(*entry.ownerSlot,
                               state.capturedParticipants.size()) ||
         !capturedHasSlot(state.capturedParticipants, *entry.ownerSlot))) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
  }

  for (std::size_t index = 0U; index < state.holdings.size(); ++index) {
    const auto &entry = state.holdings[index];
    if (!validParticipantSlot(entry.ownerSlot,
                              state.capturedParticipants.size()) ||
        !capturedHasSlot(state.capturedParticipants, entry.ownerSlot) ||
        entry.itemId.value == 0U || entry.quantity == 0U) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    if (index != 0U) {
      const auto &previous = state.holdings[index - 1U];
      if (entry.ownerSlot == previous.ownerSlot &&
          entry.itemId == previous.itemId) {
        return BattleStateCodecErrorCode::DuplicateId;
      }
      if (requireCanonicalOrder &&
          (entry.ownerSlot < previous.ownerSlot ||
           (entry.ownerSlot == previous.ownerSlot &&
            entry.itemId.value < previous.itemId.value))) {
        return BattleStateCodecErrorCode::InvalidOrdering;
      }
    }
  }

  if (state.lootResolution == LootResolutionState::NotStarted) {
    if (!state.drops.empty()) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
  } else {
    if (state.drops.size() != state.participants.size()) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    const auto hasAvailable = std::ranges::any_of(
        state.drops, [](const BattleDeterministicDropState &drop) {
          return drop.state == LootDropState::Available;
        });
    if ((state.lootResolution == LootResolutionState::Open) != hasAvailable) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
  }
  if (state.lootResolution == LootResolutionState::Open &&
      (!state.combatTerminal.has_value() ||
       state.combatTerminal->outcome != CombatOutcome::MonsterDefeated)) {
    return BattleStateCodecErrorCode::InvariantViolation;
  }
  if (state.lootResolution == LootResolutionState::Resolved &&
      (!state.combatTerminal.has_value() &&
       state.battleTerminalOutcome !=
           std::optional<BattleOutcome>{
               BattleOutcome::CancelledNoActiveParticipants})) {
    return BattleStateCodecErrorCode::InvariantViolation;
  }
  if (state.lootResolution == LootResolutionState::Resolved &&
      state.combatTerminal.has_value() &&
      state.combatTerminal->outcome != CombatOutcome::MonsterDefeated) {
    return BattleStateCodecErrorCode::InvariantViolation;
  }
  if (state.state == BattleLoadState::GameplayCommitted &&
      state.resultState == BattleResultState::NotReady) {
    const auto combatDeadlineExpected = !state.combatTerminal.has_value();
    if (state.combatDeadlineTick.has_value() != combatDeadlineExpected) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
    const auto lootDeadlineExpected =
        state.lootResolution == LootResolutionState::Open;
    if (state.lootDeadlineTick.has_value() != lootDeadlineExpected) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
  } else if (state.combatDeadlineTick.has_value() ||
             state.lootDeadlineTick.has_value()) {
    return BattleStateCodecErrorCode::InvariantViolation;
  }

  if (state.resultState == BattleResultState::Committed) {
    if (!state.committedResult.has_value() ||
        !validBattleOutcome(state.committedResult->outcome) ||
        !state.battleTerminalOutcome.has_value() ||
        *state.battleTerminalOutcome != state.committedResult->outcome ||
        state.committedResult->entries.size() !=
            state.capturedParticipants.size()) {
      return BattleStateCodecErrorCode::InvariantViolation;
    }
  } else if (state.committedResult.has_value()) {
    return BattleStateCodecErrorCode::InvariantViolation;
  }
  if (state.committedResult.has_value()) {
    const auto &entries = state.committedResult->entries;
    if (const auto error = validateFinalResultBasic(*state.committedResult,
                                                    requireCanonicalOrder);
        error.has_value()) {
      return error;
    }
    for (std::size_t index = 0U; index < entries.size(); ++index) {
      const auto &entry = entries[index];
      if (!validParticipantSlot(entry.slot,
                                state.capturedParticipants.size()) ||
          !capturedHasSlot(state.capturedParticipants, entry.slot)) {
        return BattleStateCodecErrorCode::InvariantViolation;
      }
    }
  }
  if (state.lootResultsCompletedAtNanos.has_value() &&
      *state.lootResultsCompletedAtNanos >
          state.battleTime.battleElapsedNanos) {
    return BattleStateCodecErrorCode::InvariantViolation;
  }
  return std::nullopt;
}

BattleDeterministicState canonicalState(const BattleDeterministicState &input) {
  BattleDeterministicState state = input;
  std::ranges::sort(state.candidates, [](const auto &lhs, const auto &rhs) {
    return lhs.slot < rhs.slot;
  });
  std::ranges::sort(
      state.capturedParticipants,
      [](const auto &lhs, const auto &rhs) { return lhs.slot < rhs.slot; });
  std::ranges::sort(state.participants, [](const auto &lhs, const auto &rhs) {
    return lhs.slot < rhs.slot;
  });
  std::ranges::sort(state.drops, [](const auto &lhs, const auto &rhs) {
    return lhs.drop.dropId.value < rhs.drop.dropId.value;
  });
  std::ranges::sort(state.holdings, [](const auto &lhs, const auto &rhs) {
    return lhs.ownerSlot < rhs.ownerSlot ||
           (lhs.ownerSlot == rhs.ownerSlot &&
            lhs.itemId.value < rhs.itemId.value);
  });
  if (state.committedResult.has_value()) {
    std::ranges::sort(
        state.committedResult->entries,
        [](const auto &lhs, const auto &rhs) { return lhs.slot < rhs.slot; });
  }
  for (auto &participant : state.participants) {
    std::ranges::sort(participant.attackResults.records,
                      [](const auto &lhs, const auto &rhs) {
                        return commandIdLess(lhs.commandId, rhs.commandId);
                      });
    std::ranges::sort(participant.lootResults.records,
                      [](const auto &lhs, const auto &rhs) {
                        return commandIdLess(lhs.commandId, rhs.commandId);
                      });
  }
  return state;
}

void putAttackResult(Writer &writer, const AttackTerminalResult &result) {
  putCommandId(writer, result.commandId);
  writer.u64(result.battleId.value());
  writer.u16(static_cast<std::uint16_t>(result.code));
  writer.u64(result.monsterId);
  writer.u32(result.remainingHitPoints);
  writer.u16(result.rulesetVersion);
  writer.u8(static_cast<std::uint8_t>(result.outcome));
}

bool getAttackResult(Reader &reader, AttackTerminalResult &result) {
  std::uint64_t battleId = 0U;
  std::uint16_t code = 0U;
  std::uint8_t outcome = 0U;
  return getCommandId(reader, result.commandId) && reader.u64(battleId) &&
         reader.u16(code) && reader.u64(result.monsterId) &&
         reader.u32(result.remainingHitPoints) &&
         reader.u16(result.rulesetVersion) && reader.u8(outcome) &&
         (result.battleId = shared::BattleInstanceId{battleId},
          result.code = static_cast<AttackResultCode>(code),
          result.outcome = static_cast<CombatOutcome>(outcome), true);
}

void putLootResult(Writer &writer, const ClaimLootTerminalResult &result) {
  putCommandId(writer, result.commandId);
  writer.u64(result.battleId.value());
  writer.u64(result.dropId.value);
  writer.u16(static_cast<std::uint16_t>(result.code));
}

bool getLootResult(Reader &reader, ClaimLootTerminalResult &result) {
  std::uint64_t battleId = 0U;
  std::uint64_t dropId = 0U;
  std::uint16_t code = 0U;
  return getCommandId(reader, result.commandId) && reader.u64(battleId) &&
         reader.u64(dropId) && reader.u16(code) &&
         (result.battleId = shared::BattleInstanceId{battleId},
          result.dropId = DropId{dropId},
          result.code = static_cast<ClaimLootResultCode>(code), true);
}

void putAttackStore(Writer &writer, const AttackResultStoreState &store) {
  putOptionalU64(writer, store.battleCompletedAtNanos);
  writer.u16(static_cast<std::uint16_t>(store.records.size()));
  for (const auto &entry : store.records) {
    putCommandId(writer, entry.commandId);
    writer.u64(entry.targetHint);
    putAttackResult(writer, entry.result);
  }
}

bool getAttackStore(Reader &reader, AttackResultStoreState &store) {
  std::uint16_t count = 0U;
  if (!reader.optionalU64(store.battleCompletedAtNanos) || !reader.u16(count) ||
      count > kMaximumBattleResultRecords) {
    if (!reader.error.has_value() && count > kMaximumBattleResultRecords) {
      reader.fail(BattleStateCodecErrorCode::InvalidLength,
                  reader.position - sizeof(count));
    }
    return false;
  }
  store.records.reserve(count);
  for (std::uint16_t index = 0U; index < count; ++index) {
    AttackResultRecordState entry{
        .commandId = battle::CommandId{.high = 0U, .low = 0U},
        .targetHint = 0U,
        .result = AttackTerminalResult{
            .commandId = battle::CommandId{.high = 0U, .low = 0U},
            .battleId = shared::BattleInstanceId{0U},
            .code = AttackResultCode::Ok,
            .monsterId = 0U,
            .remainingHitPoints = 0U,
            .rulesetVersion = 0U,
            .outcome = CombatOutcome::None,
        }};
    if (!getCommandId(reader, entry.commandId) ||
        !reader.u64(entry.targetHint) ||
        !getAttackResult(reader, entry.result)) {
      return false;
    }
    store.records.push_back(std::move(entry));
  }
  return true;
}

void putLootStore(Writer &writer, const LootResultStoreState &store) {
  putOptionalU64(writer, store.battleCompletedAtNanos);
  writer.u16(static_cast<std::uint16_t>(store.records.size()));
  for (const auto &entry : store.records) {
    putCommandId(writer, entry.commandId);
    writer.u64(entry.dropId.value);
    putLootResult(writer, entry.result);
  }
}

void putCanonicalResult(Writer &writer,
                        const BattleDeterministicFinalResult &result) {
  writer.u8(static_cast<std::uint8_t>(result.outcome));
  writer.u16(static_cast<std::uint16_t>(result.entries.size()));
  for (const auto &entry : result.entries) {
    writer.u16(entry.slot);
    writer.u8(static_cast<std::uint8_t>(entry.exitStatus));
    writer.u64(entry.finalAssetValue);
    putOptionalU32(writer, entry.rank);
    writer.u8(entry.isTop ? 1U : 0U);
  }
}

std::optional<BattleStateCodecError>
validateCanonicalResultPayload(ByteView payload,
                               std::vector<ParticipantSlot> &resultSlots) {
  Reader reader{.view = payload, .position = 0U, .error = std::nullopt};
  std::uint8_t rawOutcome = 0U;
  std::uint16_t count = 0U;
  if (!reader.u8(rawOutcome) || !reader.u16(count)) {
    return reader.error.value_or(
        BattleStateCodecError{.code = BattleStateCodecErrorCode::Truncated,
                              .offset = reader.position});
  }
  if (!validBattleOutcome(static_cast<BattleOutcome>(rawOutcome))) {
    return BattleStateCodecError{.code = BattleStateCodecErrorCode::InvalidEnum,
                                 .offset = 0U};
  }
  if (count < 2U || count > kMaximumBattleParticipants) {
    return BattleStateCodecError{.code =
                                     BattleStateCodecErrorCode::InvalidLength,
                                 .offset = reader.position - sizeof(count)};
  }

  resultSlots.reserve(count);
  for (std::uint16_t index = 0U; index < count; ++index) {
    std::uint16_t slot = 0U;
    std::uint8_t rawExit = 0U;
    std::uint64_t finalAssetValue = 0U;
    std::optional<std::uint32_t> rank;
    bool isTop = false;
    if (!reader.u16(slot) || !reader.u8(rawExit) ||
        !reader.u64(finalAssetValue) || !reader.optionalU32(rank) ||
        !reader.boolean(isTop)) {
      return reader.error.value_or(
          BattleStateCodecError{.code = BattleStateCodecErrorCode::Truncated,
                                .offset = reader.position});
    }
    static_cast<void>(finalAssetValue);
    static_cast<void>(isTop);
    if (slot == battle::systemParticipantSlot ||
        static_cast<std::size_t>(slot) > kMaximumBattleParticipants ||
        !validExitStatus(static_cast<ParticipantExitStatus>(rawExit)) ||
        (rank.has_value() && *rank == 0U)) {
      return BattleStateCodecError{
          .code = BattleStateCodecErrorCode::InvariantViolation, .offset = 0U};
    }
    if (!resultSlots.empty()) {
      if (slot == resultSlots.back()) {
        return BattleStateCodecError{
            .code = BattleStateCodecErrorCode::DuplicateId, .offset = 0U};
      }
      if (slot < resultSlots.back()) {
        return BattleStateCodecError{
            .code = BattleStateCodecErrorCode::InvalidOrdering, .offset = 0U};
      }
    }
    resultSlots.push_back(slot);
  }
  if (reader.position != payload.size()) {
    return BattleStateCodecError{.code =
                                     BattleStateCodecErrorCode::TrailingBytes,
                                 .offset = reader.position};
  }
  return std::nullopt;
}

std::optional<BattleStateCodecError>
validateTerminalSettlements(const TerminalReceiptPayload &receipt) {
  if (receipt.settlementIntentCount != receipt.settlements.size() ||
      receipt.settlements.size() > std::numeric_limits<std::uint16_t>::max()) {
    return BattleStateCodecError{
        .code = BattleStateCodecErrorCode::InvalidLength, .offset = 0U};
  }
  std::uint16_t previousSlot = 0U;
  for (std::size_t index = 0U; index < receipt.settlements.size(); ++index) {
    const auto &settlement = receipt.settlements[index];
    if (settlement.participantSlot == 0U) {
      return BattleStateCodecError{
          .code = BattleStateCodecErrorCode::InvariantViolation, .offset = 0U};
    }
    if (index != 0U) {
      if (settlement.participantSlot == previousSlot) {
        return BattleStateCodecError{
            .code = BattleStateCodecErrorCode::DuplicateId, .offset = 0U};
      }
      if (settlement.participantSlot < previousSlot) {
        return BattleStateCodecError{
            .code = BattleStateCodecErrorCode::InvalidOrdering, .offset = 0U};
      }
    }
    for (std::size_t prior = 0U; prior < index; ++prior) {
      if (settlement.settlementId == receipt.settlements[prior].settlementId) {
        return BattleStateCodecError{
            .code = BattleStateCodecErrorCode::DuplicateId, .offset = 0U};
      }
    }
    previousSlot = settlement.participantSlot;
  }
  return std::nullopt;
}

bool getLootStore(Reader &reader, LootResultStoreState &store) {
  std::uint16_t count = 0U;
  if (!reader.optionalU64(store.battleCompletedAtNanos) || !reader.u16(count) ||
      count > kMaximumBattleResultRecords) {
    if (!reader.error.has_value() && count > kMaximumBattleResultRecords) {
      reader.fail(BattleStateCodecErrorCode::InvalidLength,
                  reader.position - sizeof(count));
    }
    return false;
  }
  store.records.reserve(count);
  for (std::uint16_t index = 0U; index < count; ++index) {
    LootResultRecordState entry{
        .commandId = battle::CommandId{.high = 0U, .low = 0U},
        .dropId = DropId{0U},
        .result = ClaimLootTerminalResult{
            .commandId = battle::CommandId{.high = 0U, .low = 0U},
            .battleId = shared::BattleInstanceId{0U},
            .dropId = DropId{0U},
            .code = ClaimLootResultCode::Ok,
        }};
    if (!getCommandId(reader, entry.commandId) ||
        !reader.u64(entry.dropId.value) ||
        !getLootResult(reader, entry.result)) {
      return false;
    }
    store.records.push_back(std::move(entry));
  }
  return true;
}

void putState(Writer &writer, const BattleDeterministicState &state) {
  writer.u32(kBattleStateMagic);
  writer.u16(kBattleStateSchemaVersion);
  writer.u64(state.roomId.value());
  writer.u64(state.battleId.value());
  writer.u32(state.rulesetVersion);
  writer.u64(state.seed);
  writer.u64(state.battleTime.logicalTick);
  writer.u64(state.battleTime.battleElapsedNanos);
  writer.u8(static_cast<std::uint8_t>(state.state));

  writer.u16(static_cast<std::uint16_t>(state.candidates.size()));
  for (const auto &entry : state.candidates) {
    writer.u16(entry.slot);
    writer.u64(entry.sessionId.value());
    writer.u8(static_cast<std::uint8_t>(entry.state));
  }
  writer.u16(static_cast<std::uint16_t>(state.capturedParticipants.size()));
  for (const auto &entry : state.capturedParticipants) {
    writer.u16(entry.slot);
    writer.u64(entry.sessionId.value());
    writer.u8(static_cast<std::uint8_t>(entry.exitStatus));
  }
  writer.u16(static_cast<std::uint16_t>(state.participants.size()));
  for (const auto &entry : state.participants) {
    writer.u16(entry.slot);
    writer.u64(entry.sessionId.value());
    writer.i32(entry.posXMillimeter);
    writer.i32(entry.posYMillimeter);
    writer.i32(entry.tickDeltaXMillimeter);
    writer.i32(entry.tickDeltaYMillimeter);
    putOptionalU32(writer, entry.latestSeenActionSequence);
    putOptionalU64(writer, entry.lastRateUpdateNanos);
    writer.u64(entry.rateCreditUnits);
    putOptionalU64(writer, entry.lastAttackRateUpdateNanos);
    writer.u64(entry.attackRateCreditUnits);
    putOptionalU64(writer, entry.lastClaimRateUpdateNanos);
    writer.u64(entry.claimRateCreditUnits);
    putOptionalU64(writer, entry.lastAcceptedAttackNanos);
    putAttackStore(writer, entry.attackResults);
    putLootStore(writer, entry.lootResults);
    writer.u8(entry.gameplayEligible ? 1U : 0U);
    writer.u8(entry.inputEnabled ? 1U : 0U);
    writer.u8(static_cast<std::uint8_t>(entry.exitStatus));
  }

  putOptionalU64(writer, state.loadDeadlineTick);
  putOptionalU64(writer, state.combatDeadlineTick);
  putOptionalU64(writer, state.lootDeadlineTick);
  if (state.lastIntegratedServerTick.has_value()) {
    writer.u8(1U);
    writer.u32(*state.lastIntegratedServerTick);
  } else {
    writer.u8(0U);
  }
  writer.u32(state.nextSnapshotSequence);
  writer.u32(state.nextAttackAppliedSequence);

  writer.u8(state.monster.has_value() ? 1U : 0U);
  if (state.monster.has_value()) {
    const auto &monster = *state.monster;
    writer.u64(monster.id);
    writer.i32(monster.position.xMillimeter);
    writer.i32(monster.position.yMillimeter);
    writer.u32(monster.hitPoints);
    writer.u8(static_cast<std::uint8_t>(monster.state));
  }
  writer.u8(state.combatTerminal.has_value() ? 1U : 0U);
  if (state.combatTerminal.has_value()) {
    const auto &terminal = *state.combatTerminal;
    putEventId(writer, terminal.eventId);
    writer.u64(terminal.battleId.value());
    writer.u32(terminal.eventSequence);
    writer.u8(static_cast<std::uint8_t>(terminal.outcome));
    writer.u64(terminal.monsterId);
    writer.u32(terminal.serverTick);
    writer.u16(terminal.rulesetVersion);
  }
  writer.u8(state.battleTerminalOutcome.has_value() ? 1U : 0U);
  if (state.battleTerminalOutcome.has_value()) {
    writer.u8(static_cast<std::uint8_t>(*state.battleTerminalOutcome));
  }

  writer.u16(static_cast<std::uint16_t>(state.drops.size()));
  for (const auto &entry : state.drops) {
    writer.u64(entry.drop.dropId.value);
    writer.u64(entry.drop.itemId.value);
    writer.u64(entry.drop.quantity);
    writer.i32(entry.drop.position.xMillimeter);
    writer.i32(entry.drop.position.yMillimeter);
    writer.u8(static_cast<std::uint8_t>(entry.state));
    putOptionalSlot(writer, entry.ownerSlot);
  }
  writer.u16(static_cast<std::uint16_t>(state.holdings.size()));
  for (const auto &entry : state.holdings) {
    writer.u16(entry.ownerSlot);
    writer.u64(entry.itemId.value);
    writer.u64(entry.quantity);
  }
  writer.u8(static_cast<std::uint8_t>(state.lootResolution));
  writer.u8(static_cast<std::uint8_t>(state.resultState));
  writer.u8(state.committedResult.has_value() ? 1U : 0U);
  if (state.committedResult.has_value()) {
    putCanonicalResult(writer, *state.committedResult);
  }
  putOptionalU64(writer, state.lootResultsCompletedAtNanos);
}

bool getState(Reader &reader, BattleDeterministicState &state) {
  std::uint64_t roomId = 0U;
  std::uint64_t battleId = 0U;
  std::uint8_t rawState = 0U;
  if (!reader.u64(roomId) || !reader.u64(battleId) ||
      !reader.u32(state.rulesetVersion) || !reader.u64(state.seed) ||
      !reader.u64(state.battleTime.logicalTick) ||
      !reader.u64(state.battleTime.battleElapsedNanos) ||
      !reader.u8(rawState)) {
    return false;
  }
  state.roomId = shared::RoomId{roomId};
  state.battleId = shared::BattleInstanceId{battleId};
  state.state = static_cast<BattleLoadState>(rawState);

  std::uint16_t count = 0U;
  if (!reader.u16(count) || count < 2U || count > kMaximumBattleParticipants) {
    if (!reader.error.has_value()) {
      reader.fail(BattleStateCodecErrorCode::InvalidLength,
                  reader.position - sizeof(count));
    }
    return false;
  }
  state.candidates.reserve(count);
  for (std::uint16_t index = 0U; index < count; ++index) {
    BattleDeterministicCandidateState entry{
        .slot = 0U,
        .sessionId = shared::SessionId{0U},
        .state = LoadCandidateState::PendingLoad,
    };
    std::uint64_t sessionId = 0U;
    std::uint8_t rawCandidateState = 0U;
    if (!reader.u16(entry.slot) || !reader.u64(sessionId) ||
        !reader.u8(rawCandidateState)) {
      return false;
    }
    entry.sessionId = shared::SessionId{sessionId};
    entry.state = static_cast<LoadCandidateState>(rawCandidateState);
    state.candidates.push_back(std::move(entry));
  }

  if (!reader.u16(count) || count > kMaximumBattleParticipants) {
    if (!reader.error.has_value()) {
      reader.fail(BattleStateCodecErrorCode::InvalidLength,
                  reader.position - sizeof(count));
    }
    return false;
  }
  state.capturedParticipants.reserve(count);
  for (std::uint16_t index = 0U; index < count; ++index) {
    BattleDeterministicCapturedParticipantState entry{
        .slot = 0U,
        .sessionId = shared::SessionId{0U},
        .exitStatus = ParticipantExitStatus::GameplayEligible,
    };
    std::uint64_t sessionId = 0U;
    std::uint8_t rawExit = 0U;
    if (!reader.u16(entry.slot) || !reader.u64(sessionId) ||
        !reader.u8(rawExit)) {
      return false;
    }
    entry.sessionId = shared::SessionId{sessionId};
    entry.exitStatus = static_cast<ParticipantExitStatus>(rawExit);
    state.capturedParticipants.push_back(std::move(entry));
  }

  if (!reader.u16(count) || count > kMaximumBattleParticipants) {
    if (!reader.error.has_value()) {
      reader.fail(BattleStateCodecErrorCode::InvalidLength,
                  reader.position - sizeof(count));
    }
    return false;
  }
  state.participants.reserve(count);
  for (std::uint16_t index = 0U; index < count; ++index) {
    BattleDeterministicParticipantState entry{
        .slot = 0U,
        .sessionId = shared::SessionId{0U},
        .posXMillimeter = 0,
        .posYMillimeter = 0,
        .tickDeltaXMillimeter = 0,
        .tickDeltaYMillimeter = 0,
        .latestSeenActionSequence = std::nullopt,
        .lastRateUpdateNanos = std::nullopt,
        .rateCreditUnits = 0U,
        .lastAttackRateUpdateNanos = std::nullopt,
        .attackRateCreditUnits = 0U,
        .lastClaimRateUpdateNanos = std::nullopt,
        .claimRateCreditUnits = 0U,
        .lastAcceptedAttackNanos = std::nullopt,
        .attackResults =
            AttackResultStoreState{.battleCompletedAtNanos = std::nullopt,
                                   .records = {}},
        .lootResults =
            LootResultStoreState{.battleCompletedAtNanos = std::nullopt,
                                 .records = {}},
        .gameplayEligible = false,
        .inputEnabled = false,
        .exitStatus = ParticipantExitStatus::GameplayEligible,
    };
    std::uint64_t sessionId = 0U;
    std::uint8_t rawExit = 0U;
    if (!reader.u16(entry.slot) || !reader.u64(sessionId) ||
        !reader.i32(entry.posXMillimeter) ||
        !reader.i32(entry.posYMillimeter) ||
        !reader.i32(entry.tickDeltaXMillimeter) ||
        !reader.i32(entry.tickDeltaYMillimeter) ||
        !reader.optionalU32(entry.latestSeenActionSequence) ||
        !reader.optionalU64(entry.lastRateUpdateNanos) ||
        !reader.u64(entry.rateCreditUnits) ||
        !reader.optionalU64(entry.lastAttackRateUpdateNanos) ||
        !reader.u64(entry.attackRateCreditUnits) ||
        !reader.optionalU64(entry.lastClaimRateUpdateNanos) ||
        !reader.u64(entry.claimRateCreditUnits) ||
        !reader.optionalU64(entry.lastAcceptedAttackNanos) ||
        !getAttackStore(reader, entry.attackResults) ||
        !getLootStore(reader, entry.lootResults) ||
        !reader.boolean(entry.gameplayEligible) ||
        !reader.boolean(entry.inputEnabled) || !reader.u8(rawExit)) {
      return false;
    }
    entry.sessionId = shared::SessionId{sessionId};
    entry.exitStatus = static_cast<ParticipantExitStatus>(rawExit);
    state.participants.push_back(std::move(entry));
  }

  if (!reader.optionalU64(state.loadDeadlineTick) ||
      !reader.optionalU64(state.combatDeadlineTick) ||
      !reader.optionalU64(state.lootDeadlineTick)) {
    return false;
  }
  std::uint8_t hasServerTick = 0U;
  if (!reader.u8(hasServerTick)) {
    return false;
  }
  if (hasServerTick > 1U) {
    reader.fail(BattleStateCodecErrorCode::InvalidBoolean,
                reader.position - 1U);
    return false;
  }
  if (hasServerTick == 1U) {
    std::uint32_t serverTick = 0U;
    if (!reader.u32(serverTick)) {
      return false;
    }
    state.lastIntegratedServerTick = serverTick;
  } else {
    state.lastIntegratedServerTick.reset();
  }
  if (!reader.u32(state.nextSnapshotSequence) ||
      !reader.u32(state.nextAttackAppliedSequence)) {
    return false;
  }

  std::uint8_t hasMonster = 0U;
  if (!reader.u8(hasMonster)) {
    return false;
  }
  if (hasMonster > 1U) {
    reader.fail(BattleStateCodecErrorCode::InvalidBoolean,
                reader.position - 1U);
    return false;
  }
  if (hasMonster == 1U) {
    BattleDeterministicMonsterState monster{
        .id = 0U,
        .position = battle::CombatPosition{.xMillimeter = 0, .yMillimeter = 0},
        .hitPoints = 0U,
        .state = MonsterState::Alive,
    };
    if (!reader.u64(monster.id) || !reader.i32(monster.position.xMillimeter) ||
        !reader.i32(monster.position.yMillimeter) ||
        !reader.u32(monster.hitPoints)) {
      return false;
    }
    std::uint8_t rawMonsterState = 0U;
    if (!reader.u8(rawMonsterState)) {
      return false;
    }
    monster.state = static_cast<MonsterState>(rawMonsterState);
    state.monster = monster;
  } else {
    state.monster.reset();
  }

  std::uint8_t hasCombatTerminal = 0U;
  if (!reader.u8(hasCombatTerminal)) {
    return false;
  }
  if (hasCombatTerminal > 1U) {
    reader.fail(BattleStateCodecErrorCode::InvalidBoolean,
                reader.position - 1U);
    return false;
  }
  if (hasCombatTerminal == 1U) {
    CombatTerminalRecord terminal{
        .eventId = EventId{.high = 0U, .low = 0U},
        .battleId = shared::BattleInstanceId{0U},
        .eventSequence = 0U,
        .outcome = CombatOutcome::None,
        .monsterId = 0U,
        .serverTick = 0U,
        .rulesetVersion = 0U,
    };
    std::uint64_t terminalBattleId = 0U;
    std::uint8_t rawOutcome = 0U;
    if (!getEventId(reader, terminal.eventId) ||
        !reader.u64(terminalBattleId) || !reader.u32(terminal.eventSequence) ||
        !reader.u8(rawOutcome) || !reader.u64(terminal.monsterId) ||
        !reader.u32(terminal.serverTick) ||
        !reader.u16(terminal.rulesetVersion)) {
      return false;
    }
    terminal.battleId = shared::BattleInstanceId{terminalBattleId};
    terminal.outcome = static_cast<CombatOutcome>(rawOutcome);
    state.combatTerminal = terminal;
  } else {
    state.combatTerminal.reset();
  }

  std::uint8_t hasBattleTerminalOutcome = 0U;
  if (!reader.u8(hasBattleTerminalOutcome)) {
    return false;
  }
  if (hasBattleTerminalOutcome > 1U) {
    reader.fail(BattleStateCodecErrorCode::InvalidBoolean,
                reader.position - 1U);
    return false;
  }
  if (hasBattleTerminalOutcome == 1U) {
    std::uint8_t rawOutcome = 0U;
    if (!reader.u8(rawOutcome)) {
      return false;
    }
    state.battleTerminalOutcome = static_cast<BattleOutcome>(rawOutcome);
  } else {
    state.battleTerminalOutcome.reset();
  }

  if (!reader.u16(count) || count > kMaximumBattleDrops) {
    if (!reader.error.has_value()) {
      reader.fail(BattleStateCodecErrorCode::InvalidLength,
                  reader.position - sizeof(count));
    }
    return false;
  }
  state.drops.reserve(count);
  for (std::uint16_t index = 0U; index < count; ++index) {
    BattleDeterministicDropState entry{
        .drop =
            battle::RelicDrop{
                .dropId = DropId{0U},
                .itemId = battle::ItemId{0U},
                .quantity = 0U,
                .position =
                    battle::DropPosition{.xMillimeter = 0, .yMillimeter = 0},
            },
        .state = LootDropState::Available,
        .ownerSlot = std::nullopt,
    };
    std::uint8_t rawDropState = 0U;
    if (!reader.u64(entry.drop.dropId.value) ||
        !reader.u64(entry.drop.itemId.value) ||
        !reader.u64(entry.drop.quantity) ||
        !reader.i32(entry.drop.position.xMillimeter) ||
        !reader.i32(entry.drop.position.yMillimeter) ||
        !reader.u8(rawDropState) || !reader.optionalSlot(entry.ownerSlot)) {
      return false;
    }
    entry.state = static_cast<LootDropState>(rawDropState);
    state.drops.push_back(std::move(entry));
  }
  if (!reader.u16(count) || count > kMaximumBattleHoldings) {
    if (!reader.error.has_value()) {
      reader.fail(BattleStateCodecErrorCode::InvalidLength,
                  reader.position - sizeof(count));
    }
    return false;
  }
  state.holdings.reserve(count);
  for (std::uint16_t index = 0U; index < count; ++index) {
    BattleDeterministicHoldingState entry{};
    if (!reader.u16(entry.ownerSlot) || !reader.u64(entry.itemId.value) ||
        !reader.u64(entry.quantity)) {
      return false;
    }
    state.holdings.push_back(std::move(entry));
  }

  std::uint8_t rawLootResolution = 0U;
  std::uint8_t rawResultState = 0U;
  if (!reader.u8(rawLootResolution) || !reader.u8(rawResultState)) {
    return false;
  }
  state.lootResolution = static_cast<LootResolutionState>(rawLootResolution);
  state.resultState = static_cast<BattleResultState>(rawResultState);

  std::uint8_t hasCommittedResult = 0U;
  if (!reader.u8(hasCommittedResult)) {
    return false;
  }
  if (hasCommittedResult > 1U) {
    reader.fail(BattleStateCodecErrorCode::InvalidBoolean,
                reader.position - 1U);
    return false;
  }
  if (hasCommittedResult == 1U) {
    BattleDeterministicFinalResult result{};
    std::uint8_t rawOutcome = 0U;
    if (!reader.u8(rawOutcome) || !reader.u16(count) ||
        count > kMaximumBattleParticipants) {
      if (!reader.error.has_value() && count > kMaximumBattleParticipants) {
        reader.fail(BattleStateCodecErrorCode::InvalidLength,
                    reader.position - sizeof(count));
      }
      return false;
    }
    result.outcome = static_cast<BattleOutcome>(rawOutcome);
    result.entries.reserve(count);
    for (std::uint16_t index = 0U; index < count; ++index) {
      BattleDeterministicResultEntry entry{};
      std::uint8_t rawExit = 0U;
      if (!reader.u16(entry.slot) || !reader.u8(rawExit) ||
          !reader.u64(entry.finalAssetValue) ||
          !reader.optionalU32(entry.rank) || !reader.boolean(entry.isTop)) {
        return false;
      }
      entry.exitStatus = static_cast<ParticipantExitStatus>(rawExit);
      result.entries.push_back(std::move(entry));
    }
    state.committedResult = std::move(result);
  } else {
    state.committedResult.reset();
  }
  return reader.optionalU64(state.lootResultsCompletedAtNanos);
}

BattleStateDecodeResult decodeError(const Reader &reader) {
  BattleStateDecodeResult result;
  result.error = reader.error.value_or(BattleStateCodecError{
      .code = BattleStateCodecErrorCode::Truncated, .offset = reader.position});
  return result;
}

} // namespace

BattleStateEncodeResult
encodeBattleState(const BattleDeterministicState &input) {
  BattleStateEncodeResult result;
  BattleDeterministicState state = canonicalState(input);
  if (const auto error = validateState(state, true); error.has_value()) {
    result.error = BattleStateCodecError{.code = *error, .offset = 0U};
    return result;
  }
  Writer writer;
  writer.bytes.reserve(16U * 1024U);
  putState(writer, state);
  if (writer.bytes.size() > kMaximumBattleStateBytes) {
    result.error = BattleStateCodecError{
        .code = BattleStateCodecErrorCode::StateTooLarge, .offset = 0U};
    return result;
  }
  result.bytes = std::move(writer.bytes);
  return result;
}

BattleStateDecodeResult
decodeBattleState(std::span<const std::uint8_t> encodedState) {
  BattleStateDecodeResult result;
  if (encodedState.size() > kMaximumBattleStateBytes) {
    result.error = BattleStateCodecError{
        .code = BattleStateCodecErrorCode::StateTooLarge, .offset = 0U};
    return result;
  }
  Reader reader{.view = encodedState, .position = 0U, .error = std::nullopt};
  std::uint32_t magic = 0U;
  std::uint16_t schemaVersion = 0U;
  if (!reader.u32(magic) || !reader.u16(schemaVersion)) {
    return decodeError(reader);
  }
  if (magic != kBattleStateMagic) {
    result.error = BattleStateCodecError{
        .code = BattleStateCodecErrorCode::InvalidMagic, .offset = 0U};
    return result;
  }
  if (schemaVersion != kBattleStateSchemaVersion) {
    result.error = BattleStateCodecError{
        .code = BattleStateCodecErrorCode::UnsupportedSchemaVersion,
        .offset = 4U};
    return result;
  }

  BattleDeterministicState state{
      .roomId = shared::RoomId{0U},
      .battleId = shared::BattleInstanceId{0U},
      .rulesetVersion = 0U,
      .seed = 0U,
      .battleTime = battle::BattleTime{},
      .state = BattleLoadState::Created,
      .candidates = {},
      .capturedParticipants = {},
      .participants = {},
      .loadDeadlineTick = std::nullopt,
      .combatDeadlineTick = std::nullopt,
      .lootDeadlineTick = std::nullopt,
      .lastIntegratedServerTick = std::nullopt,
      .nextSnapshotSequence = 0U,
      .nextAttackAppliedSequence = 0U,
      .monster = std::nullopt,
      .combatTerminal = std::nullopt,
      .battleTerminalOutcome = std::nullopt,
      .drops = {},
      .holdings = {},
      .lootResolution = LootResolutionState::NotStarted,
      .resultState = BattleResultState::NotReady,
      .committedResult = std::nullopt,
      .lootResultsCompletedAtNanos = std::nullopt,
  };
  if (!getState(reader, state)) {
    return decodeError(reader);
  }
  if (reader.position != encodedState.size()) {
    result.error =
        BattleStateCodecError{.code = BattleStateCodecErrorCode::TrailingBytes,
                              .offset = reader.position};
    return result;
  }
  if (const auto error = validateState(state, true); error.has_value()) {
    result.error = BattleStateCodecError{.code = *error, .offset = 0U};
    return result;
  }
  result.state = std::move(state);
  return result;
}

BattleResultEncodeResult encodeCanonicalBattleResult(
    const battle::BattleDeterministicFinalResult &input) {
  BattleResultEncodeResult result;
  battle::BattleDeterministicFinalResult canonical = input;
  std::ranges::sort(canonical.entries, [](const auto &lhs, const auto &rhs) {
    return lhs.slot < rhs.slot;
  });
  if (const auto error = validateFinalResultBasic(canonical, true);
      error.has_value()) {
    result.error = BattleStateCodecError{.code = *error, .offset = 0U};
    return result;
  }
  Writer writer;
  writer.bytes.reserve(2U + canonical.entries.size() * 16U);
  putCanonicalResult(writer, canonical);
  if (writer.bytes.size() > kMaximumBattleStateBytes) {
    result.error = BattleStateCodecError{
        .code = BattleStateCodecErrorCode::StateTooLarge, .offset = 0U};
    return result;
  }
  result.bytes = std::move(writer.bytes);
  return result;
}

BattleResultEncodeResult encodeCanonicalTerminalState(
    std::span<const std::uint8_t> canonicalBattleStateBytes,
    const TerminalReceiptPayload &terminalReceipt) {
  BattleResultEncodeResult result;
  if (canonicalBattleStateBytes.empty() ||
      canonicalBattleStateBytes.size() > kMaximumBattleStateBytes) {
    result.error = BattleStateCodecError{
        .code = BattleStateCodecErrorCode::InvalidLength, .offset = 0U};
    return result;
  }
  const auto decodedState = decodeBattleState(canonicalBattleStateBytes);
  if (!decodedState.ok()) {
    result.error = decodedState.error;
    return result;
  }
  if (terminalReceipt.canonicalResultPayload.size() >
      std::numeric_limits<std::uint32_t>::max()) {
    result.error = BattleStateCodecError{
        .code = BattleStateCodecErrorCode::InvalidLength, .offset = 0U};
    return result;
  }

  std::vector<ParticipantSlot> resultSlots;
  if (const auto error = validateCanonicalResultPayload(
          terminalReceipt.canonicalResultPayload, resultSlots);
      error.has_value()) {
    result.error = *error;
    return result;
  }
  if (const auto error = validateTerminalSettlements(terminalReceipt);
      error.has_value()) {
    result.error = *error;
    return result;
  }
  if (resultSlots.size() != terminalReceipt.settlements.size()) {
    result.error = BattleStateCodecError{
        .code = BattleStateCodecErrorCode::InvariantViolation, .offset = 0U};
    return result;
  }
  for (std::size_t index = 0U; index < resultSlots.size(); ++index) {
    if (resultSlots[index] !=
        terminalReceipt.settlements[index].participantSlot) {
      result.error = BattleStateCodecError{
          .code = BattleStateCodecErrorCode::InvariantViolation, .offset = 0U};
      return result;
    }
  }

  Writer writer;
  writer.bytes.reserve(canonicalBattleStateBytes.size() +
                       terminalReceipt.canonicalResultPayload.size() + 64U);
  writer.bytesFrom(canonicalBattleStateBytes);
  writer.u16(terminalReceipt.terminalReason);
  writer.u32(static_cast<std::uint32_t>(
      terminalReceipt.canonicalResultPayload.size()));
  writer.bytesFrom(terminalReceipt.canonicalResultPayload);
  writer.u64(terminalReceipt.resultCommittedUnixEpochMilliseconds);
  writer.u64(terminalReceipt.resultCommittedBattleElapsedNanos);
  writer.array(terminalReceipt.settlementBatchId);
  writer.u16(terminalReceipt.settlementIntentCount);
  for (const auto &settlement : terminalReceipt.settlements) {
    writer.u16(settlement.participantSlot);
    writer.array(settlement.settlementId);
    writer.array(settlement.settlementPayloadHash);
  }
  if (writer.bytes.size() > kMaximumBattleStateBytes) {
    result.error = BattleStateCodecError{
        .code = BattleStateCodecErrorCode::StateTooLarge, .offset = 0U};
    return result;
  }
  result.bytes = std::move(writer.bytes);
  return result;
}

std::optional<Hash> canonicalBattleTerminalHash(
    std::span<const std::uint8_t> canonicalBattleStateBytes,
    const TerminalReceiptPayload &terminalReceipt) {
  const auto encoded =
      encodeCanonicalTerminalState(canonicalBattleStateBytes, terminalReceipt);
  if (!encoded.ok()) {
    return std::nullopt;
  }
  return canonicalStateHash(encoded.bytes);
}

std::optional<Hash>
battleStateHash(std::span<const std::uint8_t> canonicalStateBytes) noexcept {
  return canonicalStateHash(canonicalStateBytes);
}

} // namespace lol::battle_continuity
