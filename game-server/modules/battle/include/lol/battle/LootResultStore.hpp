#pragma once

#include <lol/battle/BattleTime.hpp>
#include <lol/battle/LootApi.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

namespace lol::battle {

enum class LootResultStoreDecision : std::uint8_t {
  Available,
  Replay,
  Conflict,
  Overloaded,
  ScopeMismatch,
};

struct LootResultStoreInspection final {
  LootResultStoreDecision decision;
  std::optional<ClaimLootTerminalResult> result;
};

struct LootResultRecordState final {
  CommandId commandId;
  DropId dropId;
  ClaimLootTerminalResult result;

  bool operator==(const LootResultRecordState &) const = default;
};

struct LootResultStoreState final {
  std::optional<std::uint64_t> battleCompletedAtNanos;
  std::vector<LootResultRecordState> records;

  bool operator==(const LootResultStoreState &) const = default;
};

// Battle-owned bounded terminal result retention for ClaimLoot, patterned
// narrowly after AttackResultStore; no generic result-store framework.
// Scope is the exact tuple (SessionId, SessionGeneration, BattleInstanceId);
// a different scope is a distinct store and may reuse the same CommandId.
class LootResultStore final {
public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::size_t maximumResults = 256;
  static constexpr auto retentionAfterBattleCompleted =
      std::chrono::milliseconds{30000};

  LootResultStore(shared::SessionId sessionId,
                  shared::SessionGeneration generation,
                  shared::BattleInstanceId battleId);

  [[nodiscard]] LootResultStoreInspection
  inspect(const ClaimLootCommand &command) const;
  [[nodiscard]] bool retain(const ClaimLootCommand &command,
                            ClaimLootTerminalResult result);
  void markBattleCompleted(std::uint64_t completedAtNanos) noexcept;
  [[nodiscard]] std::size_t evictExpired(std::uint64_t nowNanos);
  void markBattleCompleted(Clock::time_point completedAt) noexcept;
  [[nodiscard]] std::size_t evictExpired(Clock::time_point now);
  [[nodiscard]] LootResultStoreState exportState() const;
  [[nodiscard]] bool importState(const LootResultStoreState &state);
  [[nodiscard]] std::size_t size() const noexcept;

private:
  struct Record final {
    CommandId commandId;
    DropId dropId;
    ClaimLootTerminalResult result;
  };

  [[nodiscard]] bool
  scopeMatches(const ClaimLootCommand &command) const noexcept;

  shared::SessionId sessionId_;
  shared::SessionGeneration generation_;
  shared::BattleInstanceId battleId_;
  std::deque<Record> records_;
  std::optional<std::uint64_t> battleCompletedAtNanos_;
  [[nodiscard]] std::uint64_t
  compatibilityNanos(Clock::time_point point) const noexcept;
};

// Immutable Battle-produced retention value handed to the Room Cell when the
// completed Battle aggregate is detached after settlement durability. It
// exposes only old-Battle replay/conflict routing; fresh commands are stale.
class RetainedLootResults final {
public:
  using Clock = LootResultStore::Clock;

  [[nodiscard]] shared::BattleInstanceId battleId() const noexcept;
  [[nodiscard]] bool expired(std::uint64_t nowNanos) const noexcept;
  [[nodiscard]] bool expired(Clock::time_point now) const noexcept;
  [[nodiscard]] ClaimLootTerminalResult route(const ClaimLootCommand &command,
                                              std::uint64_t nowNanos) const;
  [[nodiscard]] ClaimLootTerminalResult route(const ClaimLootCommand &command,
                                              Clock::time_point now) const;

private:
  friend class BattleInstance;

  RetainedLootResults(
      shared::BattleInstanceId battleId, std::uint64_t completedAtNanos,
      std::optional<std::uint64_t> compatibilityCompletedAtNanos,
      std::vector<LootResultStore> stores);

  shared::BattleInstanceId battleId_;
  std::uint64_t completedAtNanos_;
  // Only populated by the legacy chrono adapter. It is never exported as
  // Battle state and is kept as an integer to avoid storing a process-local
  // time_point in a result-affecting object.
  mutable std::optional<std::uint64_t> compatibilityCompletedAtNanos_;
  std::vector<LootResultStore> stores_;
};

} // namespace lol::battle
