#include <lol/battle/LootResultStore.hpp>

#include <algorithm>
#include <limits>
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

std::uint64_t clockNanos(LootResultStore::Clock::time_point point) noexcept {
  if (point <= LootResultStore::Clock::time_point{}) {
    return 0;
  }
  const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
      point - LootResultStore::Clock::time_point{});
  const auto count = duration.count();
  if (count <= 0) {
    return 0;
  }
  using Rep = decltype(count);
  if constexpr (std::numeric_limits<Rep>::digits >=
                std::numeric_limits<std::uint64_t>::digits) {
    if (static_cast<std::uintmax_t>(count) >
        std::numeric_limits<std::uint64_t>::max()) {
      return std::numeric_limits<std::uint64_t>::max();
    }
  }
  return static_cast<std::uint64_t>(count);
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
    std::uint64_t completedAtNanos) noexcept {
  if (!battleCompletedAtNanos_.has_value()) {
    battleCompletedAtNanos_ = completedAtNanos;
  }
}

std::size_t LootResultStore::evictExpired(std::uint64_t nowNanos) {
  constexpr auto retentionNanos = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          retentionAfterBattleCompleted)
          .count());
  if (!battleCompletedAtNanos_.has_value() ||
      nowNanos < *battleCompletedAtNanos_ ||
      nowNanos - *battleCompletedAtNanos_ < retentionNanos) {
    return 0;
  }
  const auto removed = records_.size();
  while (!records_.empty()) {
    records_.pop_front();
  }
  return removed;
}

void LootResultStore::markBattleCompleted(
    Clock::time_point completedAt) noexcept {
  markBattleCompleted(compatibilityNanos(completedAt));
}

std::size_t LootResultStore::evictExpired(Clock::time_point now) {
  return evictExpired(compatibilityNanos(now));
}

std::uint64_t
LootResultStore::compatibilityNanos(Clock::time_point point) const noexcept {
  return clockNanos(point);
}

LootResultStoreState LootResultStore::exportState() const {
  LootResultStoreState state{.battleCompletedAtNanos = battleCompletedAtNanos_,
                             .records = {}};
  state.records.reserve(records_.size());
  for (const auto &record : records_) {
    state.records.push_back(LootResultRecordState{
        .commandId = record.commandId,
        .dropId = record.dropId,
        .result = record.result,
    });
  }
  return state;
}

bool LootResultStore::importState(const LootResultStoreState &state) {
  if (state.records.size() > maximumResults) {
    return false;
  }
  std::deque<Record> records;
  for (const auto &entry : state.records) {
    if (entry.result.commandId != entry.commandId ||
        entry.result.battleId != battleId_ ||
        entry.result.dropId != entry.dropId) {
      return false;
    }
    const auto duplicate =
        std::ranges::find_if(records, [&entry](const Record &record) {
          return record.commandId == entry.commandId;
        });
    if (duplicate != records.end()) {
      return false;
    }
    records.push_back(Record{
        .commandId = entry.commandId,
        .dropId = entry.dropId,
        .result = entry.result,
    });
  }
  records_ = std::move(records);
  battleCompletedAtNanos_ = state.battleCompletedAtNanos;
  return true;
}

std::size_t LootResultStore::size() const noexcept { return records_.size(); }

RetainedLootResults::RetainedLootResults(
    shared::BattleInstanceId battleId, std::uint64_t completedAtNanos,
    std::optional<std::uint64_t> compatibilityCompletedAtNanos,
    std::vector<LootResultStore> stores)
    : battleId_(battleId), completedAtNanos_(completedAtNanos),
      compatibilityCompletedAtNanos_(compatibilityCompletedAtNanos),
      stores_(std::move(stores)) {}

shared::BattleInstanceId RetainedLootResults::battleId() const noexcept {
  return battleId_;
}

bool RetainedLootResults::expired(std::uint64_t nowNanos) const noexcept {
  constexpr auto retentionNanos = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          LootResultStore::retentionAfterBattleCompleted)
          .count());
  return nowNanos >= completedAtNanos_ &&
         nowNanos - completedAtNanos_ >= retentionNanos;
}

bool RetainedLootResults::expired(Clock::time_point now) const noexcept {
  if (!compatibilityCompletedAtNanos_.has_value()) {
    compatibilityCompletedAtNanos_ = clockNanos(now);
  }
  const auto nowNanos = clockNanos(now);
  if (nowNanos <= *compatibilityCompletedAtNanos_) {
    return false;
  }
  const auto elapsed = nowNanos - *compatibilityCompletedAtNanos_;
  const auto relative =
      completedAtNanos_ > std::numeric_limits<std::uint64_t>::max() - elapsed
          ? std::numeric_limits<std::uint64_t>::max()
          : completedAtNanos_ + elapsed;
  return expired(relative);
}

ClaimLootTerminalResult
RetainedLootResults::route(const ClaimLootCommand &command,
                           std::uint64_t nowNanos) const {
  if (command.battleId != battleId_ || expired(nowNanos)) {
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

ClaimLootTerminalResult
RetainedLootResults::route(const ClaimLootCommand &command,
                           Clock::time_point now) const {
  if (!compatibilityCompletedAtNanos_.has_value()) {
    compatibilityCompletedAtNanos_ = clockNanos(now);
  }
  const auto nowNanos = clockNanos(now);
  if (nowNanos <= *compatibilityCompletedAtNanos_) {
    return route(command, completedAtNanos_);
  }
  const auto elapsed = nowNanos - *compatibilityCompletedAtNanos_;
  const auto relative =
      completedAtNanos_ > std::numeric_limits<std::uint64_t>::max() - elapsed
          ? std::numeric_limits<std::uint64_t>::max()
          : completedAtNanos_ + elapsed;
  return route(command, relative);
}

} // namespace lol::battle
