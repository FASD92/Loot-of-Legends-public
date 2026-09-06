#pragma once

#include <lol/shared/Identifiers.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace lol::battle {

using BattleRulesetVersion = std::uint32_t;
using BattleSeed = std::uint64_t;
inline constexpr BattleRulesetVersion battleRulesetVersion = 1;

[[nodiscard]] BattleSeed
deriveBattleSeed(shared::RoomId roomId, shared::BattleInstanceId battleId,
                 BattleRulesetVersion rulesetVersion) noexcept;

struct BattleStartCandidate final {
  shared::AccountId accountId;
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
  std::string nickname;

  bool operator==(const BattleStartCandidate &) const = default;
};

struct BattleAdmissionSnapshot final {
  shared::RoomId roomId;
  shared::BattleInstanceId battleId;
  std::vector<BattleStartCandidate> candidates;
  // Battle continuity v1 is explicit at creation. Defaults retain source
  // compatibility for existing callers while still rejecting zero/unknown
  // values in BattleInstance::create.
  BattleRulesetVersion rulesetVersion{battleRulesetVersion};
  BattleSeed seed{1};

  bool operator==(const BattleAdmissionSnapshot &) const = default;
};

} // namespace lol::battle
