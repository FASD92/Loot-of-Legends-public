#include <lol/battle/BattleLoadApi.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>

namespace lol::battle {
namespace {

constexpr std::size_t minimumCandidates = 2;
constexpr std::size_t maximumCandidates = 10;
constexpr std::int32_t minimumPositionMillimeters = -10000;
constexpr std::int32_t maximumPositionMillimeters = 10000;
constexpr std::int32_t movementPerTickMillimeters = 250;
constexpr std::uint64_t rateTokenUnits = 1'000'000'000ULL;
constexpr std::uint64_t rateCapacityUnits = 6 * rateTokenUnits;
constexpr std::uint64_t rateUnitsPerNanosecond = 30;
constexpr std::uint64_t fullRateRefillNanos = 200'000'000ULL;
constexpr std::uint64_t attackRateTokenUnits = 1'000'000'000ULL;
constexpr std::uint64_t attackRateCapacityUnits =
    CombatRuleset::attackBurst * attackRateTokenUnits;
constexpr std::uint64_t attackRateUnitsPerNanosecond =
    CombatRuleset::attackRatePerSecond;
constexpr std::uint64_t fullAttackRateRefillNanos = 500'000'000ULL;
constexpr std::uint64_t loadBarrierDurationTicks =
    10ULL * BattleTime::tickHertz;
constexpr std::uint64_t combatDurationTicks =
    (static_cast<std::uint64_t>(CombatRuleset::combatDeadlineMillis) *
     BattleTime::tickHertz) /
    1000ULL;
constexpr std::uint64_t lootResolutionDurationTicks =
    (static_cast<std::uint64_t>(RelicRuleset::resolutionWindowMillis) *
     BattleTime::tickHertz) /
    1000ULL;

std::uint64_t addTicks(std::uint64_t base, std::uint64_t duration) noexcept {
  if (base > std::numeric_limits<std::uint64_t>::max() - duration) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return base + duration;
}

AttackTerminalResult makeAttackResult(const AttackCommand &command,
                                      AttackResultCode code,
                                      std::uint32_t remainingHitPoints,
                                      CombatOutcome outcome) noexcept {
  return AttackTerminalResult{
      .commandId = command.commandId,
      .battleId = command.battleId,
      .code = code,
      .monsterId = CombatRuleset::monsterId,
      .remainingHitPoints = remainingHitPoints,
      .rulesetVersion = CombatRuleset::version,
      .outcome = outcome,
  };
}

bool serialNewer(std::uint32_t candidate, std::uint32_t reference) noexcept {
  const std::uint32_t distance = candidate - reference;
  return distance != 0 && distance < 0x80000000U;
}

std::int32_t scaledComponent(std::int16_t component,
                             std::uint64_t squaredMagnitude) noexcept {
  if (component == 0 || squaredMagnitude == 0) {
    return 0;
  }
  const auto signedComponent = static_cast<std::int32_t>(component);
  const auto absoluteComponent = static_cast<std::uint64_t>(
      signedComponent < 0 ? -signedComponent : signedComponent);
  const auto numerator = absoluteComponent *
                         static_cast<std::uint64_t>(movementPerTickMillimeters);
  const auto squaredNumerator = numerator * numerator;
  std::uint64_t low = 0;
  std::uint64_t high = static_cast<std::uint64_t>(movementPerTickMillimeters);
  while (low < high) {
    const auto middle = low + ((high - low + 1) / 2);
    if (middle * middle * squaredMagnitude <= squaredNumerator) {
      low = middle;
    } else {
      high = middle - 1;
    }
  }
  const auto scaled = static_cast<std::int32_t>(low);
  return signedComponent < 0 ? -scaled : scaled;
}

std::pair<std::int32_t, std::int32_t>
tickDelta(const DirectionIntent &direction) noexcept {
  const auto x = static_cast<std::int64_t>(direction.desiredX);
  const auto y = static_cast<std::int64_t>(direction.desiredY);
  const auto squaredMagnitude = static_cast<std::uint64_t>((x * x) + (y * y));
  return {scaledComponent(direction.desiredX, squaredMagnitude),
          scaledComponent(direction.desiredY, squaredMagnitude)};
}

bool validAdmission(const BattleAdmissionSnapshot &admission) {
  if (admission.roomId.value() == 0 || admission.battleId.value() == 0 ||
      admission.rulesetVersion != battleRulesetVersion || admission.seed == 0 ||
      admission.candidates.size() < minimumCandidates ||
      admission.candidates.size() > maximumCandidates) {
    return false;
  }

  for (std::size_t index = 0; index < admission.candidates.size(); ++index) {
    const auto &candidate = admission.candidates[index];
    if (candidate.sessionId.value() == 0 || candidate.generation.value() == 0 ||
        candidate.nickname.empty()) {
      return false;
    }
    for (std::size_t other = index + 1; other < admission.candidates.size();
         ++other) {
      if (candidate.sessionId == admission.candidates[other].sessionId) {
        return false;
      }
    }
  }
  return true;
}

bool validLoadStateValue(BattleLoadState value) noexcept {
  switch (value) {
  case BattleLoadState::Created:
  case BattleLoadState::LoadBarrierOpen:
  case BattleLoadState::GameplayCommitted:
  case BattleLoadState::LoadCancelled:
    return true;
  }
  return false;
}

bool validCandidateStateValue(LoadCandidateState value) noexcept {
  switch (value) {
  case LoadCandidateState::PendingLoad:
  case LoadCandidateState::Ready:
  case LoadCandidateState::Disconnected:
  case LoadCandidateState::TimedOut:
    return true;
  }
  return false;
}

bool validExitStatusValue(ParticipantExitStatus value) noexcept {
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

bool validMonsterStateValue(MonsterState value) noexcept {
  switch (value) {
  case MonsterState::Alive:
  case MonsterState::Dying:
  case MonsterState::Dead:
  case MonsterState::TimedOut:
    return true;
  }
  return false;
}

bool validCombatOutcomeValue(CombatOutcome value) noexcept {
  switch (value) {
  case CombatOutcome::None:
  case CombatOutcome::MonsterDefeated:
  case CombatOutcome::CombatTimeout:
    return true;
  }
  return false;
}

bool validLootDropStateValue(LootDropState value) noexcept {
  switch (value) {
  case LootDropState::Available:
  case LootDropState::Claimed:
  case LootDropState::Unclaimed:
    return true;
  }
  return false;
}

bool validLootResolutionStateValue(LootResolutionState value) noexcept {
  switch (value) {
  case LootResolutionState::NotStarted:
  case LootResolutionState::Open:
  case LootResolutionState::Resolved:
    return true;
  }
  return false;
}

bool validBattleOutcomeValue(BattleOutcome value) noexcept {
  switch (value) {
  case BattleOutcome::MonsterDefeated:
  case BattleOutcome::CombatTimeout:
  case BattleOutcome::CancelledNoActiveParticipants:
    return true;
  }
  return false;
}

bool validResultStateValue(BattleResultState value) noexcept {
  switch (value) {
  case BattleResultState::NotReady:
  case BattleResultState::Committed:
  case BattleResultState::ResultGenerationFailed:
    return true;
  }
  return false;
}

} // namespace

CreateBattleResult BattleInstance::create(BattleAdmissionSnapshot admission) {
  if (!validAdmission(admission)) {
    return {BattleLoadResultCode::InvalidArgument, std::nullopt};
  }
  return {BattleLoadResultCode::Ok, BattleInstance{std::move(admission)}};
}

BattleInstance::BattleInstance(BattleAdmissionSnapshot admission)
    : roomId_(admission.roomId), battleId_(admission.battleId),
      rulesetVersion_(admission.rulesetVersion), seed_(admission.seed) {
  candidates_.reserve(admission.candidates.size());
  for (auto &candidate : admission.candidates) {
    candidates_.push_back(
        CandidateRecord{std::move(candidate), LoadCandidateState::PendingLoad});
  }
}

BattleTime BattleInstance::battleTimeFromLiveClock(
    std::chrono::steady_clock::time_point at) const noexcept {
  // Legacy callers already use a process monotonic epoch. Keeping that epoch
  // at this adapter boundary preserves their ordering even when old tests or
  // flows mix independently constructed time_points; no such timestamp is
  // exported as Battle state.
  if (at <= std::chrono::steady_clock::time_point{}) {
    return BattleTime::fromLogicalTick(0);
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      at - std::chrono::steady_clock::time_point{});
  const auto count = elapsed.count();
  if (count <= 0) {
    return BattleTime::fromLogicalTick(0);
  }
  using Rep = decltype(count);
  if constexpr (std::numeric_limits<Rep>::digits >=
                std::numeric_limits<std::uint64_t>::digits) {
    if (static_cast<std::uintmax_t>(count) >
        std::numeric_limits<std::uint64_t>::max()) {
      return BattleTime::fromLogicalTick(
          std::numeric_limits<std::uint64_t>::max() / BattleTime::tickNanos);
    }
  }
  return BattleTime::fromElapsedNanos(static_cast<std::uint64_t>(count));
}

void BattleInstance::observeBattleTime(BattleTime at) noexcept {
  if (!at.valid()) {
    return;
  }
  if (at.logicalTick > battleTime_.logicalTick ||
      (at.logicalTick == battleTime_.logicalTick &&
       at.battleElapsedNanos > battleTime_.battleElapsedNanos)) {
    battleTime_ = at;
  }
}

BattleLoadResultCode BattleInstance::openLoadBarrier(BattleTime at) {
  if (state_ == BattleLoadState::Created) {
    observeBattleTime(at);
    state_ = BattleLoadState::LoadBarrierOpen;
    loadDeadlineTick_ =
        addTicks(battleTime_.logicalTick, loadBarrierDurationTicks);
    return BattleLoadResultCode::Ok;
  }
  return state_ == BattleLoadState::LoadBarrierOpen
             ? BattleLoadResultCode::Ok
             : BattleLoadResultCode::NotEligible;
}

BattleLoadResultCode BattleInstance::openLoadBarrier() {
  return openLoadBarrier(battleTime_);
}

BattleLoadResultCode BattleInstance::correlate(
    shared::RoomId roomId, shared::BattleInstanceId battleId,
    shared::SessionId sessionId, shared::SessionGeneration generation,
    CandidateRecord *&candidate) {
  candidate = nullptr;
  if (roomId != roomId_ || battleId != battleId_) {
    return BattleLoadResultCode::StaleBattle;
  }
  const auto found =
      std::find_if(candidates_.begin(), candidates_.end(),
                   [&](const CandidateRecord &entry) {
                     return entry.identity.sessionId == sessionId;
                   });
  if (found == candidates_.end()) {
    return BattleLoadResultCode::NotEligible;
  }
  if (found->identity.generation != generation) {
    return BattleLoadResultCode::StaleSession;
  }
  candidate = &*found;
  return BattleLoadResultCode::Ok;
}

BattleLoadResultCode
BattleInstance::completeLoad(const ArenaLoadCompleteCommand &command,
                             bool gameplayTransportReady, BattleTime at) {
  CandidateRecord *candidate = nullptr;
  const auto result =
      correlate(command.roomId, command.battleId, command.sessionId,
                command.generation, candidate);
  if (result != BattleLoadResultCode::Ok) {
    return result;
  }
  if (candidate->state == LoadCandidateState::Ready) {
    return BattleLoadResultCode::Ok;
  }
  if (state_ != BattleLoadState::LoadBarrierOpen ||
      candidate->state != LoadCandidateState::PendingLoad) {
    return BattleLoadResultCode::NotEligible;
  }
  if (!gameplayTransportReady) {
    return BattleLoadResultCode::Ok;
  }
  observeBattleTime(at);
  candidate->state = LoadCandidateState::Ready;
  resolveIfComplete();
  return BattleLoadResultCode::Ok;
}

BattleLoadResultCode
BattleInstance::completeLoad(const ArenaLoadCompleteCommand &command,
                             bool gameplayTransportReady) {
  return completeLoad(command, gameplayTransportReady, battleTime_);
}

BattleLoadResultCode
BattleInstance::freezeExited(const CandidateDisconnectedCommand &command,
                             ParticipantExitStatus exitStatus,
                             BattleTime completedAt) {
  CandidateRecord *candidate = nullptr;
  const auto result =
      correlate(command.roomId, command.battleId, command.sessionId,
                command.generation, candidate);
  if (result != BattleLoadResultCode::Ok) {
    return result;
  }
  if (state_ != BattleLoadState::GameplayCommitted) {
    // No captured participant exists before gameplay commit; the room
    // membership removal (in the composition seam) is the only exit effect.
    return BattleLoadResultCode::Ok;
  }
  auto *participant = participantRecord(command.sessionId);
  if (participant == nullptr) {
    return BattleLoadResultCode::Ok;
  }
  if (participant->exitStatus != ParticipantExitStatus::GameplayEligible) {
    // The first exit reason is sticky; later notifications are no mutation.
    return BattleLoadResultCode::Ok;
  }
  observeBattleTime(completedAt);
  participant->gameplayEligible = false;
  participant->inputEnabled = false;
  participant->tickDeltaXMillimeter = 0;
  participant->tickDeltaYMillimeter = 0;
  participant->exitStatus = exitStatus;
  for (auto &captured : capturedParticipants_) {
    if (captured.sessionId == command.sessionId) {
      captured.exitStatus = exitStatus;
      break;
    }
  }
  // The active-count transition is detected only after the first sticky exit
  // is applied; duplicate notifications never reach this point.
  applyCancellationIfNoActiveParticipants();
  // A first all-active-participants-exited cancellation closed the Battle
  // terminal source and any Open loot; commit the immutable cancellation
  // result exactly once.
  commitResultIfReady(completedAt);
  return BattleLoadResultCode::Ok;
}

void BattleInstance::applyCancellationIfNoActiveParticipants() {
  if (battleTerminalOutcome_.has_value() ||
      resultState_ != BattleResultState::NotReady) {
    // A prior Battle terminal outcome already won, or a normal result already
    // committed or failed: later exits are no mutation.
    return;
  }
  const auto activeCount = std::count_if(
      participantRecords_.begin(), participantRecords_.end(),
      [](const ParticipantRecord &record) { return record.gameplayEligible; });
  if (activeCount != 0) {
    return;
  }
  // Exactly-once cancellation: the Battle terminal outcome wins the durable
  // terminal source without creating a combat terminal record or altering
  // historical combat. Every Captured Participant record and sticky exit
  // reason, and every claimed owner and holding, are retained exactly. Open
  // loot is closed to Unclaimed so the immutable source is terminal; loot that
  // never started generates no Drop and stays empty.
  battleTerminalOutcome_ = BattleOutcome::CancelledNoActiveParticipants;
  loadDeadlineTick_.reset();
  combatDeadlineTick_.reset();
  lootDeadlineTick_.reset();
  if (lootResolution_ == LootResolutionState::Open) {
    for (auto &record : drops_) {
      if (record.state == LootDropState::Available) {
        record.state = LootDropState::Unclaimed;
      }
    }
    lootResolution_ = LootResolutionState::Resolved;
  }
}

BattleLoadResultCode
BattleInstance::disconnect(const CandidateDisconnectedCommand &command,
                           BattleTime completedAt) {
  CandidateRecord *candidate = nullptr;
  const auto result =
      correlate(command.roomId, command.battleId, command.sessionId,
                command.generation, candidate);
  if (result != BattleLoadResultCode::Ok) {
    return result;
  }
  if (state_ == BattleLoadState::GameplayCommitted) {
    return freezeExited(command, ParticipantExitStatus::Disconnected,
                        completedAt);
  }
  if (state_ != BattleLoadState::LoadBarrierOpen ||
      candidate->state != LoadCandidateState::PendingLoad) {
    return BattleLoadResultCode::Ok;
  }
  observeBattleTime(completedAt);
  candidate->state = LoadCandidateState::Disconnected;
  resolveIfComplete();
  return BattleLoadResultCode::Ok;
}

BattleLoadResultCode
BattleInstance::disconnect(const CandidateDisconnectedCommand &command,
                           std::chrono::steady_clock::time_point completedAt) {
  const auto battleTime = battleTimeFromLiveClock(completedAt);
  return disconnect(command, battleTime);
}

BattleLoadResultCode
BattleInstance::disconnect(const CandidateDisconnectedCommand &command) {
  return disconnect(command, battleTime_);
}

BattleLoadResultCode
BattleInstance::leave(const CandidateDisconnectedCommand &command,
                      BattleTime completedAt) {
  return freezeExited(command, ParticipantExitStatus::VoluntaryLeft,
                      completedAt);
}

BattleLoadResultCode
BattleInstance::leave(const CandidateDisconnectedCommand &command,
                      std::chrono::steady_clock::time_point completedAt) {
  return leave(command, battleTimeFromLiveClock(completedAt));
}

BattleLoadResultCode
BattleInstance::leave(const CandidateDisconnectedCommand &command) {
  return leave(command, battleTime_);
}

BattleLoadResultCode
BattleInstance::expireLoadBarrier(const LoadBarrierDeadlineCommand &command,
                                  BattleTime at) {
  if (command.roomId != roomId_ || command.battleId != battleId_) {
    return BattleLoadResultCode::StaleBattle;
  }
  if (state_ != BattleLoadState::LoadBarrierOpen) {
    return state_ == BattleLoadState::Created
               ? BattleLoadResultCode::NotEligible
               : BattleLoadResultCode::Ok;
  }
  if (!at.valid() || !loadDeadlineTick_.has_value() ||
      at.logicalTick < *loadDeadlineTick_) {
    return BattleLoadResultCode::NotEligible;
  }
  observeBattleTime(at);
  for (auto &candidate : candidates_) {
    if (candidate.state == LoadCandidateState::PendingLoad) {
      candidate.state = LoadCandidateState::TimedOut;
    }
  }
  resolveIfComplete();
  return BattleLoadResultCode::Ok;
}

BattleLoadResultCode
BattleInstance::expireLoadBarrier(const LoadBarrierDeadlineCommand &command) {
  const auto at = loadDeadlineTick_.has_value()
                      ? BattleTime::fromLogicalTick(*loadDeadlineTick_)
                      : battleTime_;
  return expireLoadBarrier(command, at);
}

BattleInputResultCode
BattleInstance::suspendInput(shared::SessionId sessionId,
                             shared::SessionGeneration generation) {
  return suspendInput(sessionId, generation, battleTime_);
}

BattleInputResultCode
BattleInstance::suspendInput(shared::SessionId sessionId,
                             shared::SessionGeneration generation,
                             BattleTime at) {
  if (state_ != BattleLoadState::GameplayCommitted) {
    return BattleInputResultCode::NotEligible;
  }
  auto *participant = participantRecord(sessionId);
  if (participant == nullptr) {
    return BattleInputResultCode::NotEligible;
  }
  if (participant->generation != generation) {
    return BattleInputResultCode::StaleSession;
  }
  if (participant->inputEnabled || participant->tickDeltaXMillimeter != 0 ||
      participant->tickDeltaYMillimeter != 0) {
    observeBattleTime(at);
    participant->inputEnabled = false;
    participant->tickDeltaXMillimeter = 0;
    participant->tickDeltaYMillimeter = 0;
  }
  return BattleInputResultCode::Ok;
}

BattleInputResultCode
BattleInstance::resumeInput(shared::SessionId sessionId,
                            shared::SessionGeneration generation) {
  return resumeInput(sessionId, generation, battleTime_);
}

BattleInputResultCode
BattleInstance::resumeInput(shared::SessionId sessionId,
                            shared::SessionGeneration generation,
                            BattleTime at) {
  if (state_ != BattleLoadState::GameplayCommitted) {
    return BattleInputResultCode::NotEligible;
  }
  auto *participant = participantRecord(sessionId);
  if (participant == nullptr) {
    return BattleInputResultCode::NotEligible;
  }
  if (participant->generation != generation) {
    return BattleInputResultCode::StaleSession;
  }
  const auto inputEnabled = participant->gameplayEligible &&
                            resultState_ == BattleResultState::NotReady;
  if (participant->inputEnabled != inputEnabled) {
    observeBattleTime(at);
    participant->inputEnabled = inputEnabled;
  }
  return BattleInputResultCode::Ok;
}

std::optional<BattleResumeProjection>
BattleInstance::resumeProjection(shared::SessionId sessionId,
                                 shared::SessionGeneration generation) const {
  if (state_ != BattleLoadState::GameplayCommitted) {
    return std::nullopt;
  }
  const auto found =
      std::find_if(participantRecords_.begin(), participantRecords_.end(),
                   [sessionId](const ParticipantRecord &record) {
                     return record.sessionId == sessionId;
                   });
  if (found == participantRecords_.end() || found->generation != generation) {
    return std::nullopt;
  }
  std::vector<BattleResumeParticipantProjection> participants;
  participants.reserve(participantRecords_.size());
  for (const auto &participant : participantRecords_) {
    participants.push_back(BattleResumeParticipantProjection{
        .sessionId = participant.sessionId,
        .generation = participant.generation,
        .posXMillimeter = participant.posXMillimeter,
        .posYMillimeter = participant.posYMillimeter,
        .exitStatus = participant.exitStatus,
        .inputEnabled = participant.inputEnabled,
    });
  }
  return BattleResumeProjection{
      .roomId = roomId_,
      .battleId = battleId_,
      .sessionId = sessionId,
      .generation = generation,
      .battleState = state_,
      .serverTick = lastIntegratedServerTick_.value_or(0),
      .inputEnabled = found->inputEnabled,
      .participants = std::move(participants),
      .combat = combatProjection(),
      .loot = lootProjection(),
      .result = resultProjection(),
  };
}

void BattleInstance::resolveIfComplete() {
  const auto allTerminal = std::all_of(
      candidates_.begin(), candidates_.end(), [](const CandidateRecord &entry) {
        return entry.state != LoadCandidateState::PendingLoad;
      });
  if (!allTerminal) {
    return;
  }

  const auto readyCount = std::count_if(
      candidates_.begin(), candidates_.end(), [](const CandidateRecord &entry) {
        return entry.state == LoadCandidateState::Ready;
      });
  if (readyCount < static_cast<std::ptrdiff_t>(minimumCandidates)) {
    state_ = BattleLoadState::LoadCancelled;
    loadDeadlineTick_.reset();
    return;
  }

  capturedParticipants_.reserve(static_cast<std::size_t>(readyCount));
  participantRecords_.reserve(static_cast<std::size_t>(readyCount));
  std::size_t readyIndex = 0;
  for (const auto &candidate : candidates_) {
    if (candidate.state == LoadCandidateState::Ready) {
      // Participant slots are dense in captured/admission order. A candidate
      // that disconnected or timed out before commit must not leave a hole in
      // the value-only participant identity space.
      const auto slot = static_cast<ParticipantSlot>(readyIndex + 1);
      const auto spawn = CombatRuleset::participantSpawnPosition(
          static_cast<std::uint32_t>(readyCount),
          static_cast<std::uint32_t>(readyIndex));
      capturedParticipants_.push_back(CapturedParticipant{
          .accountId = candidate.identity.accountId,
          .sessionId = candidate.identity.sessionId,
          .generation = candidate.identity.generation,
          .nickname = candidate.identity.nickname,
          .exitStatus = ParticipantExitStatus::GameplayEligible,
      });
      participantRecords_.push_back(ParticipantRecord{
          .slot = slot,
          .sessionId = candidate.identity.sessionId,
          .generation = candidate.identity.generation,
          .posXMillimeter = spawn.xMillimeter,
          .posYMillimeter = spawn.yMillimeter,
          .tickDeltaXMillimeter = 0,
          .tickDeltaYMillimeter = 0,
          .latestSeenActionSequence = std::nullopt,
          .lastRateUpdateNanos = std::nullopt,
          .rateCreditUnits = rateCapacityUnits,
          .lastAttackRateUpdateNanos = std::nullopt,
          .attackRateCreditUnits = attackRateCapacityUnits,
          .lastClaimRateUpdateNanos = std::nullopt,
          .claimRateCreditUnits = claimRateCapacityUnits,
          .lastAcceptedAttackNanos = std::nullopt,
          .attackResults =
              AttackResultStore{candidate.identity.sessionId,
                                candidate.identity.generation, battleId_},
          .lootResults =
              LootResultStore{candidate.identity.sessionId,
                              candidate.identity.generation, battleId_},
          .gameplayEligible = true,
          .inputEnabled = true,
      });
      ++readyIndex;
    }
  }
  monster_ = Monster::spawn(CombatRuleset::monsterHitPointsForParticipants(
      static_cast<std::uint32_t>(participantRecords_.size())));
  state_ = BattleLoadState::GameplayCommitted;
  loadDeadlineTick_.reset();
  combatDeadlineTick_ = addTicks(battleTime_.logicalTick, combatDurationTicks);
}

BattleInstance::ParticipantRecord *
BattleInstance::participantRecord(shared::SessionId sessionId) noexcept {
  const auto found =
      std::find_if(participantRecords_.begin(), participantRecords_.end(),
                   [sessionId](const ParticipantRecord &record) {
                     return record.sessionId == sessionId;
                   });
  return found == participantRecords_.end() ? nullptr : &*found;
}

ParticipantSlot
BattleInstance::slotForSession(shared::SessionId sessionId) const noexcept {
  const auto participant =
      std::find_if(participantRecords_.begin(), participantRecords_.end(),
                   [sessionId](const ParticipantRecord &record) {
                     return record.sessionId == sessionId;
                   });
  if (participant != participantRecords_.end()) {
    return participant->slot;
  }
  return systemParticipantSlot;
}

std::optional<shared::SessionId>
BattleInstance::sessionForSlot(ParticipantSlot slot) const noexcept {
  if (slot == systemParticipantSlot) {
    return std::nullopt;
  }
  const auto participant = std::find_if(
      participantRecords_.begin(), participantRecords_.end(),
      [slot](const ParticipantRecord &record) { return record.slot == slot; });
  if (participant != participantRecords_.end()) {
    return participant->sessionId;
  }
  return std::nullopt;
}

MovementResultCode BattleInstance::acceptMove(const MoveCommand &command,
                                              BattleTime receivedAt) {
  if (command.battleId != battleId_) {
    return MovementResultCode::StaleBattle;
  }
  if (state_ != BattleLoadState::GameplayCommitted) {
    return MovementResultCode::NotEligible;
  }
  auto *movement = participantRecord(command.sessionId);
  if (movement == nullptr) {
    return MovementResultCode::NotEligible;
  }
  if (movement->generation != command.generation) {
    return MovementResultCode::StaleSession;
  }
  if (!movement->gameplayEligible) {
    return MovementResultCode::NotEligible;
  }
  if (!movement->inputEnabled) {
    return MovementResultCode::NotEligible;
  }
  if (command.direction.inputFlags != 0 ||
      command.direction.desiredX == std::numeric_limits<std::int16_t>::min() ||
      command.direction.desiredY == std::numeric_limits<std::int16_t>::min()) {
    return MovementResultCode::InvalidArgument;
  }
  if (movement->latestSeenActionSequence.has_value() &&
      !serialNewer(command.actionSequence,
                   *movement->latestSeenActionSequence)) {
    return MovementResultCode::StaleAction;
  }

  observeBattleTime(receivedAt);
  if (!movement->lastRateUpdateNanos.has_value()) {
    movement->lastRateUpdateNanos = receivedAt.battleElapsedNanos;
  } else if (receivedAt.battleElapsedNanos > *movement->lastRateUpdateNanos) {
    const auto elapsedNanoseconds =
        std::min(receivedAt.battleElapsedNanos - *movement->lastRateUpdateNanos,
                 fullRateRefillNanos);
    movement->rateCreditUnits = std::min(
        rateCapacityUnits, movement->rateCreditUnits +
                               elapsedNanoseconds * rateUnitsPerNanosecond);
    movement->lastRateUpdateNanos = receivedAt.battleElapsedNanos;
  }
  if (movement->rateCreditUnits < rateTokenUnits) {
    return MovementResultCode::RateLimited;
  }
  movement->rateCreditUnits -= rateTokenUnits;
  movement->latestSeenActionSequence = command.actionSequence;
  const auto [deltaX, deltaY] = tickDelta(command.direction);
  movement->tickDeltaXMillimeter = deltaX;
  movement->tickDeltaYMillimeter = deltaY;
  return MovementResultCode::Ok;
}

MovementResultCode
BattleInstance::acceptMove(const MoveCommand &command,
                           std::chrono::steady_clock::time_point receivedAt) {
  return acceptMove(command, battleTimeFromLiveClock(receivedAt));
}

MovementResultCode BattleInstance::acceptMove(const MoveCommand &command) {
  return acceptMove(command, battleTime_);
}

MovementResultCode
BattleInstance::integrateMovement(const MovementTickCommand &command,
                                  BattleTime at) {
  if (command.battleId != battleId_) {
    return MovementResultCode::StaleBattle;
  }
  if (state_ != BattleLoadState::GameplayCommitted) {
    return MovementResultCode::NotEligible;
  }
  if (lastIntegratedServerTick_.has_value() &&
      !serialNewer(command.serverTick, *lastIntegratedServerTick_)) {
    return MovementResultCode::StaleTick;
  }
  observeBattleTime(at);
  for (auto &movement : participantRecords_) {
    if (!movement.gameplayEligible || !movement.inputEnabled) {
      continue;
    }
    movement.posXMillimeter =
        std::clamp(movement.posXMillimeter + movement.tickDeltaXMillimeter,
                   minimumPositionMillimeters, maximumPositionMillimeters);
    movement.posYMillimeter =
        std::clamp(movement.posYMillimeter + movement.tickDeltaYMillimeter,
                   minimumPositionMillimeters, maximumPositionMillimeters);
  }
  lastIntegratedServerTick_ = command.serverTick;
  return MovementResultCode::Ok;
}

MovementResultCode
BattleInstance::integrateMovement(const MovementTickCommand &command) {
  return integrateMovement(command,
                           BattleTime::fromLogicalTick(command.serverTick));
}

MovementProjection BattleInstance::movementProjection() const {
  std::vector<PlayerPositionProjection> players;
  players.reserve(participantRecords_.size());
  for (const auto &movement : participantRecords_) {
    players.push_back(PlayerPositionProjection{
        .sessionId = movement.sessionId,
        .posXMillimeter = movement.posXMillimeter,
        .posYMillimeter = movement.posYMillimeter,
    });
  }
  return MovementProjection{
      .battleId = battleId_,
      .serverTick = lastIntegratedServerTick_.value_or(0),
      .players = std::move(players),
  };
}

std::optional<StateSnapshotProjection> BattleInstance::captureStateSnapshot() {
  if (state_ != BattleLoadState::GameplayCommitted ||
      !lastIntegratedServerTick_.has_value()) {
    return std::nullopt;
  }
  auto movement = movementProjection();
  const auto sequence = nextSnapshotSequence_++;
  if (nextSnapshotSequence_ == 0) {
    nextSnapshotSequence_ = 1;
  }
  return StateSnapshotProjection{
      .battleId = movement.battleId,
      .snapshotSequence = sequence,
      .serverTick = movement.serverTick,
      .players = std::move(movement.players),
  };
}

AttackTerminalResult BattleInstance::attack(const AttackCommand &command,
                                            BattleTime receivedAt) {
  const auto currentHitPoints =
      monster_.has_value() ? monster_->hitPoints() : std::uint32_t{0};
  const auto currentOutcome = combatTerminal_.has_value()
                                  ? combatTerminal_->outcome
                                  : CombatOutcome::None;
  if (command.battleId != battleId_) {
    return makeAttackResult(command, AttackResultCode::StaleBattle,
                            currentHitPoints, currentOutcome);
  }
  if (state_ != BattleLoadState::GameplayCommitted || !monster_.has_value()) {
    return makeAttackResult(command, AttackResultCode::NotEligible,
                            currentHitPoints, currentOutcome);
  }
  auto *participant = participantRecord(command.sessionId);
  if (participant == nullptr) {
    return makeAttackResult(command, AttackResultCode::NotEligible,
                            monster_->hitPoints(), currentOutcome);
  }
  if (participant->generation != command.generation) {
    return makeAttackResult(command, AttackResultCode::StaleSession,
                            monster_->hitPoints(), currentOutcome);
  }
  const auto evicted =
      participant->attackResults.evictExpired(receivedAt.battleElapsedNanos);
  if (evicted != 0) {
    // Expiration is itself a deterministic store mutation. A replay or
    // rejected command with no expiration must not move Battle logical time.
    observeBattleTime(receivedAt);
  }
  const auto inspection = participant->attackResults.inspect(command);
  if (inspection.decision == AttackResultStoreDecision::Replay) {
    return *inspection.result;
  }
  if (inspection.decision == AttackResultStoreDecision::Conflict) {
    return makeAttackResult(command, AttackResultCode::CommandConflict,
                            monster_->hitPoints(), currentOutcome);
  }
  if (inspection.decision == AttackResultStoreDecision::Overloaded) {
    return makeAttackResult(command, AttackResultCode::Overloaded,
                            monster_->hitPoints(), currentOutcome);
  }
  if (inspection.decision != AttackResultStoreDecision::Available) {
    std::terminate();
  }
  observeBattleTime(receivedAt);

  const auto retain = [participant, &command, this](AttackResultCode code) {
    const auto outcome = combatTerminal_.has_value() ? combatTerminal_->outcome
                                                     : CombatOutcome::None;
    auto result =
        makeAttackResult(command, code, monster_->hitPoints(), outcome);
    if (!participant->attackResults.retain(command, result)) {
      std::terminate();
    }
    return result;
  };

  if (!participant->gameplayEligible) {
    return retain(AttackResultCode::NotEligible);
  }
  if (combatTerminal_.has_value()) {
    return retain(AttackResultCode::TerminalAlreadyDecided);
  }
  if (!participant->inputEnabled) {
    return retain(AttackResultCode::NotEligible);
  }

  if (!participant->lastAttackRateUpdateNanos.has_value()) {
    participant->lastAttackRateUpdateNanos = receivedAt.battleElapsedNanos;
  } else if (receivedAt.battleElapsedNanos >
             *participant->lastAttackRateUpdateNanos) {
    const auto elapsedNanoseconds = std::min(
        receivedAt.battleElapsedNanos - *participant->lastAttackRateUpdateNanos,
        fullAttackRateRefillNanos);
    participant->attackRateCreditUnits =
        std::min(attackRateCapacityUnits,
                 participant->attackRateCreditUnits +
                     elapsedNanoseconds * attackRateUnitsPerNanosecond);
    participant->lastAttackRateUpdateNanos = receivedAt.battleElapsedNanos;
  }
  if (participant->attackRateCreditUnits < attackRateTokenUnits) {
    return retain(AttackResultCode::Overloaded);
  }
  participant->attackRateCreditUnits -= attackRateTokenUnits;

  if (command.targetHint != monster_->id() ||
      monster_->state() != MonsterState::Alive) {
    return retain(AttackResultCode::InvalidTarget);
  }
  if (!CombatRuleset::inAttackRange(CombatPosition{participant->posXMillimeter,
                                                   participant->posYMillimeter},
                                    monster_->position())) {
    return retain(AttackResultCode::OutOfRange);
  }
  constexpr auto attackCooldownNanos =
      static_cast<std::uint64_t>(CombatRuleset::perPlayerCooldownMillis) *
      1'000'000ULL;
  if (participant->lastAcceptedAttackNanos.has_value() &&
      (receivedAt.battleElapsedNanos < *participant->lastAcceptedAttackNanos ||
       receivedAt.battleElapsedNanos - *participant->lastAcceptedAttackNanos <
           attackCooldownNanos)) {
    return retain(AttackResultCode::Cooldown);
  }

  const auto damage = monster_->applyAttack();
  participant->lastAcceptedAttackNanos = receivedAt.battleElapsedNanos;
  if (damage == MonsterDamageResult::Applied) {
    return retain(AttackResultCode::Ok);
  }
  if (damage != MonsterDamageResult::Lethal || !monster_->completeDeath()) {
    std::terminate();
  }

  combatTerminal_ = CombatTerminalRecord{
      .eventId = EventId{.high = battleId_.value(), .low = 2},
      .battleId = battleId_,
      .eventSequence = 2,
      .outcome = CombatOutcome::MonsterDefeated,
      .monsterId = monster_->id(),
      .serverTick = lastIntegratedServerTick_.value_or(0),
      .rulesetVersion = CombatRuleset::version,
  };
  combatDeadlineTick_.reset();
  // Claimable loot exists only after the actual MonsterDefeated terminal. The
  // frozen inputs (captured count 2..10, immutable v1 snapshot) make generation
  // total for a valid committed Battle; on an impossible internal invariant no
  // partial drops or fabricated holdings are created.
  const auto drops =
      generateDrops(roomId_, battleId_, seed_, RelicRuleset::version,
                    static_cast<std::uint32_t>(capturedParticipants_.size()),
                    RelicCatalog::v1Snapshot());
  if (drops.has_value()) {
    drops_.reserve(drops->size());
    for (const auto &drop : *drops) {
      // Every generated Drop starts claimable (Available) with no owner.
      drops_.push_back(LootDropRecord{.drop = drop, .owner = std::nullopt});
    }
    lootResolution_ = LootResolutionState::Open;
    lootDeadlineTick_ =
        addTicks(receivedAt.logicalTick, lootResolutionDurationTicks);
  } else {
    // Drop generation itself violated the frozen Task 1 invariant (impossible
    // for a valid committed Battle): no partial drops, no fabricated holdings,
    // and no empty result are committed.
    resultState_ = BattleResultState::ResultGenerationFailed;
    markLootResultsCompleted(receivedAt);
  }
  auto result = retain(AttackResultCode::Ok);
  for (auto &record : participantRecords_) {
    record.attackResults.markBattleCompleted(receivedAt.battleElapsedNanos);
  }
  // No commit while loot is still Open; the result becomes NotReady until the
  // resolution deadline or the final claim closes it.
  commitResultIfReady(receivedAt);
  return result;
}

AttackTerminalResult
BattleInstance::attack(const AttackCommand &command,
                       std::chrono::steady_clock::time_point receivedAt) {
  return attack(command, battleTimeFromLiveClock(receivedAt));
}

AttackTerminalResult BattleInstance::attack(const AttackCommand &command) {
  return attack(command, battleTime_);
}

AttackExecution BattleInstance::attackWithApplied(const AttackCommand &command,
                                                  BattleTime receivedAt) {
  const auto before = monster_.has_value() ? monster_->hitPoints() : 0U;
  auto result = attack(command, receivedAt);
  const auto after = monster_.has_value() ? monster_->hitPoints() : 0U;
  if (result.code != AttackResultCode::Ok || after >= before) {
    return {.result = std::move(result), .applied = std::nullopt};
  }
  const auto sequence = nextAttackAppliedSequence_++;
  if (nextAttackAppliedSequence_ == 0) {
    nextAttackAppliedSequence_ = 1;
  }
  return {
      .result = std::move(result),
      .applied =
          AttackAppliedRecord{
              .eventId = EventId{.high = battleId_.value(),
                                 .low = (std::uint64_t{3} << 32U) | sequence},
              .battleId = battleId_,
              .eventSequence = sequence,
              .attackerSessionId = command.sessionId,
              .monsterId = monster_->id(),
              .actualDamage = before - after,
              .remainingHitPoints = after,
              .serverTick = lastIntegratedServerTick_.value_or(0),
              .outcome = combatTerminal_.has_value() ? combatTerminal_->outcome
                                                     : CombatOutcome::None,
          },
  };
}

AttackExecution BattleInstance::attackWithApplied(
    const AttackCommand &command,
    std::chrono::steady_clock::time_point receivedAt) {
  return attackWithApplied(command, battleTimeFromLiveClock(receivedAt));
}

AttackExecution
BattleInstance::attackWithApplied(const AttackCommand &command) {
  return attackWithApplied(command, battleTime_);
}

CombatDeadlineResultCode
BattleInstance::expireCombat(const CombatDeadlineCommand &command,
                             BattleTime completedAt) {
  if (command.battleId != battleId_) {
    return CombatDeadlineResultCode::StaleBattle;
  }
  if (state_ != BattleLoadState::GameplayCommitted || !monster_.has_value()) {
    return CombatDeadlineResultCode::NotEligible;
  }
  if (combatTerminal_.has_value() || battleTerminalOutcome_.has_value()) {
    // A prior combat terminal or Battle terminal outcome (for example a
    // cancellation) already decided the Battle; the deadline is no mutation
    // and never creates a CombatTimeout.
    return CombatDeadlineResultCode::TerminalAlreadyDecided;
  }
  if (!completedAt.valid() || !combatDeadlineTick_.has_value() ||
      completedAt.logicalTick < *combatDeadlineTick_) {
    return CombatDeadlineResultCode::NotEligible;
  }
  if (!monster_->timeout()) {
    return CombatDeadlineResultCode::TerminalAlreadyDecided;
  }
  observeBattleTime(completedAt);
  combatTerminal_ = CombatTerminalRecord{
      .eventId = EventId{.high = battleId_.value(), .low = 2},
      .battleId = battleId_,
      .eventSequence = 2,
      .outcome = CombatOutcome::CombatTimeout,
      .monsterId = monster_->id(),
      .serverTick = lastIntegratedServerTick_.value_or(0),
      .rulesetVersion = CombatRuleset::version,
  };
  combatDeadlineTick_.reset();
  for (auto &record : participantRecords_) {
    record.attackResults.markBattleCompleted(completedAt.battleElapsedNanos);
  }
  // CombatTimeout commits immediately without Drop generation.
  commitResultIfReady(completedAt);
  return CombatDeadlineResultCode::Ok;
}

CombatDeadlineResultCode BattleInstance::expireCombat(
    const CombatDeadlineCommand &command,
    std::chrono::steady_clock::time_point completedAt) {
  return expireCombat(command, battleTimeFromLiveClock(completedAt));
}

CombatDeadlineResultCode
BattleInstance::expireCombat(const CombatDeadlineCommand &command) {
  const auto completedAt =
      combatDeadlineTick_.has_value()
          ? BattleTime::fromLogicalTick(*combatDeadlineTick_)
          : battleTime_;
  return expireCombat(command, completedAt);
}

LootDeadlineResultCode
BattleInstance::expireLoot(const LootDeadlineCommand &command,
                           BattleTime completedAt) {
  if (command.battleId != battleId_) {
    return LootDeadlineResultCode::StaleBattle;
  }
  if (lootResolution_ != LootResolutionState::Open) {
    return lootResolution_ == LootResolutionState::Resolved
               ? LootDeadlineResultCode::ResolutionClosed
               : LootDeadlineResultCode::NotEligible;
  }
  if (!completedAt.valid() || !lootDeadlineTick_.has_value() ||
      completedAt.logicalTick < *lootDeadlineTick_) {
    return LootDeadlineResultCode::NotEligible;
  }
  observeBattleTime(completedAt);
  for (auto &record : drops_) {
    if (record.state == LootDropState::Available) {
      record.state = LootDropState::Unclaimed;
    }
  }
  lootResolution_ = LootResolutionState::Resolved;
  lootDeadlineTick_.reset();
  commitResultIfReady(completedAt);
  return LootDeadlineResultCode::Ok;
}

LootDeadlineResultCode
BattleInstance::expireLoot(const LootDeadlineCommand &command,
                           std::chrono::steady_clock::time_point completedAt) {
  return expireLoot(command, battleTimeFromLiveClock(completedAt));
}

LootDeadlineResultCode
BattleInstance::expireLoot(const LootDeadlineCommand &command) {
  const auto completedAt = lootDeadlineTick_.has_value()
                               ? BattleTime::fromLogicalTick(*lootDeadlineTick_)
                               : battleTime_;
  return expireLoot(command, completedAt);
}

std::optional<CombatProjection> BattleInstance::combatProjection() const {
  if (!monster_.has_value()) {
    return std::nullopt;
  }
  return CombatProjection{
      .battleId = battleId_,
      .monsterId = monster_->id(),
      .hitPoints = monster_->hitPoints(),
      .monsterState = monster_->state(),
      .outcome = combatTerminal_.has_value() ? combatTerminal_->outcome
                                             : CombatOutcome::None,
      .terminal = combatTerminal_,
      .serverTick = lastIntegratedServerTick_.value_or(0),
  };
}

BattleLoadProjection BattleInstance::projection() const {
  std::vector<LoadCandidateProjection> candidateProjections;
  candidateProjections.reserve(candidates_.size());
  for (const auto &candidate : candidates_) {
    candidateProjections.push_back(LoadCandidateProjection{
        .sessionId = candidate.identity.sessionId,
        .generation = candidate.identity.generation,
        .state = candidate.state,
    });
  }
  return BattleLoadProjection{
      .roomId = roomId_,
      .battleId = battleId_,
      .state = state_,
      .candidates = std::move(candidateProjections),
      .capturedParticipants = capturedParticipants_,
  };
}

void BattleInstance::markLootResultsCompleted(BattleTime completedAt) {
  if (lootResultsCompletedAtNanos_.has_value()) {
    return;
  }
  lootResultsCompletedAtNanos_ = completedAt.battleElapsedNanos;
  for (auto &record : participantRecords_) {
    record.lootResults.markBattleCompleted(completedAt.battleElapsedNanos);
  }
}

void BattleInstance::commitResultIfReady(BattleTime completedAt) {
  if (resultState_ != BattleResultState::NotReady) {
    return;
  }
  BattleOutcome outcome = BattleOutcome::MonsterDefeated;
  if (battleTerminalOutcome_.has_value()) {
    // Source 1: a Battle cancellation already stored as the terminal outcome
    // by the first all-active-participants-exited transition. It commits with
    // the stored outcome and never creates a CombatTerminalRecord.
    outcome = *battleTerminalOutcome_;
  } else if (combatTerminal_.has_value()) {
    // Source 2: a normal combat terminal. MonsterDefeated commits only after
    // loot resolution is Resolved; while it is NotStarted or Open the result
    // stays NotReady. CombatTimeout commits immediately. The CombatOutcome is
    // explicitly converted to the Battle outcome only at this commit point.
    if (combatTerminal_->outcome == CombatOutcome::MonsterDefeated &&
        lootResolution_ != LootResolutionState::Resolved) {
      return;
    }
    outcome = combatTerminal_->outcome == CombatOutcome::MonsterDefeated
                  ? BattleOutcome::MonsterDefeated
                  : BattleOutcome::CombatTimeout;
  } else {
    return;
  }
  const auto loot = lootProjection();
  const auto built = buildFinalResult(
      ResultBuildSource{
          .roomId = roomId_,
          .battleId = battleId_,
          .outcome = outcome,
          .captured = capturedParticipants_,
          .drops = loot.drops,
          .holdings = loot.holdings,
      },
      RelicCatalog::v1Snapshot());
  if (built.status != ResultBuildStatus::Built || !built.result.has_value()) {
    resultState_ = BattleResultState::ResultGenerationFailed;
    loadDeadlineTick_.reset();
    combatDeadlineTick_.reset();
    lootDeadlineTick_.reset();
    markLootResultsCompleted(completedAt);
    return;
  }
  committedResult_ = built.result;
  resultState_ = BattleResultState::Committed;
  loadDeadlineTick_.reset();
  combatDeadlineTick_.reset();
  lootDeadlineTick_.reset();
  // The Battle-owned terminal outcome source is set exactly once; the first
  // committed terminal wins and later cancellation can never rewrite it. For a
  // normal commit the CombatOutcome is converted here; for a cancellation it
  // is the already-stored source.
  battleTerminalOutcome_ = outcome;
  // The current captured/participant exit projection is mutated to its
  // terminal statuses consistently, and only after the complete result built
  // successfully. A cancellation keeps the sticky Packet A exit statuses
  // (VoluntaryLeft/Disconnected) and never rewrites the captured projection;
  // only the committed immutable entries carry TerminalExited.
  if (outcome != BattleOutcome::CancelledNoActiveParticipants) {
    for (auto &captured : capturedParticipants_) {
      captured.exitStatus = terminalExitStatus(captured.exitStatus);
    }
    for (auto &record : participantRecords_) {
      record.inputEnabled = false;
      record.exitStatus = terminalExitStatus(record.exitStatus);
    }
  }
  markLootResultsCompleted(completedAt);
}

BattleResultProjection BattleInstance::resultProjection() const {
  return BattleResultProjection{
      .state = resultState_,
      .result = committedResult_,
  };
}

std::optional<RetainedLootResults> BattleInstance::retainedLootResults() const {
  if (!lootResultsCompletedAtNanos_.has_value()) {
    return std::nullopt;
  }
  std::vector<LootResultStore> stores;
  stores.reserve(participantRecords_.size());
  for (const auto &record : participantRecords_) {
    stores.push_back(record.lootResults);
  }
  return RetainedLootResults{battleId_, *lootResultsCompletedAtNanos_,
                             std::nullopt, std::move(stores)};
}

BattleDeterministicState BattleInstance::exportDeterministicState() const {
  BattleDeterministicState state{
      .roomId = roomId_,
      .battleId = battleId_,
      .rulesetVersion = rulesetVersion_,
      .seed = seed_,
      .battleTime = battleTime_,
      .state = state_,
      .candidates = {},
      .capturedParticipants = {},
      .participants = {},
      .loadDeadlineTick = loadDeadlineTick_,
      .combatDeadlineTick = combatDeadlineTick_,
      .lootDeadlineTick = lootDeadlineTick_,
      .lastIntegratedServerTick = lastIntegratedServerTick_,
      .nextSnapshotSequence = nextSnapshotSequence_,
      .nextAttackAppliedSequence = nextAttackAppliedSequence_,
      .monster = std::nullopt,
      .combatTerminal = combatTerminal_,
      .battleTerminalOutcome = battleTerminalOutcome_,
      .drops = {},
      .holdings = {},
      .lootResolution = lootResolution_,
      .resultState = resultState_,
      .committedResult = std::nullopt,
      .lootResultsCompletedAtNanos = lootResultsCompletedAtNanos_,
  };

  state.candidates.reserve(candidates_.size());
  for (std::size_t index = 0; index < candidates_.size(); ++index) {
    state.candidates.push_back(BattleDeterministicCandidateState{
        .slot = static_cast<ParticipantSlot>(index + 1),
        .sessionId = candidates_[index].identity.sessionId,
        .state = candidates_[index].state,
    });
  }

  state.capturedParticipants.reserve(capturedParticipants_.size());
  for (const auto &captured : capturedParticipants_) {
    state.capturedParticipants.push_back(
        BattleDeterministicCapturedParticipantState{
            .slot = slotForSession(captured.sessionId),
            .sessionId = captured.sessionId,
            .exitStatus = captured.exitStatus,
        });
  }

  state.participants.reserve(participantRecords_.size());
  for (const auto &participant : participantRecords_) {
    state.participants.push_back(BattleDeterministicParticipantState{
        .slot = participant.slot,
        .sessionId = participant.sessionId,
        .posXMillimeter = participant.posXMillimeter,
        .posYMillimeter = participant.posYMillimeter,
        .tickDeltaXMillimeter = participant.tickDeltaXMillimeter,
        .tickDeltaYMillimeter = participant.tickDeltaYMillimeter,
        .latestSeenActionSequence = participant.latestSeenActionSequence,
        .lastRateUpdateNanos = participant.lastRateUpdateNanos,
        .rateCreditUnits = participant.rateCreditUnits,
        .lastAttackRateUpdateNanos = participant.lastAttackRateUpdateNanos,
        .attackRateCreditUnits = participant.attackRateCreditUnits,
        .lastClaimRateUpdateNanos = participant.lastClaimRateUpdateNanos,
        .claimRateCreditUnits = participant.claimRateCreditUnits,
        .lastAcceptedAttackNanos = participant.lastAcceptedAttackNanos,
        .attackResults = participant.attackResults.exportState(),
        .lootResults = participant.lootResults.exportState(),
        .gameplayEligible = participant.gameplayEligible,
        .inputEnabled = participant.inputEnabled,
        .exitStatus = participant.exitStatus,
    });
  }

  if (monster_.has_value()) {
    state.monster = BattleDeterministicMonsterState{
        .id = monster_->id(),
        .position = monster_->position(),
        .hitPoints = monster_->hitPoints(),
        .state = monster_->state(),
    };
  }

  state.drops.reserve(drops_.size());
  for (const auto &drop : drops_) {
    const auto ownerSlot = drop.owner.has_value() ? slotForSession(*drop.owner)
                                                  : systemParticipantSlot;
    state.drops.push_back(BattleDeterministicDropState{
        .drop = drop.drop,
        .state = drop.state,
        .ownerSlot = ownerSlot == systemParticipantSlot
                         ? std::nullopt
                         : std::optional<ParticipantSlot>{ownerSlot},
    });
  }

  state.holdings.reserve(holdings_.size());
  for (const auto &holding : holdings_) {
    state.holdings.push_back(BattleDeterministicHoldingState{
        .ownerSlot = slotForSession(holding.sessionId),
        .itemId = holding.itemId,
        .quantity = holding.quantity,
    });
  }
  std::sort(state.holdings.begin(), state.holdings.end(),
            [](const auto &lhs, const auto &rhs) {
              if (lhs.ownerSlot != rhs.ownerSlot) {
                return lhs.ownerSlot < rhs.ownerSlot;
              }
              return lhs.itemId.value < rhs.itemId.value;
            });

  if (committedResult_.has_value()) {
    BattleDeterministicFinalResult result{
        .outcome = committedResult_->outcome,
        .entries = {},
    };
    result.entries.reserve(committedResult_->entries.size());
    for (const auto &entry : committedResult_->entries) {
      result.entries.push_back(BattleDeterministicResultEntry{
          .slot = slotForSession(entry.sessionId),
          .exitStatus = entry.exitStatus,
          .finalAssetValue = entry.finalAssetValue,
          .rank = entry.rank,
          .isTop = entry.isTop,
      });
    }
    state.committedResult = std::move(result);
  }
  return state;
}

BattleStateImportResultCode BattleInstance::importDeterministicState(
    const BattleDeterministicState &state) {
  if (state.roomId != roomId_ || state.battleId != battleId_) {
    return BattleStateImportResultCode::IdentityMismatch;
  }
  if (state.rulesetVersion != rulesetVersion_ ||
      state.rulesetVersion != battleRulesetVersion || state.seed != seed_) {
    return BattleStateImportResultCode::RulesetMismatch;
  }
  if (!validLoadStateValue(state.state) ||
      !validLootResolutionStateValue(state.lootResolution) ||
      !validResultStateValue(state.resultState)) {
    return BattleStateImportResultCode::InvalidArgument;
  }
  if (state.seed == 0 || !state.battleTime.valid() ||
      state.nextSnapshotSequence == 0 || state.nextAttackAppliedSequence == 0 ||
      state.candidates.size() != candidates_.size() ||
      state.candidates.size() < minimumCandidates ||
      state.candidates.size() > maximumCandidates) {
    return BattleStateImportResultCode::InvalidArgument;
  }
  if (state.state == BattleLoadState::LoadBarrierOpen &&
      !state.loadDeadlineTick.has_value()) {
    return BattleStateImportResultCode::InvariantBroken;
  }
  if (state.state == BattleLoadState::GameplayCommitted &&
      state.resultState == BattleResultState::NotReady) {
    if (!state.combatTerminal.has_value() &&
        !state.combatDeadlineTick.has_value()) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    if (state.combatTerminal.has_value() &&
        state.combatTerminal->outcome == CombatOutcome::MonsterDefeated &&
        state.lootResolution == LootResolutionState::Open &&
        !state.lootDeadlineTick.has_value()) {
      return BattleStateImportResultCode::InvariantBroken;
    }
  }

  const auto validCandidateSlot = [this](ParticipantSlot slot) noexcept {
    return slot != systemParticipantSlot &&
           static_cast<std::size_t>(slot) <= candidates_.size();
  };
  std::vector<LoadCandidateState> candidateStates(candidates_.size());
  std::vector<bool> candidateSeen(candidates_.size(), false);
  for (const auto &candidate : state.candidates) {
    if (!validCandidateSlot(candidate.slot) ||
        candidate.sessionId.value() == 0 ||
        !validCandidateStateValue(candidate.state)) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    const auto index = static_cast<std::size_t>(candidate.slot - 1);
    if (candidateSeen[index] ||
        candidates_[index].identity.sessionId != candidate.sessionId) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    candidateSeen[index] = true;
    candidateStates[index] = candidate.state;
  }
  if (std::ranges::find(candidateSeen, false) != candidateSeen.end()) {
    return BattleStateImportResultCode::InvariantBroken;
  }
  const auto allCandidatesTerminal =
      std::ranges::all_of(candidateStates, [](LoadCandidateState value) {
        return value != LoadCandidateState::PendingLoad;
      });
  const auto readyCandidateCount = static_cast<std::size_t>(
      std::count(candidateStates.begin(), candidateStates.end(),
                 LoadCandidateState::Ready));
  switch (state.state) {
  case BattleLoadState::Created:
    if (allCandidatesTerminal || readyCandidateCount != 0U) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    break;
  case BattleLoadState::LoadBarrierOpen:
    if (allCandidatesTerminal) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    break;
  case BattleLoadState::GameplayCommitted:
    if (!allCandidatesTerminal || readyCandidateCount < minimumCandidates) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    break;
  case BattleLoadState::LoadCancelled:
    if (!allCandidatesTerminal || readyCandidateCount >= minimumCandidates) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    break;
  }

  std::vector<BattleDeterministicCapturedParticipantState> captured =
      state.capturedParticipants;
  std::sort(
      captured.begin(), captured.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.slot < rhs.slot; });
  const auto validParticipantSlot = [](ParticipantSlot slot,
                                       std::size_t participantCount) noexcept {
    return slot != systemParticipantSlot &&
           static_cast<std::size_t>(slot) <= participantCount;
  };
  std::vector<bool> capturedSlotSeen(captured.size(), false);
  std::vector<bool> capturedCandidateSeen(candidates_.size(), false);
  std::vector<CapturedParticipant> newCaptured;
  newCaptured.reserve(captured.size());
  for (const auto &entry : captured) {
    if (!validParticipantSlot(entry.slot, captured.size()) ||
        !validExitStatusValue(entry.exitStatus)) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    const auto slotIndex = static_cast<std::size_t>(entry.slot - 1);
    const auto candidate = std::find_if(
        candidates_.begin(), candidates_.end(), [&](const auto &item) {
          return item.identity.sessionId == entry.sessionId;
        });
    if (capturedSlotSeen[slotIndex] || candidate == candidates_.end()) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    const auto candidateIndex =
        static_cast<std::size_t>(std::distance(candidates_.begin(), candidate));
    if (capturedCandidateSeen[candidateIndex] ||
        candidateStates[candidateIndex] != LoadCandidateState::Ready) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    capturedSlotSeen[slotIndex] = true;
    capturedCandidateSeen[candidateIndex] = true;
    const auto &identity = candidate->identity;
    newCaptured.push_back(CapturedParticipant{
        .accountId = identity.accountId,
        .sessionId = identity.sessionId,
        .generation = identity.generation,
        .nickname = identity.nickname,
        .exitStatus = entry.exitStatus,
    });
  }
  if (std::ranges::find(capturedSlotSeen, false) != capturedSlotSeen.end()) {
    return BattleStateImportResultCode::InvariantBroken;
  }
  if (state.state == BattleLoadState::GameplayCommitted) {
    for (std::size_t index = 0; index < candidateStates.size(); ++index) {
      if (candidateStates[index] == LoadCandidateState::Ready &&
          !capturedCandidateSeen[index]) {
        return BattleStateImportResultCode::InvariantBroken;
      }
    }
  } else if (!newCaptured.empty()) {
    return BattleStateImportResultCode::InvariantBroken;
  }

  std::vector<BattleDeterministicParticipantState> participants =
      state.participants;
  std::sort(
      participants.begin(), participants.end(),
      [](const auto &lhs, const auto &rhs) { return lhs.slot < rhs.slot; });
  if (participants.size() != newCaptured.size()) {
    return BattleStateImportResultCode::InvariantBroken;
  }
  std::vector<bool> participantSlotSeen(participants.size(), false);
  std::vector<ParticipantRecord> newParticipants;
  newParticipants.reserve(participants.size());
  for (const auto &entry : participants) {
    if (!validParticipantSlot(entry.slot, participants.size()) ||
        !validExitStatusValue(entry.exitStatus)) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    const auto slotIndex = static_cast<std::size_t>(entry.slot - 1);
    if (participantSlotSeen[slotIndex] ||
        newCaptured[slotIndex].sessionId != entry.sessionId) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    const auto candidate = std::find_if(
        candidates_.begin(), candidates_.end(), [&](const auto &item) {
          return item.identity.sessionId == entry.sessionId;
        });
    if (candidate == candidates_.end()) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    participantSlotSeen[slotIndex] = true;
    const auto &identity = candidate->identity;
    ParticipantRecord record{
        .slot = entry.slot,
        .sessionId = identity.sessionId,
        .generation = identity.generation,
        .posXMillimeter = entry.posXMillimeter,
        .posYMillimeter = entry.posYMillimeter,
        .tickDeltaXMillimeter = entry.tickDeltaXMillimeter,
        .tickDeltaYMillimeter = entry.tickDeltaYMillimeter,
        .latestSeenActionSequence = entry.latestSeenActionSequence,
        .lastRateUpdateNanos = entry.lastRateUpdateNanos,
        .rateCreditUnits = entry.rateCreditUnits,
        .lastAttackRateUpdateNanos = entry.lastAttackRateUpdateNanos,
        .attackRateCreditUnits = entry.attackRateCreditUnits,
        .lastClaimRateUpdateNanos = entry.lastClaimRateUpdateNanos,
        .claimRateCreditUnits = entry.claimRateCreditUnits,
        .lastAcceptedAttackNanos = entry.lastAcceptedAttackNanos,
        .attackResults = AttackResultStore{identity.sessionId,
                                           identity.generation, battleId_},
        .lootResults =
            LootResultStore{identity.sessionId, identity.generation, battleId_},
        .gameplayEligible = entry.gameplayEligible,
        .inputEnabled = entry.inputEnabled,
        .exitStatus = entry.exitStatus,
    };
    if ((entry.attackResults.battleCompletedAtNanos.has_value() &&
         *entry.attackResults.battleCompletedAtNanos >
             state.battleTime.battleElapsedNanos) ||
        (entry.lootResults.battleCompletedAtNanos.has_value() &&
         *entry.lootResults.battleCompletedAtNanos >
             state.battleTime.battleElapsedNanos)) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    if (!record.attackResults.importState(entry.attackResults) ||
        !record.lootResults.importState(entry.lootResults)) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    newParticipants.push_back(std::move(record));
  }
  if (std::ranges::find(participantSlotSeen, false) !=
          participantSlotSeen.end() &&
      !participants.empty()) {
    return BattleStateImportResultCode::InvariantBroken;
  }
  if (state.state == BattleLoadState::GameplayCommitted &&
      newParticipants.empty()) {
    return BattleStateImportResultCode::InvariantBroken;
  }
  if (state.state != BattleLoadState::GameplayCommitted &&
      !newParticipants.empty()) {
    return BattleStateImportResultCode::InvariantBroken;
  }

  std::optional<Monster> newMonster;
  if (state.monster.has_value()) {
    if (state.monster->id != CombatRuleset::monsterId ||
        !validMonsterStateValue(state.monster->state)) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    newMonster =
        Monster::restore(state.monster->id, state.monster->position,
                         state.monster->hitPoints, state.monster->state);
  }
  if ((state.state == BattleLoadState::GameplayCommitted) !=
      newMonster.has_value()) {
    return BattleStateImportResultCode::InvariantBroken;
  }
  if (state.combatTerminal.has_value() &&
      (state.combatTerminal->battleId != battleId_ ||
       state.combatTerminal->monsterId != CombatRuleset::monsterId ||
       !validCombatOutcomeValue(state.combatTerminal->outcome) ||
       state.combatTerminal->outcome == CombatOutcome::None ||
       state.combatTerminal->rulesetVersion != CombatRuleset::version)) {
    return BattleStateImportResultCode::InvariantBroken;
  }
  if (state.combatTerminal.has_value()) {
    if (!state.monster.has_value()) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    const auto outcome = state.combatTerminal->outcome;
    if ((outcome == CombatOutcome::MonsterDefeated &&
         state.monster->state != MonsterState::Dead) ||
        (outcome == CombatOutcome::CombatTimeout &&
         state.monster->state != MonsterState::TimedOut)) {
      return BattleStateImportResultCode::InvariantBroken;
    }
  } else if (state.state == BattleLoadState::GameplayCommitted &&
             (!state.monster.has_value() ||
              state.monster->state != MonsterState::Alive)) {
    return BattleStateImportResultCode::InvariantBroken;
  }
  if (state.battleTerminalOutcome.has_value() &&
      !validBattleOutcomeValue(*state.battleTerminalOutcome)) {
    return BattleStateImportResultCode::InvariantBroken;
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
      return BattleStateImportResultCode::InvariantBroken;
    }
  }
  if (state.lootResolution == LootResolutionState::Open &&
      (!state.combatTerminal.has_value() ||
       state.combatTerminal->outcome != CombatOutcome::MonsterDefeated)) {
    return BattleStateImportResultCode::InvariantBroken;
  }
  if (state.lootResolution == LootResolutionState::Resolved &&
      (!state.combatTerminal.has_value() &&
       state.battleTerminalOutcome !=
           std::optional<BattleOutcome>{
               BattleOutcome::CancelledNoActiveParticipants})) {
    return BattleStateImportResultCode::InvariantBroken;
  }
  if (state.lootResolution == LootResolutionState::Resolved &&
      state.combatTerminal.has_value() &&
      state.combatTerminal->outcome != CombatOutcome::MonsterDefeated) {
    return BattleStateImportResultCode::InvariantBroken;
  }
  if (state.loadDeadlineTick.has_value() !=
      (state.state == BattleLoadState::LoadBarrierOpen)) {
    return BattleStateImportResultCode::InvariantBroken;
  }
  const auto combatDeadlineExpected =
      state.state == BattleLoadState::GameplayCommitted &&
      state.resultState == BattleResultState::NotReady &&
      !state.combatTerminal.has_value();
  if (state.combatDeadlineTick.has_value() != combatDeadlineExpected) {
    return BattleStateImportResultCode::InvariantBroken;
  }
  const auto lootDeadlineExpected =
      state.state == BattleLoadState::GameplayCommitted &&
      state.resultState == BattleResultState::NotReady &&
      state.lootResolution == LootResolutionState::Open;
  if (state.lootDeadlineTick.has_value() != lootDeadlineExpected) {
    return BattleStateImportResultCode::InvariantBroken;
  }

  std::vector<LootDropRecord> newDrops;
  newDrops.reserve(state.drops.size());
  const auto importedSessionForSlot =
      [&newParticipants](
          ParticipantSlot slot) -> std::optional<shared::SessionId> {
    const auto participant =
        std::find_if(newParticipants.begin(), newParticipants.end(),
                     [slot](const ParticipantRecord &record) {
                       return record.slot == slot;
                     });
    if (participant == newParticipants.end()) {
      return std::nullopt;
    }
    return participant->sessionId;
  };
  for (const auto &entry : state.drops) {
    if (entry.drop.dropId.value == 0 || entry.drop.itemId.value == 0 ||
        entry.drop.quantity == 0 || !validLootDropStateValue(entry.state)) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    if ((entry.state == LootDropState::Claimed) !=
        entry.ownerSlot.has_value()) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    if (entry.ownerSlot.has_value()) {
      if (!validParticipantSlot(*entry.ownerSlot, newParticipants.size()) ||
          !importedSessionForSlot(*entry.ownerSlot).has_value()) {
        return BattleStateImportResultCode::InvariantBroken;
      }
    }
    const auto duplicate =
        std::ranges::find_if(newDrops, [&entry](const LootDropRecord &drop) {
          return drop.drop.dropId == entry.drop.dropId;
        });
    if (duplicate != newDrops.end()) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    newDrops.push_back(LootDropRecord{
        .drop = entry.drop,
        .state = entry.state,
        .owner = entry.ownerSlot.has_value()
                     ? importedSessionForSlot(*entry.ownerSlot)
                     : std::nullopt,
    });
  }
  if (state.lootResolution == LootResolutionState::NotStarted) {
    if (!newDrops.empty()) {
      return BattleStateImportResultCode::InvariantBroken;
    }
  } else {
    if (newDrops.size() != newParticipants.size()) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    const auto hasAvailable =
        std::ranges::any_of(newDrops, [](const LootDropRecord &drop) {
          return drop.state == LootDropState::Available;
        });
    if ((state.lootResolution == LootResolutionState::Open) != hasAvailable) {
      return BattleStateImportResultCode::InvariantBroken;
    }
  }

  std::vector<HoldingRecord> newHoldings;
  newHoldings.reserve(state.holdings.size());
  for (const auto &entry : state.holdings) {
    if (!validParticipantSlot(entry.ownerSlot, newParticipants.size()) ||
        entry.itemId.value == 0 || entry.quantity == 0) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    const auto session = importedSessionForSlot(entry.ownerSlot);
    if (!session.has_value()) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    newHoldings.push_back(
        HoldingRecord{*session, entry.itemId, entry.quantity});
  }

  std::optional<BattleFinalResult> newResult;
  if (state.committedResult.has_value()) {
    std::vector<bool> resultSeen(newParticipants.size(), false);
    for (const auto &entry : state.committedResult->entries) {
      if (!validParticipantSlot(entry.slot, newParticipants.size()) ||
          resultSeen[entry.slot - 1]) {
        return BattleStateImportResultCode::InvariantBroken;
      }
      resultSeen[entry.slot - 1] = true;
      const auto session = importedSessionForSlot(entry.slot);
      if (!session.has_value()) {
        return BattleStateImportResultCode::InvariantBroken;
      }
    }
    if (std::ranges::find(resultSeen, false) != resultSeen.end()) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    if (!state.battleTerminalOutcome.has_value() ||
        *state.battleTerminalOutcome != state.committedResult->outcome) {
      return BattleStateImportResultCode::InvariantBroken;
    }

    std::vector<LootDropProjection> resultDrops;
    resultDrops.reserve(newDrops.size());
    for (const auto &drop : newDrops) {
      resultDrops.push_back(LootDropProjection{
          .dropId = drop.drop.dropId,
          .itemId = drop.drop.itemId,
          .quantity = drop.drop.quantity,
          .position = drop.drop.position,
          .state = drop.state,
          .owner = drop.owner,
      });
    }
    std::vector<LootHoldingProjection> resultHoldings;
    resultHoldings.reserve(newHoldings.size());
    for (const auto &holding : newHoldings) {
      resultHoldings.push_back(LootHoldingProjection{
          .sessionId = holding.sessionId,
          .itemId = holding.itemId,
          .quantity = holding.quantity,
      });
    }
    const auto rebuilt = buildFinalResult(
        ResultBuildSource{
            .roomId = roomId_,
            .battleId = battleId_,
            .outcome = *state.battleTerminalOutcome,
            .captured = newCaptured,
            .drops = std::move(resultDrops),
            .holdings = std::move(resultHoldings),
        },
        RelicCatalog::v1Snapshot());
    if (rebuilt.status != ResultBuildStatus::Built ||
        !rebuilt.result.has_value() ||
        rebuilt.result->entries.size() !=
            state.committedResult->entries.size()) {
      return BattleStateImportResultCode::InvariantBroken;
    }
    for (const auto &entry : state.committedResult->entries) {
      const auto session = importedSessionForSlot(entry.slot);
      const auto rebuiltEntry = std::ranges::find_if(
          rebuilt.result->entries, [session](const BattleResultEntry &value) {
            return session.has_value() && value.sessionId == *session;
          });
      if (rebuiltEntry == rebuilt.result->entries.end() ||
          rebuiltEntry->exitStatus != entry.exitStatus ||
          rebuiltEntry->finalAssetValue != entry.finalAssetValue ||
          rebuiltEntry->rank != entry.rank ||
          rebuiltEntry->isTop != entry.isTop) {
        return BattleStateImportResultCode::InvariantBroken;
      }
    }
    newResult = std::move(*rebuilt.result);
  }
  if ((state.resultState == BattleResultState::Committed) !=
          newResult.has_value() ||
      (state.resultState != BattleResultState::Committed &&
       newResult.has_value())) {
    return BattleStateImportResultCode::InvariantBroken;
  }
  if (state.resultState != BattleResultState::Committed &&
      state.battleTerminalOutcome.has_value()) {
    return BattleStateImportResultCode::InvariantBroken;
  }
  if (state.resultState != BattleResultState::NotReady &&
      state.state != BattleLoadState::GameplayCommitted) {
    return BattleStateImportResultCode::InvariantBroken;
  }
  if (state.lootResultsCompletedAtNanos.has_value() &&
      *state.lootResultsCompletedAtNanos >
          state.battleTime.battleElapsedNanos) {
    return BattleStateImportResultCode::InvariantBroken;
  }

  for (std::size_t index = 0; index < candidates_.size(); ++index) {
    candidates_[index].state = candidateStates[index];
  }
  capturedParticipants_ = std::move(newCaptured);
  participantRecords_ = std::move(newParticipants);
  battleTime_ = state.battleTime;
  state_ = state.state;
  loadDeadlineTick_ = state.loadDeadlineTick;
  combatDeadlineTick_ = state.combatDeadlineTick;
  lootDeadlineTick_ = state.lootDeadlineTick;
  lastIntegratedServerTick_ = state.lastIntegratedServerTick;
  nextSnapshotSequence_ = state.nextSnapshotSequence;
  nextAttackAppliedSequence_ = state.nextAttackAppliedSequence;
  monster_ = std::move(newMonster);
  combatTerminal_ = state.combatTerminal;
  battleTerminalOutcome_ = state.battleTerminalOutcome;
  drops_ = std::move(newDrops);
  holdings_ = std::move(newHoldings);
  lootResolution_ = state.lootResolution;
  resultState_ = state.resultState;
  committedResult_ = std::move(newResult);
  lootResultsCompletedAtNanos_ = state.lootResultsCompletedAtNanos;
  return BattleStateImportResultCode::Ok;
}

} // namespace lol::battle
