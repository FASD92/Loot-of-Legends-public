#pragma once

#include <cstdint>
#include <limits>

namespace lol::battle {

// Battle time is the only time representation that crosses the Battle
// aggregate boundary. A live adapter may derive it from a monotonic clock,
// but the aggregate stores and replays the value below. Inputs between ticks
// are assigned to the next tick so the pair always satisfies the canonical
// elapsed-time equation used by the continuity codec.
struct BattleTime final {
  static constexpr std::uint32_t tickHertz = 20;
  static constexpr std::uint64_t tickNanos = 50'000'000ULL;

  std::uint64_t logicalTick{0};
  std::uint64_t battleElapsedNanos{0};

  [[nodiscard]] static constexpr BattleTime
  fromLogicalTick(std::uint64_t tick) noexcept {
    return BattleTime{tick, tick * tickNanos};
  }

  [[nodiscard]] static constexpr BattleTime
  fromElapsedNanos(std::uint64_t elapsedNanos) noexcept {
    const auto wholeTicks = elapsedNanos / tickNanos;
    const auto hasPartialTick = (elapsedNanos % tickNanos) != 0;
    const auto maximumTick =
        std::numeric_limits<std::uint64_t>::max() / tickNanos;
    const auto tick = wholeTicks >= maximumTick
                          ? maximumTick
                          : wholeTicks + (hasPartialTick ? 1ULL : 0ULL);
    return fromLogicalTick(tick);
  }

  [[nodiscard]] constexpr bool valid() const noexcept {
    return logicalTick <=
               std::numeric_limits<std::uint64_t>::max() / tickNanos &&
           battleElapsedNanos == logicalTick * tickNanos;
  }

  bool operator==(const BattleTime &) const = default;
};

} // namespace lol::battle
