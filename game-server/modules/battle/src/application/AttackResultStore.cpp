#include <lol/battle/CombatResultStore.hpp>

#include <algorithm>
#include <limits>
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
    std::uint64_t completedAtNanos) noexcept {
  if (!battleCompletedAtNanos_.has_value()) {
    battleCompletedAtNanos_ = completedAtNanos;
  }
}

std::size_t AttackResultStore::evictExpired(std::uint64_t nowNanos) {
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

void AttackResultStore::markBattleCompleted(
    Clock::time_point completedAt) noexcept {
  markBattleCompleted(compatibilityNanos(completedAt));
}

std::size_t AttackResultStore::evictExpired(Clock::time_point now) {
  return evictExpired(compatibilityNanos(now));
}

std::uint64_t
AttackResultStore::compatibilityNanos(Clock::time_point point) const noexcept {
  if (point <= Clock::time_point{}) {
    return 0;
  }
  const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
      point - Clock::time_point{});
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

AttackResultStoreState AttackResultStore::exportState() const {
  AttackResultStoreState state{
      .battleCompletedAtNanos = battleCompletedAtNanos_, .records = {}};
  state.records.reserve(records_.size());
  for (const auto &record : records_) {
    state.records.push_back(AttackResultRecordState{
        .commandId = record.commandId,
        .targetHint = record.targetHint,
        .result = record.result,
    });
  }
  return state;
}

bool AttackResultStore::importState(const AttackResultStoreState &state) {
  if (state.records.size() > maximumResults) {
    return false;
  }
  std::deque<Record> records;
  records.clear();
  for (const auto &entry : state.records) {
    if (entry.result.commandId != entry.commandId ||
        entry.result.battleId != battleId_) {
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
        .targetHint = entry.targetHint,
        .result = entry.result,
    });
  }
  records_ = std::move(records);
  battleCompletedAtNanos_ = state.battleCompletedAtNanos;
  return true;
}

std::size_t AttackResultStore::size() const noexcept { return records_.size(); }

} // namespace lol::battle
