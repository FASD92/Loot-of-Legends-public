#pragma once

#include <lol/shared/Identifiers.hpp>

#include <string>
#include <vector>

namespace lol::battle {

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

  bool operator==(const BattleAdmissionSnapshot &) const = default;
};

} // namespace lol::battle
