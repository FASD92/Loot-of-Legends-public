#pragma once

#include <lol/battle/BattleProjections.hpp>
#include <lol/battle/LootApi.hpp>
#include <lol/shared/Identifiers.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lol::battle {

// Battle-owned terminal outcome source, separate from the combat terminal
// record: cancellation never creates a combat terminal, and the durable Battle
// terminal source survives later result/ranking conversion. This is internal
// Battle module data, not a wire error number or player visibility.
enum class BattleOutcome : std::uint8_t {
  MonsterDefeated = 1,
  CombatTimeout = 2,
  CancelledNoActiveParticipants = 3,
};

// One ordered Battle-owned result entry. This is internal Battle module data:
// it is not player visibility and not a protocol contract.
struct BattleResultEntry final {
  shared::SessionId sessionId;
  std::string nickname;
  // Terminal projection of the participant exit status at commit time.
  ParticipantExitStatus exitStatus;
  std::uint64_t finalAssetValue;
  std::optional<std::uint32_t> rank;
  bool isTop;

  bool operator==(const BattleResultEntry &) const = default;
};

// Immutable final result value. The committed value is created once; repeated
// reads return equal copies and cannot mutate the Battle-owned value.
struct BattleFinalResult final {
  shared::RoomId roomId;
  shared::BattleInstanceId battleId;
  // Battle-owned terminal outcome. A normal CombatTerminalRecord is explicitly
  // converted from CombatOutcome (1/2) to BattleOutcome (1/2) only when the
  // normal Battle result is ready to commit; a cancellation is 3 and never
  // creates a combat terminal record.
  BattleOutcome outcome;
  std::vector<BattleResultEntry> entries;

  bool operator==(const BattleFinalResult &) const = default;
};

enum class BattleResultState : std::uint8_t {
  NotReady,
  Committed,
  ResultGenerationFailed,
};

// Value projection containing the state and an optional copied
// BattleFinalResult. No mutable pointer or reference escapes Battle.
struct BattleResultProjection final {
  BattleResultState state;
  std::optional<BattleFinalResult> result;

  bool operator==(const BattleResultProjection &) const = default;
};

// Internal build failure reasons. They may distinguish an inconsistent source,
// a missing catalog item, and arithmetic overflow, but there is no wire number.
enum class ResultBuildStatus : std::uint8_t {
  Built,
  InvariantBroken,
  CatalogIncomplete,
  ArithmeticOverflow,
};

// Copy-in immutable snapshot for one pure result build.
struct ResultBuildSource final {
  shared::RoomId roomId;
  shared::BattleInstanceId battleId;
  // Battle-owned terminal outcome, never the combat-only CombatOutcome.
  BattleOutcome outcome;
  std::vector<CapturedParticipant> captured;
  std::vector<LootDropProjection> drops;
  std::vector<LootHoldingProjection> holdings;
};

struct ResultBuildOutcome final {
  ResultBuildStatus status;
  // Set exactly when status is Built; on failure no partial entries, no fake
  // rank, and no winner are ever produced.
  std::optional<BattleFinalResult> result;

  bool operator==(const ResultBuildOutcome &) const = default;
};

// Terminal projection of a captured participant exit status at successful
// result commit: GameplayEligible becomes TerminalPresent, VoluntaryLeft or
// Disconnected becomes TerminalExited, and already-terminal statuses stay.
// Already-terminal inputs are mapped to themselves.
[[nodiscard]] ParticipantExitStatus
terminalExitStatus(ParticipantExitStatus status) noexcept;

// Pure builder of the Battle-owned immutable final result: integer-only
// checked asset sums, deterministic 1,1,3 competition ranking, and
// SessionId-ascending tie order. No floating point, locale, hash iteration
// order, random distribution, or platform-dependent ordering is used.
[[nodiscard]] ResultBuildOutcome
buildFinalResult(const ResultBuildSource &source, const RelicCatalog &catalog);

} // namespace lol::battle
