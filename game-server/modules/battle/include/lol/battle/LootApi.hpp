#pragma once

#include <lol/battle/CombatApi.hpp>
#include <lol/shared/Identifiers.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lol::battle {

// Battle-owned loot identity. shared kernel은 확장하지 않는다.
struct ItemId final {
  std::uint64_t value;

  bool operator==(const ItemId &) const = default;
};

struct DropId final {
  std::uint64_t value;

  bool operator==(const DropId &) const = default;
};

struct DropPosition final {
  std::int32_t xMillimeter;
  std::int32_t yMillimeter;

  bool operator==(const DropPosition &) const = default;
};

struct RelicItem final {
  ItemId itemId;
  std::uint64_t unitValue;

  bool operator==(const RelicItem &) const = default;
};

// Immutable v1 relic catalog snapshot. 생성 후 조회만 가능하고 mutation API가
// 없다. live DB 조회가 아니라 고정된 snapshot value다.
class RelicCatalog final {
public:
  static constexpr std::uint16_t version = 1;

  [[nodiscard]] static RelicCatalog v1Snapshot();

  [[nodiscard]] const std::vector<RelicItem> &items() const noexcept;
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] std::optional<std::uint64_t>
  unitValueOf(ItemId itemId) const noexcept;

private:
  explicit RelicCatalog(std::vector<RelicItem> items) noexcept;

  std::vector<RelicItem> items_;
};

struct RelicDrop final {
  DropId dropId;
  ItemId itemId;
  std::uint64_t quantity;
  DropPosition position;

  bool operator==(const RelicDrop &) const = default;
};

struct RelicRuleset final {
  static constexpr std::uint16_t version = 1;
  static constexpr std::uint32_t minCapturedParticipants = 2;
  static constexpr std::uint32_t maxCapturedParticipants = 10;
  static constexpr std::uint32_t rareCount = 1;
  static constexpr std::int32_t arenaBoundMillimeters = 10000;
  static constexpr std::uint32_t resolutionWindowMillis = 15000;
  static constexpr std::uint32_t claimRatePerSecond = 8;
  static constexpr std::uint32_t claimBurst = 4;
  static constexpr ItemId commonItemId{1};
  static constexpr ItemId rareItemId{2};
};

// Captured participant 수 N(2..10)에 대해 Rare 1개 + Common N-1개를 순서대로
// 생성한다. 범위 밖 N은 nullopt로 거부하며 절대 clamp/fabricate하지 않는다.
// DropId는 1부터 시작해 Battle-local로 strictly increasing하고, 위치는
// (RoomId, BattleInstanceId, rulesetVersion)에서 유도된 결정적 PRNG로
// arena bounds 안에 생성된다. 호출 간 상태는 없다.
[[nodiscard]] std::optional<std::vector<RelicDrop>>
generateDrops(shared::RoomId roomId, shared::BattleInstanceId battleId,
              std::uint16_t rulesetVersion, std::uint32_t capturedCount,
              const RelicCatalog &catalog);

// Server-authority claim command. The client supplies no position, ItemId,
// unit value, quantity, eligibility, or owner; every payload is fixed here.
struct ClaimLootCommand final {
  CommandId commandId;
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
  shared::BattleInstanceId battleId;
  DropId dropId;

  bool operator==(const ClaimLootCommand &) const = default;
};

// Enum integer values are Battle-private/module API details; no wire error
// number or protocol schema exists yet.
enum class ClaimLootResultCode : std::uint16_t {
  Ok = 0,
  NotEligible = 1,
  StaleSession = 2,
  StaleBattle = 3,
  InvalidDrop = 4,
  UnknownDrop = 5,
  OutOfRange = 6,
  AlreadyClaimed = 7,
  Overloaded = 8,
  CommandConflict = 9,
  CatalogRejected = 10,
  // Fresh claim while loot resolution is already Resolved. Module-internal
  // result only; Task 7 owns any wire mapping.
  ResolutionClosed = 11,
};

struct ClaimLootTerminalResult final {
  CommandId commandId;
  shared::BattleInstanceId battleId;
  DropId dropId;
  ClaimLootResultCode code;

  bool operator==(const ClaimLootTerminalResult &) const = default;
};

// Battle-owned loot resolution lifecycle:
// NotStarted (before MonsterDefeated) -> Open (drop generation) -> Resolved
// (all drops terminal: every Drop Claimed or Unclaimed).
enum class LootResolutionState : std::uint8_t {
  NotStarted,
  Open,
  Resolved,
};

// Per-Drop state machine: Available -> Claimed(owner) or Unclaimed by the
// 15-second resolution deadline.
enum class LootDropState : std::uint8_t {
  Available,
  Claimed,
  Unclaimed,
};

// Control deadline correlated by BattleInstanceId. A stale battle is an
// explicit no mutation.
struct LootDeadlineCommand final {
  shared::BattleInstanceId battleId;
};

enum class LootDeadlineResultCode : std::uint8_t {
  Ok,
  NotEligible,
  StaleBattle,
  ResolutionClosed,
};

// Immutable value projections of Battle-owned loot state. No mutable pointer
// or reference leaves Battle; later Battle consumers read these only.
struct LootDropProjection final {
  DropId dropId;
  ItemId itemId;
  std::uint64_t quantity;
  DropPosition position;
  LootDropState state;
  // Valid combinations: Available/Unclaimed have no owner; Claimed has exactly
  // one owner.
  std::optional<shared::SessionId> owner;

  bool operator==(const LootDropProjection &) const = default;
};

struct LootHoldingProjection final {
  shared::SessionId sessionId;
  ItemId itemId;
  std::uint64_t quantity;

  bool operator==(const LootHoldingProjection &) const = default;
};

struct LootProjection final {
  shared::BattleInstanceId battleId;
  LootResolutionState resolution;
  std::vector<LootDropProjection> drops;
  std::vector<LootHoldingProjection> holdings;

  bool operator==(const LootProjection &) const = default;
};

} // namespace lol::battle
