#include "execution/RoomExecutionCell.hpp"

#include <lol/battle/BattleLoadApi.hpp>
#include <lol/battle/CombatApi.hpp>
#include <lol/game_flow/GameplayTransportReadinessPort.hpp>
#include <lol/lobby_room/RoomApi.hpp>
#include <lol/runtime/DeadlineScheduler.hpp>
#include <lol/runtime/WorkerPool.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using lol::battle::ArenaLoadCompleteCommand;
using lol::battle::AttackCommand;
using lol::battle::AttackResultCode;
using lol::battle::BattleAdmissionSnapshot;
using lol::battle::BattleInstance;
using lol::battle::BattleLoadProjection;
using lol::battle::BattleLoadResultCode;
using lol::battle::BattleStartCandidate;
using lol::battle::CandidateDisconnectedCommand;
using lol::battle::ClaimLootCommand;
using lol::battle::ClaimLootResultCode;
using lol::battle::CombatDeadlineCommand;
using lol::battle::CombatDeadlineResultCode;
using lol::battle::CombatOutcome;
using lol::battle::CommandId;
using lol::battle::DirectionIntent;
using lol::battle::DropId;
using lol::battle::LootDeadlineResultCode;
using lol::battle::LootDropState;
using lol::battle::LootProjection;
using lol::battle::LootResolutionState;
using lol::battle::MonsterState;
using lol::battle::MoveCommand;
using lol::battle::MovementResultCode;
using lol::battle::MovementTickCommand;
using lol::battle::ParticipantExitStatus;
using lol::game_flow::GameplayTransportReadinessPort;
using lol::game_flow::execution::ConfirmedDisconnectCommand;
using lol::game_flow::execution::RoomCellCommand;
using lol::game_flow::execution::RoomCommandAdmission;
using lol::game_flow::execution::RoomCommandEnvelope;
using lol::game_flow::execution::RoomCommandKind;
using lol::game_flow::execution::RoomCommandOutcome;
using lol::game_flow::execution::RoomControlCommand;
using lol::game_flow::execution::RoomControlEnvelope;
using lol::game_flow::execution::RoomExecutionCell;
using lol::game_flow::execution::WorkBudget;
using lol::lobby_room::CreateRoomCommand;
using lol::lobby_room::HostStartEligibilityCommand;
using lol::lobby_room::JoinRoomCommand;
using lol::lobby_room::LeaveRoomCommand;
using lol::lobby_room::Room;
using lol::lobby_room::RoomMemberIdentity;
using lol::lobby_room::RoomResultCode;
using lol::lobby_room::SetReadyCommand;
using lol::runtime::DeadlineScheduler;
using lol::runtime::WorkerPool;
using lol::runtime::WorkerPoolConfig;
using lol::shared::AccountId;
using lol::shared::BattleInstanceId;
using lol::shared::RequestId;
using lol::shared::RoomId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;

constexpr auto kStart = std::chrono::steady_clock::time_point{};

AccountId account(std::uint64_t suffix) {
  AccountId::Bytes bytes{};
  bytes.back() = static_cast<std::uint8_t>(suffix);
  return AccountId{bytes};
}

BattleStartCandidate candidate(std::uint64_t sessionId) {
  return BattleStartCandidate{
      .accountId = account(sessionId),
      .sessionId = SessionId{sessionId},
      .generation = SessionGeneration{1},
      .nickname = "player-" + std::to_string(sessionId),
  };
}

BattleInstance committedBattle(std::size_t participantCount) {
  std::vector<BattleStartCandidate> candidates;
  for (std::size_t index = 1; index <= participantCount; ++index) {
    candidates.push_back(candidate(index));
  }
  auto created = BattleInstance::create(BattleAdmissionSnapshot{
      .roomId = RoomId{7},
      .battleId = BattleInstanceId{1},
      .candidates = std::move(candidates),
  });
  if (created.code != BattleLoadResultCode::Ok || !created.battle.has_value() ||
      created.battle->openLoadBarrier() != BattleLoadResultCode::Ok) {
    std::abort();
  }
  for (std::size_t index = 1; index <= participantCount; ++index) {
    if (created.battle->completeLoad(
            ArenaLoadCompleteCommand{
                .sessionId = SessionId{index},
                .generation = SessionGeneration{1},
                .roomId = RoomId{7},
                .battleId = BattleInstanceId{1},
            },
            true) != BattleLoadResultCode::Ok) {
      std::abort();
    }
  }
  return std::move(*created.battle);
}

AttackCommand attack(std::uint64_t commandId, std::uint64_t sessionId,
                     std::uint64_t target = 1) {
  return AttackCommand{
      .commandId = CommandId{.high = 0, .low = commandId},
      .sessionId = SessionId{sessionId},
      .generation = SessionGeneration{1},
      .battleId = BattleInstanceId{1},
      .targetHint = target,
  };
}

bool reduceToTwentyHitPoints(BattleInstance &battle,
                             std::uint64_t &nextCommandId) {
  for (std::uint32_t round = 0; round < 26; ++round) {
    for (std::uint64_t session = 1; session <= 3; ++session) {
      const auto result = battle.attack(attack(nextCommandId++, session),
                                        kStart + round * 750ms);
      if (result.code != AttackResultCode::Ok ||
          result.outcome != CombatOutcome::None) {
        return false;
      }
    }
  }
  const auto result =
      battle.attack(attack(nextCommandId++, 1), kStart + 26 * 750ms);
  const auto projection = battle.combatProjection();
  return result.code == AttackResultCode::Ok &&
         result.remainingHitPoints == 20 && projection.has_value() &&
         projection->hitPoints == 20 &&
         projection->monsterState == MonsterState::Alive;
}

bool nonlethalReplayAndExitedParticipantAreStable() {
  auto battle = committedBattle(2);
  const auto first = attack(1, 1);
  const auto accepted = battle.attack(first, kStart);
  const auto replay = battle.attack(first, kStart + 1ms);
  const auto conflict = battle.attack(attack(1, 1, 2), kStart + 2ms);
  if (accepted.code != AttackResultCode::Ok || accepted != replay ||
      conflict.code != AttackResultCode::CommandConflict ||
      accepted.remainingHitPoints != 1580 ||
      battle.combatProjection()->hitPoints != 1580) {
    return false;
  }

  if (battle.disconnect(CandidateDisconnectedCommand{
          .sessionId = SessionId{2},
          .generation = SessionGeneration{1},
          .roomId = RoomId{7},
          .battleId = BattleInstanceId{1},
      }) != BattleLoadResultCode::Ok) {
    return false;
  }
  const auto rejected = battle.attack(attack(2, 2), kStart);
  const auto projection = battle.combatProjection();
  return rejected.code == AttackResultCode::NotEligible &&
         rejected.remainingHitPoints == 1580 && projection.has_value() &&
         projection->hitPoints == 1580 && !projection->terminal.has_value();
}

bool validationAndCapacityPrecedeDamage() {
  auto stale = committedBattle(2);
  auto wrongBattle = attack(1, 1);
  wrongBattle.battleId = BattleInstanceId{2};
  auto wrongGeneration = attack(2, 1);
  wrongGeneration.generation = SessionGeneration{2};
  if (stale.attack(wrongBattle, kStart).code != AttackResultCode::StaleBattle ||
      stale.attack(wrongGeneration, kStart).code !=
          AttackResultCode::StaleSession ||
      stale.combatProjection()->hitPoints != 1600) {
    return false;
  }

  auto cooldown = committedBattle(2);
  if (cooldown.attack(attack(1, 1), kStart).code != AttackResultCode::Ok ||
      cooldown.attack(attack(2, 1), kStart + 1ms).code !=
          AttackResultCode::Cooldown ||
      cooldown.combatProjection()->hitPoints != 1580) {
    return false;
  }

  auto rateLimited = committedBattle(2);
  for (std::uint64_t commandId = 1; commandId <= 4; ++commandId) {
    if (rateLimited.attack(attack(commandId, 1, 2), kStart).code !=
        AttackResultCode::InvalidTarget) {
      return false;
    }
  }
  if (rateLimited.attack(attack(5, 1), kStart).code !=
          AttackResultCode::Overloaded ||
      rateLimited.combatProjection()->hitPoints != 1600) {
    return false;
  }

  auto outOfRange = committedBattle(2);
  if (outOfRange.acceptMove(
          MoveCommand{
              .sessionId = SessionId{2},
              .generation = SessionGeneration{1},
              .battleId = BattleInstanceId{1},
              .actionSequence = 1,
              .direction = DirectionIntent{.desiredX = 32767,
                                           .desiredY = 0,
                                           .inputFlags = 0},
          },
          kStart) != MovementResultCode::Ok) {
    return false;
  }
  for (std::uint32_t tick = 1; tick <= 13; ++tick) {
    if (outOfRange.integrateMovement(MovementTickCommand{
            .battleId = BattleInstanceId{1}, .serverTick = tick}) !=
        MovementResultCode::Ok) {
      return false;
    }
  }
  if (outOfRange.attack(attack(1, 2), kStart).code !=
          AttackResultCode::OutOfRange ||
      outOfRange.combatProjection()->hitPoints != 1600) {
    return false;
  }

  auto atCapacity = committedBattle(2);
  for (std::uint64_t commandId = 1; commandId <= 256; ++commandId) {
    if (atCapacity
            .attack(attack(commandId, 1, 2), kStart + (commandId - 1) * 125ms)
            .code != AttackResultCode::InvalidTarget) {
      return false;
    }
  }
  return atCapacity.attack(attack(257, 1), kStart + 32s).code ==
             AttackResultCode::Overloaded &&
         atCapacity.combatProjection()->hitPoints == 1600;
}

bool twoAndTenParticipantsCompleteDeterministically() {
  for (const std::size_t participantCount : {std::size_t{2}, std::size_t{10}}) {
    auto battle = committedBattle(participantCount);
    for (std::uint64_t hit = 0; hit < 80; ++hit) {
      const auto sessionId = (hit % participantCount) + 1;
      const auto round = hit / participantCount;
      const auto result =
          battle.attack(attack(hit + 1, sessionId), kStart + round * 750ms);
      if (result.code != AttackResultCode::Ok ||
          (hit < 79 && result.outcome != CombatOutcome::None) ||
          (hit == 79 && result.outcome != CombatOutcome::MonsterDefeated)) {
        return false;
      }
    }
    const auto projection = battle.combatProjection();
    if (!projection.has_value() || projection->hitPoints != 0 ||
        projection->monsterState != MonsterState::Dead ||
        projection->outcome != CombatOutcome::MonsterDefeated ||
        !projection->terminal.has_value() ||
        projection->terminal->eventId !=
            lol::battle::EventId{.high = 1, .low = 2}) {
      return false;
    }
  }
  return true;
}

bool lethalAndDeadlineKeepOneStableTerminal() {
  auto defeated = committedBattle(3);
  std::uint64_t commandId = 1;
  if (!reduceToTwentyHitPoints(defeated, commandId)) {
    return false;
  }
  const auto lethal =
      defeated.attack(attack(commandId++, 2), kStart + 26 * 750ms);
  const auto afterLethal = defeated.combatProjection();
  if (lethal.code != AttackResultCode::Ok ||
      lethal.outcome != CombatOutcome::MonsterDefeated ||
      !afterLethal.has_value() ||
      afterLethal->monsterState != MonsterState::Dead ||
      !afterLethal->terminal.has_value()) {
    return false;
  }
  const auto terminal = *afterLethal->terminal;
  const auto lateAttack =
      defeated.attack(attack(commandId, 3), kStart + 26 * 750ms);
  if (lateAttack.code != AttackResultCode::TerminalAlreadyDecided ||
      lateAttack.outcome != CombatOutcome::MonsterDefeated ||
      defeated.expireCombat(CombatDeadlineCommand{BattleInstanceId{1}},
                            kStart + 30s) !=
          CombatDeadlineResultCode::TerminalAlreadyDecided ||
      defeated.combatProjection()->terminal != terminal) {
    return false;
  }

  auto timedOut = committedBattle(2);
  if (timedOut.expireCombat(CombatDeadlineCommand{BattleInstanceId{1}},
                            kStart + 30s) != CombatDeadlineResultCode::Ok) {
    return false;
  }
  const auto timeout = timedOut.combatProjection();
  const auto afterTimeout = timedOut.attack(attack(1, 1), kStart + 30s);
  return timeout.has_value() && timeout->hitPoints == 1600 &&
         timeout->monsterState == MonsterState::TimedOut &&
         timeout->outcome == CombatOutcome::CombatTimeout &&
         timeout->terminal.has_value() &&
         afterTimeout.code == AttackResultCode::TerminalAlreadyDecided &&
         afterTimeout.outcome == CombatOutcome::CombatTimeout &&
         timedOut.combatProjection()->terminal == timeout->terminal;
}

RoomMemberIdentity member(std::uint64_t sessionId) {
  return RoomMemberIdentity{
      .accountId = account(sessionId),
      .sessionId = SessionId{sessionId},
      .generation = SessionGeneration{1},
      .nickname = "player-" + std::to_string(sessionId),
  };
}

std::optional<Room> readyRoom(std::size_t participantCount) {
  auto created = Room::create(CreateRoomCommand{
      .roomId = RoomId{7},
      .title = "combat",
      .capacity = 10,
      .creator = member(1),
  });
  if (!created.room.has_value()) {
    return std::nullopt;
  }
  for (std::size_t index = 2; index <= participantCount; ++index) {
    if (created.room->join(JoinRoomCommand{member(index)}) !=
        RoomResultCode::Ok) {
      return std::nullopt;
    }
  }
  for (std::size_t index = 1; index <= participantCount; ++index) {
    if (created.room->setReady(SetReadyCommand{
            .sessionId = SessionId{index},
            .generation = SessionGeneration{1},
            .ready = true,
        }) != RoomResultCode::Ok) {
      return std::nullopt;
    }
  }
  return std::move(created.room);
}

class ManualDeadlineScheduler final : public DeadlineScheduler {
public:
  bool scheduleAfter(std::chrono::milliseconds delay, Task task) override {
    if (delay.count() < 0 || !task) {
      return false;
    }
    pending_.push_back(Pending{.due = now_ + delay, .task = std::move(task)});
    std::stable_sort(pending_.begin(), pending_.end(),
                     [](const Pending &left, const Pending &right) {
                       return left.due < right.due;
                     });
    return true;
  }

  [[nodiscard]] std::unique_ptr<lol::runtime::DeadlineLease>
  tryReserve() override {
    const auto id = nextLeaseId_++;
    leases_.push_back(id);
    return std::make_unique<Lease>(*this, id);
  }

  void advance(std::chrono::milliseconds elapsed) {
    now_ += elapsed;
    while (!pending_.empty() && pending_.front().due <= now_) {
      auto task = std::move(pending_.front().task);
      pending_.erase(pending_.begin());
      task();
    }
  }

private:
  class Lease final : public lol::runtime::DeadlineLease {
  public:
    Lease(ManualDeadlineScheduler &owner, std::uint64_t id)
        : owner_(&owner), id_(id) {}
    ~Lease() override { owner_->release(id_); }
    [[nodiscard]] bool armAfter(std::chrono::milliseconds delay,
                                Task task) override {
      return owner_->arm(id_, delay, std::move(task));
    }
    void cancel() noexcept override { owner_->cancel(id_); }

  private:
    ManualDeadlineScheduler *owner_;
    std::uint64_t id_;
  };

  struct Pending final {
    std::chrono::milliseconds due;
    std::uint64_t leaseId{0};
    Task task;
  };

  bool arm(std::uint64_t id, std::chrono::milliseconds delay, Task task) {
    if (std::ranges::find(leases_, id) == leases_.end()) {
      return false;
    }
    cancel(id);
    if (delay.count() < 0 || !task) {
      return false;
    }
    pending_.push_back(
        Pending{.due = now_ + delay, .leaseId = id, .task = std::move(task)});
    std::stable_sort(pending_.begin(), pending_.end(),
                     [](const Pending &left, const Pending &right) {
                       return left.due < right.due;
                     });
    return true;
  }

  void cancel(std::uint64_t id) noexcept {
    std::erase_if(pending_, [id](const Pending &pending) {
      return pending.leaseId == id;
    });
  }

  void release(std::uint64_t id) noexcept {
    cancel(id);
    std::erase(leases_, id);
  }

  std::chrono::milliseconds now_{0};
  std::uint64_t nextLeaseId_{1};
  std::vector<Pending> pending_;
  std::vector<std::uint64_t> leases_;
};

class ReadyTransport final : public GameplayTransportReadinessPort {
public:
  bool isReady(SessionId, SessionGeneration) const noexcept override {
    return true;
  }
};

class OutcomeCollector final {
public:
  void add(RoomCommandOutcome outcome) {
    std::lock_guard lock{mutex_};
    outcomes_.push_back(std::move(outcome));
  }

  std::vector<RoomCommandOutcome> take() {
    std::lock_guard lock{mutex_};
    return std::exchange(outcomes_, {});
  }

private:
  std::mutex mutex_;
  std::vector<RoomCommandOutcome> outcomes_;
};

class CellScenario final {
public:
  explicit CellScenario(std::size_t participantCount)
      : workers_(WorkerPoolConfig{.threadCount = 1, .queueCapacity = 32}) {
    auto room = readyRoom(participantCount);
    if (!room.has_value()) {
      std::abort();
    }
    cell_ = RoomExecutionCell::create(
        workers_, deadlines_, std::move(*room),
        WorkBudget{.maxCommands = 64, .maxWallTime = 2ms},
        [this](RoomCommandOutcome outcome) {
          outcomes_.add(std::move(outcome));
        },
        &readiness_);
  }

  bool start(std::size_t participantCount) {
    if (cell_->enqueue(RoomCommandEnvelope{
            .requestId = RequestId{1},
            .command = RoomCellCommand{HostStartEligibilityCommand{
                .actorSessionId = SessionId{1},
                .actorGeneration = SessionGeneration{1},
            }},
        }) != RoomCommandAdmission::Accepted ||
        !cell_->waitUntilIdle(2s)) {
      return false;
    }
    outcomes_.take();
    for (std::size_t index = 1; index <= participantCount; ++index) {
      if (cell_->enqueue(RoomCommandEnvelope{
              .requestId = RequestId{10 + index},
              .command = RoomCellCommand{ArenaLoadCompleteCommand{
                  .sessionId = SessionId{index},
                  .generation = SessionGeneration{1},
                  .roomId = RoomId{7},
                  .battleId = BattleInstanceId{1},
              }},
          }) != RoomCommandAdmission::Accepted) {
        return false;
      }
    }
    if (!cell_->waitUntilIdle(2s)) {
      return false;
    }
    const auto outcomes = outcomes_.take();
    return outcomes.size() == participantCount &&
           outcomes.back().gameplayStartCommitted;
  }

  bool submitAttack(std::uint64_t commandId, std::uint64_t sessionId,
                    std::chrono::steady_clock::time_point receivedAt) {
    return cell_->enqueue(RoomCommandEnvelope{
               .requestId = std::nullopt,
               .command = RoomCellCommand{attack(commandId, sessionId)},
               .receivedAt = receivedAt,
           }) == RoomCommandAdmission::Accepted;
  }

  bool submitClaim(std::uint64_t commandLow, std::uint64_t sessionId,
                   std::uint64_t dropId,
                   std::chrono::steady_clock::time_point receivedAt = kStart) {
    return cell_->enqueue(RoomCommandEnvelope{
               .requestId = std::nullopt,
               .command = RoomCellCommand{ClaimLootCommand{
                   .commandId = CommandId{.high = 0, .low = commandLow},
                   .sessionId = SessionId{sessionId},
                   .generation = SessionGeneration{1},
                   .battleId = BattleInstanceId{1},
                   .dropId = DropId{dropId},
               }},
               .receivedAt = receivedAt,
           }) == RoomCommandAdmission::Accepted;
  }

  bool submitLeave(std::uint64_t sessionId) {
    return cell_->enqueue(RoomCommandEnvelope{
               .requestId = std::nullopt,
               .command = RoomCellCommand{LeaveRoomCommand{
                   .sessionId = SessionId{sessionId},
                   .generation = SessionGeneration{1},
               }},
           }) == RoomCommandAdmission::Accepted;
  }

  bool submitDisconnect(std::uint64_t sessionId) {
    return cell_->enqueueControl(RoomControlEnvelope{
               .command = RoomControlCommand{ConfirmedDisconnectCommand{
                   .sessionId = SessionId{sessionId},
                   .generation = SessionGeneration{1},
               }},
           }) == RoomCommandAdmission::Accepted;
  }

  bool wait() { return cell_->waitUntilIdle(2s); }
  std::vector<RoomCommandOutcome> take() { return outcomes_.take(); }
  void advance(std::chrono::milliseconds elapsed) {
    deadlines_.advance(elapsed);
  }

private:
  ManualDeadlineScheduler deadlines_;
  WorkerPool workers_;
  ReadyTransport readiness_;
  OutcomeCollector outcomes_;
  std::shared_ptr<RoomExecutionCell> cell_;
};

bool cellSerializesLethalAndDeadlineOrders() {
  CellScenario defeated{3};
  if (!defeated.start(3)) {
    return false;
  }
  std::uint64_t commandId = 1;
  for (std::uint32_t round = 0; round < 26; ++round) {
    for (std::uint64_t session = 1; session <= 3; ++session) {
      if (!defeated.submitAttack(commandId++, session,
                                 kStart + round * 750ms)) {
        return false;
      }
    }
  }
  if (!defeated.submitAttack(commandId++, 1, kStart + 26 * 750ms) ||
      !defeated.submitAttack(commandId++, 2, kStart + 26 * 750ms) ||
      !defeated.submitAttack(commandId, 3, kStart + 26 * 750ms) ||
      !defeated.wait()) {
    return false;
  }
  const auto attacks = defeated.take();
  if (attacks.size() != 81 || !attacks[79].attackResult.has_value() ||
      !attacks[80].attackResult.has_value() ||
      !attacks[79].combat.has_value() || !attacks[80].combat.has_value() ||
      attacks[79].attackResult->outcome != CombatOutcome::MonsterDefeated ||
      attacks[80].attackResult->code !=
          AttackResultCode::TerminalAlreadyDecided ||
      attacks[79].combat->terminal != attacks[80].combat->terminal) {
    return false;
  }
  defeated.advance(30s);
  if (!defeated.wait()) {
    return false;
  }
  const auto afterDeadline = defeated.take();
  const auto staleCombatDeadline = std::find_if(
      afterDeadline.begin(), afterDeadline.end(), [](const auto &outcome) {
        return outcome.kind == RoomCommandKind::CombatDeadline;
      });
  const auto lootDeadline = std::find_if(
      afterDeadline.begin(), afterDeadline.end(), [](const auto &outcome) {
        return outcome.kind == RoomCommandKind::LootDeadline;
      });
  if (staleCombatDeadline != afterDeadline.end() ||
      lootDeadline == afterDeadline.end() ||
      lootDeadline->lootDeadlineCode != LootDeadlineResultCode::Ok) {
    return false;
  }

  CellScenario timedOut{2};
  if (!timedOut.start(2)) {
    return false;
  }
  timedOut.advance(30s);
  if (!timedOut.wait()) {
    return false;
  }
  const auto deadlineFirst = timedOut.take();
  const auto timeout = std::find_if(
      deadlineFirst.begin(), deadlineFirst.end(), [](const auto &outcome) {
        return outcome.kind == RoomCommandKind::CombatDeadline;
      });
  if (timeout == deadlineFirst.end() ||
      timeout->combatDeadlineCode != CombatDeadlineResultCode::Ok ||
      !timeout->combat.has_value() ||
      timeout->combat->outcome != CombatOutcome::CombatTimeout ||
      !timedOut.submitAttack(1, 1, kStart + 30s) || !timedOut.wait()) {
    return false;
  }
  const auto lateAttack = timedOut.take();
  return lateAttack.size() == 1 &&
         lateAttack.front().attackResult.has_value() &&
         lateAttack.front().combat.has_value() &&
         lateAttack.front().attackResult->code ==
             AttackResultCode::TerminalAlreadyDecided &&
         lateAttack.front().combat->outcome == CombatOutcome::CombatTimeout;
}

// 80 fixed 20-damage hits (two participants alternating every 750 ms) empty
// the fixed 1600 HP monster through the Cell; the last hit is lethal and the
// resulting outcome is MonsterDefeated.
bool killMonsterThroughCell(CellScenario &scenario, std::uint64_t &commandId) {
  for (std::uint32_t round = 0; round < 40; ++round) {
    for (std::uint64_t session = 1; session <= 2; ++session) {
      if (!scenario.submitAttack(commandId++, session,
                                 kStart + round * 750ms)) {
        return false;
      }
    }
  }
  if (!scenario.wait()) {
    return false;
  }
  const auto attacks = scenario.take();
  return attacks.size() == 80 && attacks.back().attackResult.has_value() &&
         attacks.back().attackResult->outcome == CombatOutcome::MonsterDefeated;
}

const RoomCommandOutcome *
findOutcome(const std::vector<RoomCommandOutcome> &outcomes,
            RoomCommandKind kind) {
  const auto found = std::find_if(
      outcomes.begin(), outcomes.end(),
      [kind](const auto &outcome) { return outcome.kind == kind; });
  return found == outcomes.end() ? nullptr : &*found;
}

const RoomCommandOutcome *
findOutcomeFor(const std::vector<RoomCommandOutcome> &outcomes,
               RoomCommandKind kind, std::uint64_t sessionId) {
  const auto found = std::find_if(
      outcomes.begin(), outcomes.end(), [kind, sessionId](const auto &outcome) {
        return outcome.kind == kind &&
               outcome.actorSessionId == SessionId{sessionId};
      });
  return found == outcomes.end() ? nullptr : &*found;
}

// 79 fixed 20-damage hits (two participants alternating every 750 ms) reduce
// the fixed 1600 HP monster to exactly 20 HP without a terminal: the 80th hit
// would be lethal.
bool reduceToTwentyHitPointsThroughCell(CellScenario &scenario,
                                        std::uint64_t &commandId) {
  for (std::uint32_t round = 0; round < 39; ++round) {
    for (std::uint64_t session = 1; session <= 2; ++session) {
      if (!scenario.submitAttack(commandId++, session,
                                 kStart + round * 750ms)) {
        return false;
      }
    }
  }
  if (!scenario.submitAttack(commandId++, 1, kStart + 39 * 750ms) ||
      !scenario.wait()) {
    return false;
  }
  const auto attacks = scenario.take();
  return attacks.size() == 79 && attacks.back().attackResult.has_value() &&
         attacks.back().attackResult->code == AttackResultCode::Ok &&
         attacks.back().attackResult->remainingHitPoints == 20 &&
         attacks.back().combat.has_value() &&
         attacks.back().combat->hitPoints == 20 &&
         attacks.back().combat->monsterState == MonsterState::Alive &&
         !attacks.back().combat->terminal.has_value();
}

const lol::battle::LootDropProjection *
findDrop(const LootProjection &projection, std::uint64_t dropId) {
  for (const auto &drop : projection.drops) {
    if (drop.dropId == DropId{dropId}) {
      return &drop;
    }
  }
  return nullptr;
}

std::optional<std::uint64_t> holdingQuantity(const LootProjection &projection,
                                             std::uint64_t sessionId,
                                             std::uint64_t itemId) {
  for (const auto &holding : projection.holdings) {
    if (holding.sessionId == SessionId{sessionId} &&
        holding.itemId == lol::battle::ItemId{itemId}) {
      return holding.quantity;
    }
  }
  return std::nullopt;
}

ParticipantExitStatus exitStatusOf(const BattleLoadProjection &projection,
                                   std::uint64_t sessionId) {
  for (const auto &captured : projection.capturedParticipants) {
    if (captured.sessionId == SessionId{sessionId}) {
      return captured.exitStatus;
    }
  }
  std::abort();
}

// ClaimLoot has an independent per-participant 8/s token bucket with burst 4.
// Every terminal CommandId is retained, while replay/conflict inspection
// precedes rate admission and consumes no refilled credit.
bool cellClaimRetainsOverloadAcrossRateRefill() {
  CellScenario scenario{2};
  if (!scenario.start(2)) {
    return false;
  }
  std::uint64_t commandId = 1;
  if (!killMonsterThroughCell(scenario, commandId)) {
    return false;
  }

  const auto claimAt = kStart + 31s;
  const auto firstClaimCommand = commandId;
  for (std::uint64_t offset = 0; offset < 5; ++offset) {
    if (!scenario.submitClaim(commandId++, 1, 100 + offset, claimAt)) {
      return false;
    }
  }
  if (!scenario.wait()) {
    return false;
  }
  const auto burst = scenario.take();
  if (burst.size() != 5) {
    return false;
  }
  for (std::size_t index = 0; index < burst.size(); ++index) {
    const auto expected = index < 4 ? ClaimLootResultCode::UnknownDrop
                                    : ClaimLootResultCode::Overloaded;
    if (!burst[index].lootClaimResult.has_value() ||
        burst[index].lootClaimResult->code != expected ||
        !burst[index].loot.has_value() ||
        !burst[index].loot->holdings.empty()) {
      return false;
    }
    for (const auto &drop : burst[index].loot->drops) {
      if (drop.state != LootDropState::Available || drop.owner.has_value()) {
        return false;
      }
    }
  }

  // Zero remaining credit cannot hide an existing replay or conflict.
  if (!scenario.submitClaim(firstClaimCommand, 1, 100, claimAt) ||
      !scenario.submitClaim(firstClaimCommand, 1, 999, claimAt) ||
      !scenario.wait()) {
    return false;
  }
  const auto dedup = scenario.take();
  if (dedup.size() != 2 || !dedup[0].lootClaimResult.has_value() ||
      !dedup[1].lootClaimResult.has_value() ||
      dedup[0].lootClaimResult->code != ClaimLootResultCode::UnknownDrop ||
      dedup[1].lootClaimResult->code != ClaimLootResultCode::CommandConflict) {
    return false;
  }

  // The fifth command remains Overloaded after refill, a changed payload
  // conflicts, and only a new CommandId spends the refilled token.
  const auto fifthCommand = firstClaimCommand + 4;
  if (!scenario.submitClaim(fifthCommand, 1, 104, claimAt + 125ms) ||
      !scenario.submitClaim(fifthCommand, 1, 999, claimAt + 125ms) ||
      !scenario.submitClaim(commandId, 1, 105, claimAt + 125ms) ||
      !scenario.wait()) {
    return false;
  }
  const auto afterRefill = scenario.take();
  return afterRefill.size() == 3 &&
         afterRefill[0].lootClaimResult.has_value() &&
         afterRefill[1].lootClaimResult.has_value() &&
         afterRefill[2].lootClaimResult.has_value() &&
         afterRefill[0].lootClaimResult->code ==
             ClaimLootResultCode::Overloaded &&
         afterRefill[1].lootClaimResult->code ==
             ClaimLootResultCode::CommandConflict &&
         afterRefill[2].lootClaimResult->code ==
             ClaimLootResultCode::UnknownDrop &&
         std::all_of(afterRefill.begin(), afterRefill.end(),
                     [](const auto &outcome) {
                       if (!outcome.loot.has_value() ||
                           !outcome.loot->holdings.empty()) {
                         return false;
                       }
                       return std::all_of(
                           outcome.loot->drops.begin(),
                           outcome.loot->drops.end(), [](const auto &drop) {
                             return drop.state == LootDropState::Available &&
                                    !drop.owner.has_value();
                           });
                     });
}

// Drop 2 (Common at (-414, 819)) is in claim range from the origin; the
// admission ordinal across the external and control lanes decides which of
// ClaimLoot and the 15 s control deadline wins.
bool cellClaimVsLootDeadlineOrders() {
  // Order 1: ClaimLoot admitted first, then the control deadline.
  {
    CellScenario scenario{2};
    if (!scenario.start(2)) {
      return false;
    }
    std::uint64_t commandId = 1;
    if (!killMonsterThroughCell(scenario, commandId)) {
      return false;
    }

    if (!scenario.submitClaim(commandId++, 1, 2)) {
      return false;
    }
    scenario.advance(15s);
    if (!scenario.wait()) {
      return false;
    }
    const auto outcomes = scenario.take();
    const auto *claimOutcome =
        findOutcome(outcomes, RoomCommandKind::ClaimLoot);
    const auto *deadlineOutcome =
        findOutcome(outcomes, RoomCommandKind::LootDeadline);
    if (claimOutcome == nullptr || deadlineOutcome == nullptr ||
        claimOutcome->admissionOrdinal >= deadlineOutcome->admissionOrdinal) {
      return false;
    }
    if (!claimOutcome->lootClaimResult.has_value() ||
        claimOutcome->lootClaimResult->code != ClaimLootResultCode::Ok ||
        !claimOutcome->loot.has_value() ||
        claimOutcome->loot->resolution != LootResolutionState::Open ||
        findDrop(*claimOutcome->loot, 2)->state != LootDropState::Claimed ||
        findDrop(*claimOutcome->loot, 2)->owner != SessionId{1} ||
        findDrop(*claimOutcome->loot, 1)->state != LootDropState::Available) {
      return false;
    }
    if (!deadlineOutcome->lootDeadlineCode.has_value() ||
        *deadlineOutcome->lootDeadlineCode != LootDeadlineResultCode::Ok ||
        !deadlineOutcome->loot.has_value() ||
        deadlineOutcome->loot->resolution != LootResolutionState::Resolved ||
        findDrop(*deadlineOutcome->loot, 2)->state != LootDropState::Claimed ||
        findDrop(*deadlineOutcome->loot, 2)->owner != SessionId{1} ||
        findDrop(*deadlineOutcome->loot, 1)->state !=
            LootDropState::Unclaimed ||
        holdingQuantity(*deadlineOutcome->loot, 1, 1) !=
            std::optional<std::uint64_t>{1}) {
      return false;
    }
  }

  // Order 2: the control deadline is admitted first, then the player command.
  {
    CellScenario scenario{2};
    if (!scenario.start(2)) {
      return false;
    }
    std::uint64_t commandId = 1;
    if (!killMonsterThroughCell(scenario, commandId)) {
      return false;
    }

    scenario.advance(15s);
    if (!scenario.submitClaim(commandId++, 1, 2)) {
      return false;
    }
    if (!scenario.wait()) {
      return false;
    }
    const auto outcomes = scenario.take();
    const auto *deadlineOutcome =
        findOutcome(outcomes, RoomCommandKind::LootDeadline);
    const auto *claimOutcome =
        findOutcome(outcomes, RoomCommandKind::ClaimLoot);
    if (deadlineOutcome == nullptr || claimOutcome == nullptr ||
        deadlineOutcome->admissionOrdinal >= claimOutcome->admissionOrdinal) {
      return false;
    }
    if (!deadlineOutcome->lootDeadlineCode.has_value() ||
        *deadlineOutcome->lootDeadlineCode != LootDeadlineResultCode::Ok ||
        !deadlineOutcome->loot.has_value() ||
        deadlineOutcome->loot->resolution != LootResolutionState::Resolved ||
        findDrop(*deadlineOutcome->loot, 1)->state !=
            LootDropState::Unclaimed ||
        findDrop(*deadlineOutcome->loot, 2)->state !=
            LootDropState::Unclaimed) {
      return false;
    }
    if (!claimOutcome->lootClaimResult.has_value() ||
        claimOutcome->lootClaimResult->code !=
            ClaimLootResultCode::ResolutionClosed ||
        !claimOutcome->loot.has_value() ||
        !claimOutcome->loot->holdings.empty()) {
      return false;
    }
  }
  return true;
}

// The admission ordinal decides whether ClaimLoot or the participant exit
// wins; already-acquired holdings survive the exit and later claims reject.
bool cellClaimVsExitOrders() {
  // Order 1: ClaimLoot first, then the claimant's own voluntary Leave.
  {
    CellScenario scenario{2};
    if (!scenario.start(2)) {
      return false;
    }
    std::uint64_t commandId = 1;
    if (!killMonsterThroughCell(scenario, commandId)) {
      return false;
    }
    const auto claimCommandLow = commandId++;
    if (!scenario.submitClaim(claimCommandLow, 1, 2) ||
        !scenario.submitLeave(1) || !scenario.wait()) {
      return false;
    }
    const auto outcomes = scenario.take();
    const auto *claimOutcome =
        findOutcome(outcomes, RoomCommandKind::ClaimLoot);
    const auto *leaveOutcome = findOutcome(outcomes, RoomCommandKind::Leave);
    if (claimOutcome == nullptr || leaveOutcome == nullptr ||
        claimOutcome->admissionOrdinal >= leaveOutcome->admissionOrdinal) {
      return false;
    }
    if (!claimOutcome->lootClaimResult.has_value() ||
        claimOutcome->lootClaimResult->code != ClaimLootResultCode::Ok ||
        !leaveOutcome->battle.has_value() ||
        exitStatusOf(*leaveOutcome->battle, 1) !=
            ParticipantExitStatus::VoluntaryLeft ||
        exitStatusOf(*leaveOutcome->battle, 2) !=
            ParticipantExitStatus::GameplayEligible ||
        !leaveOutcome->loot.has_value() ||
        findDrop(*leaveOutcome->loot, 2)->owner != SessionId{1} ||
        holdingQuantity(*leaveOutcome->loot, 1, 1) !=
            std::optional<std::uint64_t>{1}) {
      return false;
    }

    // Fresh claim from the exited claimant is rejected; the original
    // accepted ClaimLoot replays identically.
    if (!scenario.submitClaim(commandId++, 1, 2) ||
        !scenario.submitClaim(claimCommandLow, 1, 2) || !scenario.wait()) {
      return false;
    }
    const auto later = scenario.take();
    if (later.size() != 2 || !later[0].lootClaimResult.has_value() ||
        !later[1].lootClaimResult.has_value() ||
        later[0].lootClaimResult->code != ClaimLootResultCode::NotEligible ||
        later[1].lootClaimResult->code != ClaimLootResultCode::Ok ||
        later[1].lootClaimResult != *claimOutcome->lootClaimResult) {
      return false;
    }
  }

  // Order 2: voluntary Leave first, then ClaimLoot.
  {
    CellScenario scenario{2};
    if (!scenario.start(2)) {
      return false;
    }
    std::uint64_t commandId = 1;
    if (!killMonsterThroughCell(scenario, commandId)) {
      return false;
    }
    if (!scenario.submitLeave(2) || !scenario.submitClaim(commandId++, 2, 2) ||
        !scenario.wait()) {
      return false;
    }
    const auto outcomes = scenario.take();
    const auto *leaveOutcome = findOutcome(outcomes, RoomCommandKind::Leave);
    const auto *claimOutcome =
        findOutcome(outcomes, RoomCommandKind::ClaimLoot);
    if (leaveOutcome == nullptr || claimOutcome == nullptr ||
        leaveOutcome->admissionOrdinal >= claimOutcome->admissionOrdinal) {
      return false;
    }
    if (!leaveOutcome->battle.has_value() ||
        exitStatusOf(*leaveOutcome->battle, 2) !=
            ParticipantExitStatus::VoluntaryLeft ||
        !claimOutcome->lootClaimResult.has_value() ||
        claimOutcome->lootClaimResult->code !=
            ClaimLootResultCode::NotEligible ||
        !claimOutcome->loot.has_value() ||
        !claimOutcome->loot->holdings.empty() ||
        findDrop(*claimOutcome->loot, 2)->state != LootDropState::Available) {
      return false;
    }
  }

  // Order 3: ClaimLoot first, then the claimant's own ConfirmedDisconnect.
  {
    CellScenario scenario{2};
    if (!scenario.start(2)) {
      return false;
    }
    std::uint64_t commandId = 1;
    if (!killMonsterThroughCell(scenario, commandId)) {
      return false;
    }
    const auto claimCommandLow = commandId++;
    if (!scenario.submitClaim(claimCommandLow, 1, 2) ||
        !scenario.submitDisconnect(1) || !scenario.wait()) {
      return false;
    }
    const auto outcomes = scenario.take();
    const auto *claimOutcome =
        findOutcome(outcomes, RoomCommandKind::ClaimLoot);
    const auto *disconnectOutcome =
        findOutcome(outcomes, RoomCommandKind::ConfirmedDisconnect);
    if (claimOutcome == nullptr || disconnectOutcome == nullptr ||
        claimOutcome->admissionOrdinal >= disconnectOutcome->admissionOrdinal) {
      return false;
    }
    if (!claimOutcome->lootClaimResult.has_value() ||
        claimOutcome->lootClaimResult->code != ClaimLootResultCode::Ok ||
        !disconnectOutcome->battle.has_value() ||
        exitStatusOf(*disconnectOutcome->battle, 1) !=
            ParticipantExitStatus::Disconnected ||
        exitStatusOf(*disconnectOutcome->battle, 2) !=
            ParticipantExitStatus::GameplayEligible ||
        !disconnectOutcome->loot.has_value() ||
        findDrop(*disconnectOutcome->loot, 2)->owner != SessionId{1} ||
        holdingQuantity(*disconnectOutcome->loot, 1, 1) !=
            std::optional<std::uint64_t>{1}) {
      return false;
    }

    if (!scenario.submitClaim(commandId++, 1, 2) ||
        !scenario.submitClaim(claimCommandLow, 1, 2) || !scenario.wait()) {
      return false;
    }
    const auto later = scenario.take();
    if (later.size() != 2 || !later[0].lootClaimResult.has_value() ||
        !later[1].lootClaimResult.has_value() ||
        later[0].lootClaimResult->code != ClaimLootResultCode::NotEligible ||
        later[1].lootClaimResult->code != ClaimLootResultCode::Ok ||
        later[1].lootClaimResult != *claimOutcome->lootClaimResult) {
      return false;
    }
  }

  // Order 4: ConfirmedDisconnect first, then ClaimLoot.
  {
    CellScenario scenario{2};
    if (!scenario.start(2)) {
      return false;
    }
    std::uint64_t commandId = 1;
    if (!killMonsterThroughCell(scenario, commandId)) {
      return false;
    }
    if (!scenario.submitDisconnect(2) ||
        !scenario.submitClaim(commandId++, 2, 2) || !scenario.wait()) {
      return false;
    }
    const auto outcomes = scenario.take();
    const auto *disconnectOutcome =
        findOutcome(outcomes, RoomCommandKind::ConfirmedDisconnect);
    const auto *claimOutcome =
        findOutcome(outcomes, RoomCommandKind::ClaimLoot);
    if (disconnectOutcome == nullptr || claimOutcome == nullptr ||
        disconnectOutcome->admissionOrdinal >= claimOutcome->admissionOrdinal) {
      return false;
    }
    if (!disconnectOutcome->battle.has_value() ||
        exitStatusOf(*disconnectOutcome->battle, 2) !=
            ParticipantExitStatus::Disconnected ||
        !claimOutcome->lootClaimResult.has_value() ||
        claimOutcome->lootClaimResult->code !=
            ClaimLootResultCode::NotEligible ||
        !claimOutcome->loot.has_value() ||
        !claimOutcome->loot->holdings.empty() ||
        findDrop(*claimOutcome->loot, 2)->state != LootDropState::Available) {
      return false;
    }
  }
  return true;
}

// Case 1: both voluntary Leaves are admitted before the otherwise lethal
// Attack. The queued Attack is explicitly NotEligible, no combat terminal is
// created, no Drop is generated, and the canceled 30 s combat deadline emits
// no later outcome.
bool lastLeaveBeforeLethalAttackCancelsWithoutTerminal() {
  CellScenario scenario{2};
  if (!scenario.start(2)) {
    return false;
  }
  std::uint64_t commandId = 1;
  if (!reduceToTwentyHitPointsThroughCell(scenario, commandId)) {
    return false;
  }
  if (!scenario.submitLeave(1) || !scenario.submitLeave(2) ||
      !scenario.submitAttack(commandId, 1, kStart + 40 * 750ms) ||
      !scenario.wait()) {
    return false;
  }
  const auto outcomes = scenario.take();
  const auto *firstLeave = findOutcomeFor(outcomes, RoomCommandKind::Leave, 1);
  const auto *lastLeave = findOutcomeFor(outcomes, RoomCommandKind::Leave, 2);
  const auto *lateAttack = findOutcome(outcomes, RoomCommandKind::Attack);
  if (firstLeave == nullptr || lastLeave == nullptr || lateAttack == nullptr ||
      firstLeave->admissionOrdinal >= lastLeave->admissionOrdinal ||
      lastLeave->admissionOrdinal >= lateAttack->admissionOrdinal) {
    return false;
  }
  if (!firstLeave->battle.has_value() ||
      exitStatusOf(*firstLeave->battle, 1) !=
          ParticipantExitStatus::VoluntaryLeft ||
      !lastLeave->battle.has_value() ||
      exitStatusOf(*lastLeave->battle, 1) !=
          ParticipantExitStatus::VoluntaryLeft ||
      exitStatusOf(*lastLeave->battle, 2) !=
          ParticipantExitStatus::VoluntaryLeft) {
    return false;
  }
  // Cancellation during NotStarted loot generates no Drop and the empty loot
  // source stays empty.
  if (!lastLeave->loot.has_value() ||
      lastLeave->loot->resolution != LootResolutionState::NotStarted ||
      !lastLeave->loot->drops.empty() || !lastLeave->loot->holdings.empty()) {
    return false;
  }
  // The queued otherwise-lethal Attack is explicitly NotEligible and the
  // combat history stays non-terminal at 20 HP.
  if (!lateAttack->attackResult.has_value() ||
      lateAttack->attackResult->code != AttackResultCode::NotEligible ||
      !lateAttack->combat.has_value() || lateAttack->combat->hitPoints != 20 ||
      lateAttack->combat->monsterState != MonsterState::Alive ||
      lateAttack->combat->outcome != CombatOutcome::None ||
      lateAttack->combat->terminal.has_value()) {
    return false;
  }
  scenario.advance(30s);
  if (!scenario.wait()) {
    return false;
  }
  const auto afterDeadline = scenario.take();
  return afterDeadline.empty();
}

// Case 2: the lethal Attack is admitted before the last voluntary Leave.
// Combat history remains MonsterDefeated, loot opens, then the last Leave
// closes every unresolved Drop Unclaimed without fabricating a winner or any
// holding.
bool lethalAttackThenLastLeaveClosesOpenLoot() {
  CellScenario scenario{2};
  if (!scenario.start(2)) {
    return false;
  }
  std::uint64_t commandId = 1;
  if (!killMonsterThroughCell(scenario, commandId)) {
    return false;
  }
  if (!scenario.submitLeave(1) || !scenario.submitLeave(2) ||
      !scenario.wait()) {
    return false;
  }
  const auto outcomes = scenario.take();
  const auto *firstLeave = findOutcomeFor(outcomes, RoomCommandKind::Leave, 1);
  const auto *lastLeave = findOutcomeFor(outcomes, RoomCommandKind::Leave, 2);
  if (firstLeave == nullptr || lastLeave == nullptr ||
      firstLeave->admissionOrdinal >= lastLeave->admissionOrdinal) {
    return false;
  }
  if (!lastLeave->battle.has_value() ||
      exitStatusOf(*lastLeave->battle, 1) !=
          ParticipantExitStatus::VoluntaryLeft ||
      exitStatusOf(*lastLeave->battle, 2) !=
          ParticipantExitStatus::VoluntaryLeft ||
      !lastLeave->loot.has_value() ||
      lastLeave->loot->resolution != LootResolutionState::Resolved ||
      !lastLeave->loot->holdings.empty() ||
      findDrop(*lastLeave->loot, 1)->state != LootDropState::Unclaimed ||
      findDrop(*lastLeave->loot, 1)->owner.has_value() ||
      findDrop(*lastLeave->loot, 2)->state != LootDropState::Unclaimed ||
      findDrop(*lastLeave->loot, 2)->owner.has_value()) {
    return false;
  }
  // A late attack from an exited participant stays NotEligible and the combat
  // projection still shows the unchanged MonsterDefeated history.
  if (!scenario.submitAttack(commandId, 1, kStart + 20s) || !scenario.wait()) {
    return false;
  }
  const auto late = scenario.take();
  if (late.size() != 1 || !late[0].attackResult.has_value() ||
      late[0].attackResult->code != AttackResultCode::NotEligible ||
      !late[0].combat.has_value()) {
    return false;
  }
  const auto &combat = *late[0].combat;
  return combat.monsterState == MonsterState::Dead &&
         combat.outcome == CombatOutcome::MonsterDefeated &&
         combat.terminal.has_value() &&
         combat.terminal->outcome == CombatOutcome::MonsterDefeated;
}

// Case 3: the last ConfirmedDisconnect is admitted before the 30 s
// CombatTimeout. The cancellation removes the queued deadline before it can
// emit an outcome, and the combat history remains non-terminal.
bool lastDisconnectBeforeCombatDeadlineFreezesTerminal() {
  CellScenario scenario{2};
  if (!scenario.start(2)) {
    return false;
  }
  if (!scenario.submitDisconnect(1) || !scenario.submitDisconnect(2)) {
    return false;
  }
  scenario.advance(30s);
  if (!scenario.wait()) {
    return false;
  }
  const auto outcomes = scenario.take();
  const auto *first =
      findOutcomeFor(outcomes, RoomCommandKind::ConfirmedDisconnect, 1);
  const auto *last =
      findOutcomeFor(outcomes, RoomCommandKind::ConfirmedDisconnect, 2);
  const auto *deadline = findOutcome(outcomes, RoomCommandKind::CombatDeadline);
  if (first == nullptr || last == nullptr || deadline != nullptr ||
      first->admissionOrdinal >= last->admissionOrdinal) {
    return false;
  }
  if (!last->battle.has_value() ||
      exitStatusOf(*last->battle, 1) != ParticipantExitStatus::Disconnected ||
      exitStatusOf(*last->battle, 2) != ParticipantExitStatus::Disconnected ||
      !last->loot.has_value() ||
      last->loot->resolution != LootResolutionState::NotStarted ||
      !last->loot->drops.empty()) {
    return false;
  }
  return outcomes.size() == 2;
}

// Case 4: the CombatTimeout control command is admitted before the last
// disconnect. The committed timeout result/projection remains unchanged after
// the exits and cancellation never rewrites the committed normal result.
bool committedTimeoutSurvivesLaterDisconnects() {
  CellScenario scenario{2};
  if (!scenario.start(2)) {
    return false;
  }
  scenario.advance(30s);
  if (!scenario.wait()) {
    return false;
  }
  const auto deadlineOutcomes = scenario.take();
  const auto *deadline =
      findOutcome(deadlineOutcomes, RoomCommandKind::CombatDeadline);
  if (deadline == nullptr || !deadline->combatDeadlineCode.has_value() ||
      *deadline->combatDeadlineCode != CombatDeadlineResultCode::Ok ||
      !deadline->combat.has_value() ||
      deadline->combat->outcome != CombatOutcome::CombatTimeout ||
      !deadline->combat->terminal.has_value()) {
    return false;
  }
  const auto committedTerminal = *deadline->combat->terminal;
  if (!scenario.submitDisconnect(1) || !scenario.submitDisconnect(2) ||
      !scenario.wait()) {
    return false;
  }
  const auto disconnects = scenario.take();
  const auto *first =
      findOutcomeFor(disconnects, RoomCommandKind::ConfirmedDisconnect, 1);
  const auto *last =
      findOutcomeFor(disconnects, RoomCommandKind::ConfirmedDisconnect, 2);
  if (first == nullptr || last == nullptr ||
      first->admissionOrdinal >= last->admissionOrdinal) {
    return false;
  }
  // Post-commit exits are sticky no-mutation notifications: the captured
  // projections keep the committed terminal statuses and the empty loot source
  // stays empty.
  if (!last->battle.has_value() ||
      exitStatusOf(*last->battle, 1) !=
          ParticipantExitStatus::TerminalPresent ||
      exitStatusOf(*last->battle, 2) !=
          ParticipantExitStatus::TerminalPresent ||
      !last->loot.has_value() || !last->loot->drops.empty() ||
      !last->loot->holdings.empty()) {
    return false;
  }
  if (!scenario.submitAttack(1, 1, kStart + 31s) || !scenario.wait()) {
    return false;
  }
  const auto late = scenario.take();
  return late.size() == 1 && late[0].attackResult.has_value() &&
         late[0].attackResult->code ==
             AttackResultCode::TerminalAlreadyDecided &&
         late[0].attackResult->outcome == CombatOutcome::CombatTimeout &&
         late[0].combat.has_value() && late[0].combat->terminal.has_value() &&
         *late[0].combat->terminal == committedTerminal;
}

// Case 5: partial loot then all exit. One actual accepted claim stays Claimed
// with the same owner and quantity, the other Available Drop becomes Unclaimed,
// both sticky exit statuses are exact, and the canceled loot deadline emits no
// later outcome.
bool partialClaimThenAllExitClosesRemainingDrops() {
  CellScenario scenario{2};
  if (!scenario.start(2)) {
    return false;
  }
  std::uint64_t commandId = 1;
  if (!killMonsterThroughCell(scenario, commandId)) {
    return false;
  }
  if (!scenario.submitClaim(commandId++, 1, 2) || !scenario.wait()) {
    return false;
  }
  const auto claimOutcomes = scenario.take();
  const auto *claim = findOutcome(claimOutcomes, RoomCommandKind::ClaimLoot);
  if (claim == nullptr || !claim->lootClaimResult.has_value() ||
      claim->lootClaimResult->code != ClaimLootResultCode::Ok) {
    return false;
  }
  if (!scenario.submitLeave(1) || !scenario.submitLeave(2) ||
      !scenario.wait()) {
    return false;
  }
  const auto exitOutcomes = scenario.take();
  const auto *last = findOutcomeFor(exitOutcomes, RoomCommandKind::Leave, 2);
  if (last == nullptr || !last->battle.has_value() || !last->loot.has_value() ||
      exitStatusOf(*last->battle, 1) != ParticipantExitStatus::VoluntaryLeft ||
      exitStatusOf(*last->battle, 2) != ParticipantExitStatus::VoluntaryLeft) {
    return false;
  }
  const auto &loot = *last->loot;
  if (loot.resolution != LootResolutionState::Resolved ||
      loot.holdings.size() != 1 ||
      holdingQuantity(loot, 1, 1) != std::optional<std::uint64_t>{1} ||
      findDrop(loot, 1)->state != LootDropState::Unclaimed ||
      findDrop(loot, 1)->owner.has_value() ||
      findDrop(loot, 2)->state != LootDropState::Claimed ||
      findDrop(loot, 2)->owner != SessionId{1}) {
    return false;
  }
  scenario.advance(15s);
  if (!scenario.wait()) {
    return false;
  }
  const auto later = scenario.take();
  return later.empty();
}

// Case 6: exits after an already committed result plus duplicate Leave and
// Disconnect notifications. The committed result/outcome stays equal, the
// committed exit statuses are sticky, and no Drop/holding/result mutation
// happens.
bool exitsAfterCommittedResultAreNoMutation() {
  CellScenario scenario{2};
  if (!scenario.start(2)) {
    return false;
  }
  std::uint64_t commandId = 1;
  if (!killMonsterThroughCell(scenario, commandId)) {
    return false;
  }
  scenario.advance(15s);
  if (!scenario.wait()) {
    return false;
  }
  const auto deadlineOutcomes = scenario.take();
  const auto *deadline =
      findOutcome(deadlineOutcomes, RoomCommandKind::LootDeadline);
  if (deadline == nullptr || !deadline->lootDeadlineCode.has_value() ||
      *deadline->lootDeadlineCode != LootDeadlineResultCode::Ok ||
      !deadline->loot.has_value() ||
      deadline->loot->resolution != LootResolutionState::Resolved) {
    return false;
  }
  const auto committedLoot = *deadline->loot;
  if (!scenario.submitLeave(1) || !scenario.submitLeave(1) ||
      !scenario.submitDisconnect(2) || !scenario.submitDisconnect(2) ||
      !scenario.wait()) {
    return false;
  }
  const auto exits = scenario.take();
  std::vector<const RoomCommandOutcome *> leaves;
  std::vector<const RoomCommandOutcome *> disconnects;
  for (const auto &outcome : exits) {
    if (outcome.kind == RoomCommandKind::Leave) {
      leaves.push_back(&outcome);
    } else if (outcome.kind == RoomCommandKind::ConfirmedDisconnect) {
      disconnects.push_back(&outcome);
    }
  }
  if (leaves.size() != 2 || disconnects.size() != 2 ||
      leaves[0]->admissionOrdinal >= leaves[1]->admissionOrdinal ||
      leaves[1]->admissionOrdinal >= disconnects[0]->admissionOrdinal ||
      disconnects[0]->admissionOrdinal >= disconnects[1]->admissionOrdinal) {
    return false;
  }
  // The first and the duplicate Leave are both no mutation: equal battle and
  // loot projections with the committed terminal statuses and committed loot.
  for (const auto *leave : leaves) {
    if (!leave->battle.has_value() || !leave->loot.has_value() ||
        exitStatusOf(*leave->battle, 1) !=
            ParticipantExitStatus::TerminalPresent ||
        exitStatusOf(*leave->battle, 2) !=
            ParticipantExitStatus::TerminalPresent ||
        !(*leave->loot == committedLoot)) {
      return false;
    }
  }
  // The first and the duplicate ConfirmedDisconnect are both no mutation:
  // equal battle and loot projections with the committed terminal statuses and
  // committed loot.
  for (const auto *disconnect : disconnects) {
    if (!disconnect->battle.has_value() || !disconnect->loot.has_value() ||
        exitStatusOf(*disconnect->battle, 1) !=
            ParticipantExitStatus::TerminalPresent ||
        exitStatusOf(*disconnect->battle, 2) !=
            ParticipantExitStatus::TerminalPresent ||
        !(*disconnect->loot == committedLoot)) {
      return false;
    }
  }
  if (disconnects[0]->loot != disconnects[1]->loot) {
    return false;
  }
  // The committed MonsterDefeated outcome stays equal after the exits.
  if (!scenario.submitAttack(commandId, 1, kStart + 20s) || !scenario.wait()) {
    return false;
  }
  const auto late = scenario.take();
  return late.size() == 1 && late[0].attackResult.has_value() &&
         late[0].attackResult->code ==
             AttackResultCode::TerminalAlreadyDecided &&
         late[0].attackResult->outcome == CombatOutcome::MonsterDefeated &&
         late[0].combat.has_value() && late[0].combat->terminal.has_value() &&
         late[0].combat->terminal->outcome == CombatOutcome::MonsterDefeated;
}

} // namespace

int main() {
  return nonlethalReplayAndExitedParticipantAreStable() &&
                 validationAndCapacityPrecedeDamage() &&
                 twoAndTenParticipantsCompleteDeterministically() &&
                 lethalAndDeadlineKeepOneStableTerminal() &&
                 cellSerializesLethalAndDeadlineOrders() &&
                 cellClaimRetainsOverloadAcrossRateRefill() &&
                 cellClaimVsLootDeadlineOrders() && cellClaimVsExitOrders() &&
                 lastLeaveBeforeLethalAttackCancelsWithoutTerminal() &&
                 lethalAttackThenLastLeaveClosesOpenLoot() &&
                 lastDisconnectBeforeCombatDeadlineFreezesTerminal() &&
                 committedTimeoutSurvivesLaterDisconnects() &&
                 partialClaimThenAllExitClosesRemainingDrops() &&
                 exitsAfterCommittedResultAreNoMutation()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
