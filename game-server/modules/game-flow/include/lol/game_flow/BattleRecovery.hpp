#pragma once

#include <lol/shared/Identifiers.hpp>

#include <cstdint>

namespace lol::game_flow {

enum class BattleRecoveryReason : std::uint8_t {
  ResultGenerationFailed = 1,
  SettlementRecoveryPending = 2,
};

struct BattleRecoveryNotice final {
  shared::RoomId roomId;
  shared::BattleInstanceId battleId;
  BattleRecoveryReason reason;

  bool operator==(const BattleRecoveryNotice &) const = default;
};

} // namespace lol::game_flow
