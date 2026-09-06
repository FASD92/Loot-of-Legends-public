#pragma once

#include <lol/battle_continuity/FlightRecorder.hpp>
#include <lol/battle_continuity/RecoveryEnvelopeCodec.hpp>
#include <lol/lobby_room/RoomApi.hpp>
#include <lol/settlement/SettlementIntent.hpp>

#include <cstdint>
#include <optional>

namespace lol::game_flow::workflows {

[[nodiscard]] std::optional<std::uint16_t> participantSlotForSession(
    const battle::BattleDeterministicState &state, shared::SessionId sessionId);

[[nodiscard]] std::optional<battle_continuity::RoomRecoveryState>
captureRoomRecoveryState(const lobby_room::Room &room,
                         const battle::BattleInstance &battle,
                         std::uint64_t nextBattleOrdinal);

[[nodiscard]] std::optional<battle_continuity::Bytes> captureRecoveryEnvelope(
    const lobby_room::Room &room, const battle::BattleInstance &battle,
    battle_continuity::BattleIdentity identity);

[[nodiscard]] std::optional<battle_continuity::TerminalRecording>
captureTerminalRecording(
    const battle::BattleInstance &battle,
    const settlement::SettlementIntentBatch &settlementBatch);

[[nodiscard]] std::optional<battle_continuity::Bytes>
encodeJournal(const battle_continuity::BattleRecording &recording);

} // namespace lol::game_flow::workflows
