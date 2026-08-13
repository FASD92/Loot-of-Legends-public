#include "workflows/HostStartWorkflow.hpp"

#include <utility>
#include <vector>

namespace lol::game_flow::workflows {

HostStartWorkflowResult
commitHostStart(lobby_room::Room &room, shared::BattleInstanceId battleId,
                const lobby_room::HostStartEligibilityCommand &command,
                settlement::SettlementCapacityGate *capacityGate,
                const ScheduleLoadDeadline &scheduleDeadline) {
  auto eligibility = room.prepareHostStart(command);
  if (eligibility.code != lobby_room::RoomResultCode::Ok ||
      !eligibility.admission.has_value()) {
    return {eligibility.code, std::nullopt, std::nullopt, std::nullopt};
  }

  std::optional<settlement::SettlementCapacityReservation> reservation;
  if (capacityGate != nullptr) {
    auto capacity = capacityGate->tryReserve();
    if (capacity.code != settlement::CapacityReservationCode::Reserved ||
        !capacity.reservation.has_value()) {
      return {lobby_room::RoomResultCode::RoomOverloaded, std::nullopt,
              std::nullopt, std::nullopt};
    }
    reservation = std::move(capacity.reservation);
  }

  std::vector<battle::BattleStartCandidate> candidates;
  candidates.reserve(eligibility.admission->members.size());
  for (const auto &member : eligibility.admission->members) {
    candidates.push_back(battle::BattleStartCandidate{
        .accountId = member.accountId,
        .sessionId = member.sessionId,
        .generation = member.sessionGeneration,
        .nickname = member.nickname,
    });
  }
  auto created = battle::BattleInstance::create(battle::BattleAdmissionSnapshot{
      .roomId = eligibility.admission->roomId,
      .battleId = battleId,
      .candidates = std::move(candidates),
  });
  if (created.code != battle::BattleLoadResultCode::Ok ||
      !created.battle.has_value() ||
      created.battle->openLoadBarrier() != battle::BattleLoadResultCode::Ok) {
    return {lobby_room::RoomResultCode::InvalidArgument, std::nullopt,
            std::nullopt, std::nullopt};
  }

  if (!scheduleDeadline(battle::LoadBarrierDeadlineCommand{
          .roomId = eligibility.admission->roomId,
          .battleId = battleId,
      })) {
    return {lobby_room::RoomResultCode::RoomOverloaded, std::nullopt,
            std::nullopt, std::nullopt};
  }

  const auto commit = room.commitLoading(*eligibility.admission);
  if (commit != lobby_room::RoomResultCode::Ok) {
    return {commit, std::nullopt, std::nullopt, std::nullopt};
  }
  return {lobby_room::RoomResultCode::Ok, std::move(eligibility.admission),
          std::move(created.battle), std::move(reservation)};
}

} // namespace lol::game_flow::workflows
