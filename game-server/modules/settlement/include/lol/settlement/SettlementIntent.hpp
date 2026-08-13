#pragma once

#include <lol/shared/Identifiers.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace lol::settlement {

// Ordered 16-byte settlement identity. The id is derived from room, battle,
// and account only; payload changes never change the id.
class SettlementId final {
public:
  using Bytes = std::array<std::uint8_t, 16>;

  explicit SettlementId(Bytes bytes) noexcept;

  [[nodiscard]] const Bytes &bytes() const noexcept;
  auto operator<=>(const SettlementId &) const = default;

private:
  Bytes bytes_;
};

class SettlementBatchId final {
public:
  using Bytes = std::array<std::uint8_t, 16>;

  explicit SettlementBatchId(Bytes bytes) noexcept;

  [[nodiscard]] const Bytes &bytes() const noexcept;
  auto operator<=>(const SettlementBatchId &) const = default;

private:
  Bytes bytes_;
};

// SHA-256 of the canonical payload.
class CanonicalPayloadHash final {
public:
  using Bytes = std::array<std::uint8_t, 32>;

  explicit CanonicalPayloadHash(Bytes bytes) noexcept;

  [[nodiscard]] const Bytes &bytes() const noexcept;
  auto operator<=>(const CanonicalPayloadHash &) const = default;

private:
  Bytes bytes_;
};

enum class BattleOutcome : std::uint8_t {
  MonsterDefeated = 1,
  CombatTimeout = 2,
  CancelledNoActiveParticipants = 3,
};

enum class ParticipantExitStatus : std::uint8_t {
  TerminalPresent = 1,
  TerminalExited = 2,
};

// One item delta in a participant's settlement intent. Sorted by itemId
// during batch creation.
struct ItemDelta final {
  std::uint64_t itemId;
  std::uint64_t quantity;

  bool operator==(const ItemDelta &) const = default;
};

struct ResultCommittedAt final {
  std::uint64_t unixEpochMilliseconds;
  std::uint64_t monotonicNanoseconds;

  bool operator==(const ResultCommittedAt &) const = default;
};

// One captured participant as the batch source presents it. The batch factory
// validates and normalizes this before building the immutable intents.
struct SettlementParticipantSource final {
  shared::AccountId accountId;
  ParticipantExitStatus exitStatus;
  std::vector<ItemDelta> itemDeltas;
  std::uint64_t finalAssetValue;
};

// Copy-in immutable snapshot for one pure batch build. No mutable pointer or
// reference escapes settlement.
struct SettlementIntentBatchSource final {
  shared::RoomId roomId;
  shared::BattleInstanceId battleId;
  BattleOutcome outcome;
  std::uint16_t catalogVersion;
  ResultCommittedAt committedAt;
  std::vector<SettlementParticipantSource> participants;
};

class SettlementIntentBatch;

// Immutable intent value with const access only. Created exactly once by the
// batch factory; repeated reads return equal copies and cannot mutate.
class SettlementIntent final {
public:
  [[nodiscard]] const shared::AccountId &accountId() const noexcept;
  [[nodiscard]] ParticipantExitStatus exitStatus() const noexcept;
  [[nodiscard]] const std::vector<ItemDelta> &itemDeltas() const noexcept;
  [[nodiscard]] std::uint64_t finalAssetValue() const noexcept;
  [[nodiscard]] const shared::RoomId &roomId() const noexcept;
  [[nodiscard]] const shared::BattleInstanceId &battleId() const noexcept;
  [[nodiscard]] BattleOutcome outcome() const noexcept;
  [[nodiscard]] std::uint16_t catalogVersion() const noexcept;
  [[nodiscard]] const ResultCommittedAt &committedAt() const noexcept;
  [[nodiscard]] const SettlementId &id() const noexcept;
  [[nodiscard]] const CanonicalPayloadHash &canonicalHash() const noexcept;

  bool operator==(const SettlementIntent &) const = default;

private:
  friend std::optional<SettlementIntentBatch>
  createSettlementIntentBatch(const SettlementIntentBatchSource &source);

  SettlementIntent(shared::AccountId accountId,
                   ParticipantExitStatus exitStatus,
                   std::vector<ItemDelta> itemDeltas,
                   std::uint64_t finalAssetValue, shared::RoomId roomId,
                   shared::BattleInstanceId battleId, BattleOutcome outcome,
                   std::uint16_t catalogVersion, ResultCommittedAt committedAt,
                   SettlementId id,
                   CanonicalPayloadHash canonicalHash) noexcept;

  shared::AccountId accountId_;
  ParticipantExitStatus exitStatus_;
  std::vector<ItemDelta> itemDeltas_;
  std::uint64_t finalAssetValue_;
  shared::RoomId roomId_;
  shared::BattleInstanceId battleId_;
  BattleOutcome outcome_;
  std::uint16_t catalogVersion_;
  ResultCommittedAt committedAt_;
  SettlementId id_;
  CanonicalPayloadHash canonicalHash_;
};

// Immutable batch value with const access only. Created exactly once by the
// factory; repeated reads return equal copies and cannot mutate.
class SettlementIntentBatch final {
public:
  [[nodiscard]] const SettlementBatchId &id() const noexcept;
  [[nodiscard]] const shared::RoomId &roomId() const noexcept;
  [[nodiscard]] const shared::BattleInstanceId &battleId() const noexcept;
  [[nodiscard]] BattleOutcome outcome() const noexcept;
  [[nodiscard]] std::uint16_t catalogVersion() const noexcept;
  [[nodiscard]] const ResultCommittedAt &committedAt() const noexcept;
  [[nodiscard]] const std::vector<SettlementIntent> &intents() const noexcept;

  bool operator==(const SettlementIntentBatch &) const = default;

private:
  friend std::optional<SettlementIntentBatch>
  createSettlementIntentBatch(const SettlementIntentBatchSource &source);

  SettlementIntentBatch(SettlementBatchId id, shared::RoomId roomId,
                        shared::BattleInstanceId battleId,
                        BattleOutcome outcome, std::uint16_t catalogVersion,
                        ResultCommittedAt committedAt,
                        std::vector<SettlementIntent> intents) noexcept;

  SettlementBatchId id_;
  shared::RoomId roomId_;
  shared::BattleInstanceId battleId_;
  BattleOutcome outcome_;
  std::uint16_t catalogVersion_;
  ResultCommittedAt committedAt_;
  std::vector<SettlementIntent> intents_;
};

// Pure factory of the immutable batch. Returns no value when the source
// violates the settlement invariants; it never produces partial intents.
[[nodiscard]] std::optional<SettlementIntentBatch>
createSettlementIntentBatch(const SettlementIntentBatchSource &source);

// Pure canonical payload bytes of one intent in the frozen encoding.
[[nodiscard]] std::vector<std::uint8_t>
canonicalPayload(const SettlementIntent &intent);

// The canonical payload digest used by the Meta settlement HTTP contract.
[[nodiscard]] CanonicalPayloadHash
hashCanonicalPayload(std::span<const std::uint8_t> payload);

} // namespace lol::settlement
