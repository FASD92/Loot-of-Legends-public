#pragma once

#include <lol/battle/CombatApi.hpp>

#include <chrono>
#include <cstddef>
#include <deque>
#include <optional>

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
  void markBattleCompleted(Clock::time_point completedAt) noexcept;
  [[nodiscard]] std::size_t evictExpired(Clock::time_point now);
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
  std::optional<Clock::time_point> battleCompletedAt_;
};

} // namespace lol::battle
