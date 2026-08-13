#include <lol/battle/BattleLoadApi.hpp>
#include <lol/battle/MovementApi.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using lol::battle::ArenaLoadCompleteCommand;
using lol::battle::BattleAdmissionSnapshot;
using lol::battle::BattleInstance;
using lol::battle::BattleLoadResultCode;
using lol::battle::BattleStartCandidate;
using lol::battle::CandidateDisconnectedCommand;
using lol::battle::DirectionIntent;
using lol::battle::MoveCommand;
using lol::battle::MovementResultCode;
using lol::battle::MovementTickCommand;
using lol::battle::ParticipantExitStatus;
using lol::battle::PlayerPositionProjection;
using lol::shared::AccountId;
using lol::shared::BattleInstanceId;
using lol::shared::RoomId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;

constexpr auto kStart = std::chrono::steady_clock::time_point{};

AccountId account(std::uint8_t suffix) {
  AccountId::Bytes bytes{};
  bytes.back() = suffix;
  return AccountId{bytes};
}

BattleInstance committedBattle() {
  std::vector<BattleStartCandidate> candidates;
  for (std::uint64_t session = 1; session <= 2; ++session) {
    candidates.push_back(BattleStartCandidate{
        .accountId = account(static_cast<std::uint8_t>(session)),
        .sessionId = SessionId{session},
        .generation = SessionGeneration{1},
        .nickname = "player-" + std::to_string(session),
    });
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
  for (std::uint64_t session = 1; session <= 2; ++session) {
    if (created.battle->completeLoad(
            ArenaLoadCompleteCommand{.sessionId = SessionId{session},
                                     .generation = SessionGeneration{1},
                                     .roomId = RoomId{7},
                                     .battleId = BattleInstanceId{1}},
            true) != BattleLoadResultCode::Ok) {
      std::abort();
    }
  }
  return std::move(*created.battle);
}

MoveCommand move(std::uint64_t sessionId, std::uint32_t actionSequence,
                 std::int16_t desiredX, std::int16_t desiredY,
                 std::uint64_t generation = 1, std::uint64_t battleId = 1,
                 std::uint16_t inputFlags = 0) {
  return MoveCommand{.sessionId = SessionId{sessionId},
                     .generation = SessionGeneration{generation},
                     .battleId = BattleInstanceId{battleId},
                     .actionSequence = actionSequence,
                     .direction = DirectionIntent{.desiredX = desiredX,
                                                  .desiredY = desiredY,
                                                  .inputFlags = inputFlags}};
}

MovementTickCommand tick(std::uint32_t serverTick, std::uint64_t battleId = 1) {
  return MovementTickCommand{.battleId = BattleInstanceId{battleId},
                             .serverTick = serverTick};
}

std::optional<PlayerPositionProjection> position(const BattleInstance &battle,
                                                 std::uint64_t sessionId) {
  for (const auto &player : battle.movementProjection().players) {
    if (player.sessionId == SessionId{sessionId}) {
      return player;
    }
  }
  return std::nullopt;
}

bool normalizesStopsAndClampsAtFixedTicks() {
  auto battle = committedBattle();
  if (battle.acceptMove(move(1, 1, 3, 4), kStart) != MovementResultCode::Ok ||
      battle.integrateMovement(tick(1)) != MovementResultCode::Ok ||
      position(battle, 1) != PlayerPositionProjection{.sessionId = SessionId{1},
                                                      .posXMillimeter = 150,
                                                      .posYMillimeter = 200} ||
      battle.acceptMove(move(1, 2, 0, 0), kStart) != MovementResultCode::Ok ||
      battle.integrateMovement(tick(2)) != MovementResultCode::Ok ||
      position(battle, 1)->posXMillimeter != 150 ||
      position(battle, 1)->posYMillimeter != 200 ||
      battle.acceptMove(move(1, 3, 32767, 0), kStart) !=
          MovementResultCode::Ok) {
    return false;
  }
  for (std::uint32_t serverTick = 3; serverTick <= 42; ++serverTick) {
    if (battle.integrateMovement(tick(serverTick)) != MovementResultCode::Ok) {
      return false;
    }
  }
  const auto atBound = position(battle, 1);
  const auto beforeInvalid = position(battle, 2);
  if (!atBound.has_value() || atBound->posXMillimeter != 10000 ||
      atBound->posYMillimeter != 200 || !beforeInvalid.has_value() ||
      battle.acceptMove(move(2, 1, std::numeric_limits<std::int16_t>::min(), 0),
                        kStart) != MovementResultCode::InvalidArgument ||
      battle.acceptMove(move(2, 1, 1, 0, 1, 1, 1), kStart) !=
          MovementResultCode::InvalidArgument ||
      battle.integrateMovement(tick(43)) != MovementResultCode::Ok ||
      position(battle, 2) != beforeInvalid ||
      battle.acceptMove(move(2, 1, -32767, -32767), kStart) !=
          MovementResultCode::Ok) {
    return false;
  }
  for (std::uint32_t serverTick = 44; serverTick <= 101; ++serverTick) {
    if (battle.integrateMovement(tick(serverTick)) != MovementResultCode::Ok) {
      return false;
    }
  }
  const auto atMinimum = position(battle, 2);
  return atMinimum.has_value() && atMinimum->posXMillimeter == -10000 &&
         atMinimum->posYMillimeter == -10000 &&
         battle.integrateMovement(tick(101)) == MovementResultCode::StaleTick;
}

bool coalescesNewestIntentAcrossSequenceWrap() {
  auto battle = committedBattle();
  constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
  if (battle.acceptMove(move(1, maximum, 32767, 0), kStart) !=
          MovementResultCode::Ok ||
      battle.acceptMove(move(1, 0, 0, 32767), kStart) !=
          MovementResultCode::Ok ||
      battle.acceptMove(move(1, maximum, -32767, 0), kStart) !=
          MovementResultCode::StaleAction ||
      battle.integrateMovement(tick(1)) != MovementResultCode::Ok) {
    return false;
  }
  return position(battle, 1) ==
         PlayerPositionProjection{.sessionId = SessionId{1},
                                  .posXMillimeter = 0,
                                  .posYMillimeter = 250};
}

bool enforcesThirtyPerSecondWithBurstSix() {
  auto battle = committedBattle();
  for (std::uint32_t sequence = 1; sequence <= 6; ++sequence) {
    if (battle.acceptMove(move(1, sequence, 32767, 0), kStart) !=
        MovementResultCode::Ok) {
      return false;
    }
  }
  if (battle.acceptMove(move(1, 7, 0, 32767), kStart) !=
          MovementResultCode::RateLimited ||
      battle.integrateMovement(tick(1)) != MovementResultCode::Ok ||
      battle.acceptMove(move(1, 8, 0, 32767), kStart + 33'333'334ns) !=
          MovementResultCode::Ok ||
      battle.integrateMovement(tick(2)) != MovementResultCode::Ok) {
    return false;
  }
  return position(battle, 1) ==
         PlayerPositionProjection{.sessionId = SessionId{1},
                                  .posXMillimeter = 250,
                                  .posYMillimeter = 250};
}

bool rateLimitedMoveDoesNotAdvanceFreshness() {
  auto battle = committedBattle();
  for (std::uint32_t sequence = 1; sequence <= 6; ++sequence) {
    if (battle.acceptMove(move(1, sequence, 32767, 0), kStart) !=
        MovementResultCode::Ok) {
      return false;
    }
  }
  if (battle.acceptMove(move(1, 100, 0, 32767), kStart) !=
          MovementResultCode::RateLimited ||
      battle.acceptMove(move(1, 7, 0, 32767), kStart + 33'333'334ns) !=
          MovementResultCode::Ok ||
      battle.integrateMovement(tick(1)) != MovementResultCode::Ok) {
    return false;
  }
  return position(battle, 1) ==
         PlayerPositionProjection{.sessionId = SessionId{1},
                                  .posXMillimeter = 0,
                                  .posYMillimeter = 250};
}

bool rejectsStaleAndPostExitMovementInMailboxOrder() {
  auto stopped = committedBattle();
  // Pre-disconnect captured projection: membership/identity unchanged by exit,
  // only Session 1's exitStatus transitions to Disconnected.
  auto expectedParticipants = stopped.projection().capturedParticipants;
  for (auto &participant : expectedParticipants) {
    if (participant.sessionId == SessionId{1}) {
      participant.exitStatus = ParticipantExitStatus::Disconnected;
    }
  }
  const CandidateDisconnectedCommand disconnect{
      .sessionId = SessionId{1},
      .generation = SessionGeneration{1},
      .roomId = RoomId{7},
      .battleId = BattleInstanceId{1},
  };
  if (stopped.acceptMove(move(1, 1, 32767, 0), kStart) !=
          MovementResultCode::Ok ||
      stopped.acceptMove(move(1, 2, 1, 0, 2), kStart) !=
          MovementResultCode::StaleSession ||
      stopped.acceptMove(move(1, 2, 1, 0, 1, 2), kStart) !=
          MovementResultCode::StaleBattle ||
      stopped.acceptMove(move(9, 1, 1, 0), kStart) !=
          MovementResultCode::NotEligible ||
      stopped.disconnect(disconnect) != BattleLoadResultCode::Ok ||
      stopped.acceptMove(move(1, 2, 32767, 0), kStart) !=
          MovementResultCode::NotEligible ||
      stopped.integrateMovement(tick(1)) != MovementResultCode::Ok ||
      position(stopped, 1)->posXMillimeter != 0 ||
      stopped.projection().capturedParticipants != expectedParticipants) {
    return false;
  }

  auto moved = committedBattle();
  return moved.acceptMove(move(1, 1, 32767, 0), kStart) ==
             MovementResultCode::Ok &&
         moved.integrateMovement(tick(1)) == MovementResultCode::Ok &&
         moved.disconnect(disconnect) == BattleLoadResultCode::Ok &&
         position(moved, 1)->posXMillimeter == 250;
}

} // namespace

int main() {
  return normalizesStopsAndClampsAtFixedTicks() &&
                 coalescesNewestIntentAcrossSequenceWrap() &&
                 enforcesThirtyPerSecondWithBurstSix() &&
                 rateLimitedMoveDoesNotAdvanceFreshness() &&
                 rejectsStaleAndPostExitMovementInMailboxOrder()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
