#pragma once

#include <lol/battle/BattleLoadApi.hpp>
#include <lol/lobby_room/RoomApi.hpp>
#include <lol/settlement/SettlementIntent.hpp>

#include <optional>

namespace lol::game_flow::workflows {

// Joins only copied Battle public projections into the immutable settlement
// value, then moves the Room to the pre-durability hold. No storage, outbound
// visibility, or reopen action exists in this terminal workflow.
[[nodiscard]] std::optional<settlement::SettlementIntentBatch>
holdTerminalForSettlementDurability(lobby_room::Room &room,
                                    const battle::BattleInstance &battle,
                                    settlement::ResultCommittedAt committedAt);

} // namespace lol::game_flow::workflows
