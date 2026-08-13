#include <lol/battle/BattleLoadApi.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
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
constexpr auto fullRateRefill = std::chrono::milliseconds{200};
constexpr std::uint64_t attackRateTokenUnits = 1'000'000'000ULL;
constexpr std::uint64_t attackRateCapacityUnits =
    CombatRuleset::attackBurst * attackRateTokenUnits;
constexpr std::uint64_t attackRateUnitsPerNanosecond =
    CombatRuleset::attackRatePerSecond;
constexpr auto fullAttackRateRefill = std::chrono::milliseconds{500};

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

} // namespace

CreateBattleResult BattleInstance::create(BattleAdmissionSnapshot admission) {
  if (!validAdmission(admission)) {
    return {BattleLoadResultCode::InvalidArgument, std::nullopt};
  }
  return {BattleLoadResultCode::Ok, BattleInstance{std::move(admission)}};
}

BattleInstance::BattleInstance(BattleAdmissionSnapshot admission)
    : roomId_(admission.roomId), battleId_(admission.battleId) {
  candidates_.reserve(admission.candidates.size());
  for (auto &candidate : admission.candidates) {
    candidates_.push_back(
        CandidateRecord{std::move(candidate), LoadCandidateState::PendingLoad});
  }
}

BattleLoadResultCode BattleInstance::openLoadBarrier() {
  if (state_ == BattleLoadState::Created) {
    state_ = BattleLoadState::LoadBarrierOpen;
    return BattleLoadResultCode::Ok;
  }
  return state_ == BattleLoadState::LoadBarrierOpen
             ? BattleLoadResultCode::Ok
             : BattleLoadResultCode::NotEligible;
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
                             bool gameplayTransportReady) {
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
  candidate->state = LoadCandidateState::Ready;
  resolveIfComplete();
  return BattleLoadResultCode::Ok;
}

BattleLoadResultCode BattleInstance::freezeExited(
    const CandidateDisconnectedCommand &command,
    ParticipantExitStatus exitStatus,
    std::chrono::steady_clock::time_point completedAt) {
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
  participant->gameplayEligible = false;
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
                           std::chrono::steady_clock::time_point completedAt) {
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
  candidate->state = LoadCandidateState::Disconnected;
  resolveIfComplete();
  return BattleLoadResultCode::Ok;
}

BattleLoadResultCode
BattleInstance::leave(const CandidateDisconnectedCommand &command,
                      std::chrono::steady_clock::time_point completedAt) {
  return freezeExited(command, ParticipantExitStatus::VoluntaryLeft,
                      completedAt);
}

BattleLoadResultCode
BattleInstance::expireLoadBarrier(const LoadBarrierDeadlineCommand &command) {
  if (command.roomId != roomId_ || command.battleId != battleId_) {
    return BattleLoadResultCode::StaleBattle;
  }
  if (state_ != BattleLoadState::LoadBarrierOpen) {
    return state_ == BattleLoadState::Created
               ? BattleLoadResultCode::NotEligible
               : BattleLoadResultCode::Ok;
  }
  for (auto &candidate : candidates_) {
    if (candidate.state == LoadCandidateState::PendingLoad) {
      candidate.state = LoadCandidateState::TimedOut;
    }
  }
  resolveIfComplete();
  return BattleLoadResultCode::Ok;
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
    return;
  }

  capturedParticipants_.reserve(static_cast<std::size_t>(readyCount));
  participantRecords_.reserve(static_cast<std::size_t>(readyCount));
  for (const auto &candidate : candidates_) {
    if (candidate.state == LoadCandidateState::Ready) {
      capturedParticipants_.push_back(CapturedParticipant{
          .accountId = candidate.identity.accountId,
          .sessionId = candidate.identity.sessionId,
          .generation = candidate.identity.generation,
          .nickname = candidate.identity.nickname,
          .exitStatus = ParticipantExitStatus::GameplayEligible,
      });
      participantRecords_.push_back(ParticipantRecord{
          .sessionId = candidate.identity.sessionId,
          .generation = candidate.identity.generation,
          .posXMillimeter = 0,
          .posYMillimeter = 0,
          .tickDeltaXMillimeter = 0,
          .tickDeltaYMillimeter = 0,
          .latestSeenActionSequence = std::nullopt,
          .lastRateUpdate = std::nullopt,
          .rateCreditUnits = rateCapacityUnits,
          .lastAttackRateUpdate = std::nullopt,
          .attackRateCreditUnits = attackRateCapacityUnits,
          .lastClaimRateUpdate = std::nullopt,
          .claimRateCreditUnits = claimRateCapacityUnits,
          .lastAcceptedAttackAt = std::nullopt,
          .attackResults =
              AttackResultStore{candidate.identity.sessionId,
                                candidate.identity.generation, battleId_},
          .lootResults =
              LootResultStore{candidate.identity.sessionId,
                              candidate.identity.generation, battleId_},
          .gameplayEligible = true,
      });
    }
  }
  monster_ = Monster::spawn();
  state_ = BattleLoadState::GameplayCommitted;
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

MovementResultCode
BattleInstance::acceptMove(const MoveCommand &command,
                           std::chrono::steady_clock::time_point receivedAt) {
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

  if (!movement->lastRateUpdate.has_value()) {
    movement->lastRateUpdate = receivedAt;
  } else if (receivedAt > *movement->lastRateUpdate) {
    const auto elapsed = std::min(
        receivedAt - *movement->lastRateUpdate,
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            fullRateRefill));
    const auto elapsedNanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    movement->rateCreditUnits = std::min(
        rateCapacityUnits, movement->rateCreditUnits +
                               static_cast<std::uint64_t>(elapsedNanoseconds) *
                                   rateUnitsPerNanosecond);
    movement->lastRateUpdate = receivedAt;
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
BattleInstance::integrateMovement(const MovementTickCommand &command) {
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
  for (auto &movement : participantRecords_) {
    if (!movement.gameplayEligible) {
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

AttackTerminalResult
BattleInstance::attack(const AttackCommand &command,
                       std::chrono::steady_clock::time_point receivedAt) {
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

  static_cast<void>(participant->attackResults.evictExpired(receivedAt));
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

  if (!participant->lastAttackRateUpdate.has_value()) {
    participant->lastAttackRateUpdate = receivedAt;
  } else if (receivedAt > *participant->lastAttackRateUpdate) {
    const auto elapsed = std::min(
        receivedAt - *participant->lastAttackRateUpdate,
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            fullAttackRateRefill));
    const auto elapsedNanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    participant->attackRateCreditUnits =
        std::min(attackRateCapacityUnits,
                 participant->attackRateCreditUnits +
                     static_cast<std::uint64_t>(elapsedNanoseconds) *
                         attackRateUnitsPerNanosecond);
    participant->lastAttackRateUpdate = receivedAt;
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
  if (participant->lastAcceptedAttackAt.has_value() &&
      receivedAt < *participant->lastAcceptedAttackAt +
                       std::chrono::milliseconds{
                           CombatRuleset::perPlayerCooldownMillis}) {
    return retain(AttackResultCode::Cooldown);
  }

  const auto damage = monster_->applyAttack();
  participant->lastAcceptedAttackAt = receivedAt;
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
  // Claimable loot exists only after the actual MonsterDefeated terminal. The
  // frozen inputs (captured count 2..10, immutable v1 snapshot) make generation
  // total for a valid committed Battle; on an impossible internal invariant no
  // partial drops or fabricated holdings are created.
  const auto drops =
      generateDrops(roomId_, battleId_, RelicRuleset::version,
                    static_cast<std::uint32_t>(capturedParticipants_.size()),
                    RelicCatalog::v1Snapshot());
  if (drops.has_value()) {
    drops_.reserve(drops->size());
    for (const auto &drop : *drops) {
      // Every generated Drop starts claimable (Available) with no owner.
      drops_.push_back(LootDropRecord{.drop = drop, .owner = std::nullopt});
    }
    lootResolution_ = LootResolutionState::Open;
  } else {
    // Drop generation itself violated the frozen Task 1 invariant (impossible
    // for a valid committed Battle): no partial drops, no fabricated holdings,
    // and no empty result are committed.
    resultState_ = BattleResultState::ResultGenerationFailed;
    markLootResultsCompleted(receivedAt);
  }
  auto result = retain(AttackResultCode::Ok);
  for (auto &record : participantRecords_) {
    record.attackResults.markBattleCompleted(receivedAt);
  }
  // No commit while loot is still Open; the result becomes NotReady until the
  // resolution deadline or the final claim closes it.
  commitResultIfReady(receivedAt);
  return result;
}

CombatDeadlineResultCode BattleInstance::expireCombat(
    const CombatDeadlineCommand &command,
    std::chrono::steady_clock::time_point completedAt) {
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
  if (!monster_->timeout()) {
    return CombatDeadlineResultCode::TerminalAlreadyDecided;
  }
  combatTerminal_ = CombatTerminalRecord{
      .eventId = EventId{.high = battleId_.value(), .low = 2},
      .battleId = battleId_,
      .eventSequence = 2,
      .outcome = CombatOutcome::CombatTimeout,
      .monsterId = monster_->id(),
      .serverTick = lastIntegratedServerTick_.value_or(0),
      .rulesetVersion = CombatRuleset::version,
  };
  for (auto &record : participantRecords_) {
    record.attackResults.markBattleCompleted(completedAt);
  }
  // CombatTimeout commits immediately without Drop generation.
  commitResultIfReady(completedAt);
  return CombatDeadlineResultCode::Ok;
}

LootDeadlineResultCode
BattleInstance::expireLoot(const LootDeadlineCommand &command,
                           std::chrono::steady_clock::time_point completedAt) {
  if (command.battleId != battleId_) {
    return LootDeadlineResultCode::StaleBattle;
  }
  if (lootResolution_ != LootResolutionState::Open) {
    return lootResolution_ == LootResolutionState::Resolved
               ? LootDeadlineResultCode::ResolutionClosed
               : LootDeadlineResultCode::NotEligible;
  }
  for (auto &record : drops_) {
    if (record.state == LootDropState::Available) {
      record.state = LootDropState::Unclaimed;
    }
  }
  lootResolution_ = LootResolutionState::Resolved;
  commitResultIfReady(completedAt);
  return LootDeadlineResultCode::Ok;
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

void BattleInstance::markLootResultsCompleted(
    std::chrono::steady_clock::time_point completedAt) {
  if (lootResultsCompletedAt_.has_value()) {
    return;
  }
  lootResultsCompletedAt_ = completedAt;
  for (auto &record : participantRecords_) {
    record.lootResults.markBattleCompleted(completedAt);
  }
}

void BattleInstance::commitResultIfReady(
    std::chrono::steady_clock::time_point completedAt) {
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
    markLootResultsCompleted(completedAt);
    return;
  }
  committedResult_ = built.result;
  resultState_ = BattleResultState::Committed;
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
  if (!lootResultsCompletedAt_.has_value()) {
    return std::nullopt;
  }
  std::vector<LootResultStore> stores;
  stores.reserve(participantRecords_.size());
  for (const auto &record : participantRecords_) {
    stores.push_back(record.lootResults);
  }
  return RetainedLootResults{battleId_, *lootResultsCompletedAt_,
                             std::move(stores)};
}

} // namespace lol::battle
