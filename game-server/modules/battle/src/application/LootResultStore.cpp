#include <lol/battle/LootResultStore.hpp>

#include <algorithm>
#include <utility>

namespace lol::battle {
namespace {

ClaimLootTerminalResult reject(const ClaimLootCommand &command,
                               ClaimLootResultCode code) noexcept {
  return ClaimLootTerminalResult{
      .commandId = command.commandId,
      .battleId = command.battleId,
      .dropId = command.dropId,
      .code = code,
  };
}

} // namespace

LootResultStore::LootResultStore(shared::SessionId sessionId,
                                 shared::SessionGeneration generation,
                                 shared::BattleInstanceId battleId)
    : sessionId_(sessionId), generation_(generation), battleId_(battleId) {}

bool LootResultStore::scopeMatches(
    const ClaimLootCommand &command) const noexcept {
  return command.sessionId == sessionId_ && command.generation == generation_ &&
         command.battleId == battleId_;
}

LootResultStoreInspection
LootResultStore::inspect(const ClaimLootCommand &command) const {
  if (!scopeMatches(command)) {
    return {LootResultStoreDecision::ScopeMismatch, std::nullopt};
  }
  const auto found =
      std::ranges::find_if(records_, [&command](const Record &record) {
        return record.commandId == command.commandId;
      });
  if (found != records_.end()) {
    if (found->dropId != command.dropId) {
      return {LootResultStoreDecision::Conflict, std::nullopt};
    }
    return {LootResultStoreDecision::Replay, found->result};
  }
  if (records_.size() >= maximumResults) {
    return {LootResultStoreDecision::Overloaded, std::nullopt};
  }
  return {LootResultStoreDecision::Available, std::nullopt};
}

bool LootResultStore::retain(const ClaimLootCommand &command,
                             ClaimLootTerminalResult result) {
  if (inspect(command).decision != LootResultStoreDecision::Available ||
      result.commandId != command.commandId ||
      result.battleId != command.battleId || result.dropId != command.dropId) {
    return false;
  }
  records_.push_back(Record{
      .commandId = command.commandId,
      .dropId = command.dropId,
      .result = std::move(result),
  });
  return true;
}

void LootResultStore::markBattleCompleted(
    Clock::time_point completedAt) noexcept {
  if (!battleCompletedAt_.has_value()) {
    battleCompletedAt_ = completedAt;
  }
}

std::size_t LootResultStore::evictExpired(Clock::time_point now) {
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

std::size_t LootResultStore::size() const noexcept { return records_.size(); }

RetainedLootResults::RetainedLootResults(shared::BattleInstanceId battleId,
                                         Clock::time_point completedAt,
                                         std::vector<LootResultStore> stores)
    : battleId_(battleId), completedAt_(completedAt),
      stores_(std::move(stores)) {}

shared::BattleInstanceId RetainedLootResults::battleId() const noexcept {
  return battleId_;
}

bool RetainedLootResults::expired(Clock::time_point now) const noexcept {
  return now >= completedAt_ + LootResultStore::retentionAfterBattleCompleted;
}

ClaimLootTerminalResult
RetainedLootResults::route(const ClaimLootCommand &command,
                           Clock::time_point now) const {
  if (command.battleId != battleId_ || expired(now)) {
    return reject(command, ClaimLootResultCode::StaleBattle);
  }
  for (const auto &store : stores_) {
    const auto inspection = store.inspect(command);
    if (inspection.decision == LootResultStoreDecision::ScopeMismatch) {
      continue;
    }
    if (inspection.decision == LootResultStoreDecision::Replay) {
      return *inspection.result;
    }
    if (inspection.decision == LootResultStoreDecision::Conflict) {
      return reject(command, ClaimLootResultCode::CommandConflict);
    }
    return reject(command, ClaimLootResultCode::StaleBattle);
  }
  return reject(command, ClaimLootResultCode::StaleBattle);
}

} // namespace lol::battle
