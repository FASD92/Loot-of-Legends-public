#pragma once

#include <lol/battle/BattleTime.hpp>
#include <lol/battle/CombatApi.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>
#include <vector>

namespace lol::battle {

enum class AttackResultStoreDecision : std::uint8_t {
  Available,
  Replay,
  Conflict,
  Overloaded,
  ScopeMismatch,
};

struct AttackResultStoreInspection final {
  AttackResultStoreDecision decision;
  std::optional<AttackTerminalResult> result;
};

// Value-only continuity image. The store scope (including the current
// SessionGeneration) is restored by BattleInstance's private identity map;
// it is deliberately absent here.
struct AttackResultRecordState final {
  CommandId commandId;
  std::uint64_t targetHint;
  AttackTerminalResult result;

  bool operator==(const AttackResultRecordState &) const = default;
};

struct AttackResultStoreState final {
  std::optional<std::uint64_t> battleCompletedAtNanos;
  std::vector<AttackResultRecordState> records;

  bool operator==(const AttackResultStoreState &) const = default;
};

class AttackResultStore final {
public:
  using Clock = std::chrono::steady_clock;
  static constexpr std::size_t maximumResults = 256;
  static constexpr auto retentionAfterBattleCompleted =
      std::chrono::milliseconds{30000};

  AttackResultStore(shared::SessionId sessionId,
                    shared::SessionGeneration generation,
                    shared::BattleInstanceId battleId);

  [[nodiscard]] AttackResultStoreInspection
  inspect(const AttackCommand &command) const;
  [[nodiscard]] bool retain(const AttackCommand &command,
                            AttackTerminalResult result);
  void markBattleCompleted(std::uint64_t completedAtNanos) noexcept;
  [[nodiscard]] std::size_t evictExpired(std::uint64_t nowNanos);
  // Legacy live callers may still pass a process-local clock. These overloads
  // convert at the boundary and never enter the stored state.
  void markBattleCompleted(Clock::time_point completedAt) noexcept;
  [[nodiscard]] std::size_t evictExpired(Clock::time_point now);
  [[nodiscard]] AttackResultStoreState exportState() const;
  [[nodiscard]] bool importState(const AttackResultStoreState &state);
  [[nodiscard]] std::size_t size() const noexcept;

private:
  struct Record final {
    CommandId commandId;
    std::uint64_t targetHint;
    AttackTerminalResult result;
  };

  [[nodiscard]] bool scopeMatches(const AttackCommand &command) const noexcept;

  shared::SessionId sessionId_;
  shared::SessionGeneration generation_;
  shared::BattleInstanceId battleId_;
  std::deque<Record> records_;
  std::optional<std::uint64_t> battleCompletedAtNanos_;
  [[nodiscard]] std::uint64_t
  compatibilityNanos(Clock::time_point point) const noexcept;
};

} // namespace lol::battle
