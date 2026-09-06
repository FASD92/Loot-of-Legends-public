#pragma once

#include <lol/battle/BattleLoadApi.hpp>
#include <lol/lobby_room/RoomApi.hpp>
#include <lol/settlement/SettlementIntent.hpp>

#include <optional>

namespace lol::game_flow::workflows {

// Pure production settlement projection for a committed Battle. Recovery uses
// the same immutable builder with the committed wall/logical timestamps from
// the terminal receipt; no Room mutation or storage side effect occurs here.
[[nodiscard]] std::optional<settlement::SettlementIntentBatch>
createSettlementIntentBatchForTerminal(
    const battle::BattleInstance &battle,
    settlement::ResultCommittedAt committedAt);

// Joins only copied Battle public projections into the immutable settlement
// value, then moves the Room to the pre-durability hold. No storage, outbound
// visibility, or reopen action exists in this Slice 6 workflow.
[[nodiscard]] std::optional<settlement::SettlementIntentBatch>
holdTerminalForSettlementDurability(lobby_room::Room &room,
                                    const battle::BattleInstance &battle,
                                    settlement::ResultCommittedAt committedAt);

} // namespace lol::game_flow::workflows
