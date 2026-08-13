#pragma once

#include <lol/battle/BattleLoadApi.hpp>
#include <lol/lobby_room/RoomApi.hpp>
#include <lol/settlement/SettlementCapacityGate.hpp>

#include <functional>
#include <optional>

namespace lol::game_flow::workflows {

struct HostStartWorkflowResult final {
  lobby_room::RoomResultCode code;
  std::optional<lobby_room::BattleAdmissionSnapshot> admission;
  std::optional<battle::BattleInstance> battle;
  std::optional<settlement::SettlementCapacityReservation> reservation;
};

using ScheduleLoadDeadline =
    std::function<bool(battle::LoadBarrierDeadlineCommand)>;

[[nodiscard]] HostStartWorkflowResult
commitHostStart(lobby_room::Room &room, shared::BattleInstanceId battleId,
                const lobby_room::HostStartEligibilityCommand &command,
                settlement::SettlementCapacityGate *capacityGate,
                const ScheduleLoadDeadline &scheduleDeadline);

} // namespace lol::game_flow::workflows
