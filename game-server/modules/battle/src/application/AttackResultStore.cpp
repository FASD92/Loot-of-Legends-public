#include <lol/battle/CombatResultStore.hpp>

#include <algorithm>
#include <utility>

namespace lol::battle {

AttackResultStore::AttackResultStore(shared::SessionId sessionId,
                                     shared::SessionGeneration generation,
                                     shared::BattleInstanceId battleId)
    : sessionId_(sessionId), generation_(generation), battleId_(battleId) {}

bool AttackResultStore::scopeMatches(
    const AttackCommand &command) const noexcept {
  return command.sessionId == sessionId_ && command.generation == generation_ &&
         command.battleId == battleId_;
}

AttackResultStoreInspection
AttackResultStore::inspect(const AttackCommand &command) const {
  if (!scopeMatches(command)) {
    return {AttackResultStoreDecision::ScopeMismatch, std::nullopt};
  }
  const auto found =
      std::ranges::find_if(records_, [&command](const Record &record) {
        return record.commandId == command.commandId;
      });
  if (found != records_.end()) {
    if (found->targetHint != command.targetHint) {
      return {AttackResultStoreDecision::Conflict, std::nullopt};
    }
    return {AttackResultStoreDecision::Replay, found->result};
  }
  if (records_.size() >= maximumResults) {
    return {AttackResultStoreDecision::Overloaded, std::nullopt};
  }
  return {AttackResultStoreDecision::Available, std::nullopt};
}

bool AttackResultStore::retain(const AttackCommand &command,
                               AttackTerminalResult result) {
  if (inspect(command).decision != AttackResultStoreDecision::Available ||
      result.commandId != command.commandId ||
      result.battleId != command.battleId) {
    return false;
  }
  records_.push_back(Record{
      .commandId = command.commandId,
      .targetHint = command.targetHint,
      .result = std::move(result),
  });
  return true;
}

void AttackResultStore::markBattleCompleted(
    Clock::time_point completedAt) noexcept {
  if (!battleCompletedAt_.has_value()) {
    battleCompletedAt_ = completedAt;
  }
}

std::size_t AttackResultStore::evictExpired(Clock::time_point now) {
  if (!battleCompletedAt_.has_value() ||
      now < *battleCompletedAt_ + retentionAfterBattleCompleted) {
    return 0;
  }
  const auto removed = records_.size();
  while (!records_.empty()) {
    records_.pop_front();
  }
  return removed;
}

std::size_t AttackResultStore::size() const noexcept { return records_.size(); }

} // namespace lol::battle
