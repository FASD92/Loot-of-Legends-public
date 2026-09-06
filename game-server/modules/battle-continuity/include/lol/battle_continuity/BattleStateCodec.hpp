#pragma once

#include <lol/battle/BattleDeterministicState.hpp>
#include <lol/battle_continuity/RecordCodec.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace lol::battle_continuity {

// LBS1 (Loot Battle State, version 1).  The state image is a value protocol,
// not a C++ object dump: every field below is written with an explicit width
// and big-endian byte order.
inline constexpr std::uint32_t kBattleStateMagic = 0x4C425331U;
inline constexpr std::uint16_t kBattleStateSchemaVersion = 1U;
inline constexpr std::size_t kMaximumBattleStateBytes = 1U << 20U;
inline constexpr std::size_t kMaximumBattleParticipants = 10U;
inline constexpr std::size_t kMaximumBattleDrops = 10U;
// Holdings are consolidated by (participant slot, item id), but the Battle
// domain does not cap distinct catalog items at the participant count.  Keep a
// bounded wire limit with room for future v1 catalog growth.
inline constexpr std::size_t kMaximumBattleHoldings = 256U;
inline constexpr std::size_t kMaximumBattleResultRecords = 256U;

enum class BattleStateCodecErrorCode : std::uint8_t {
  Truncated,
  TrailingBytes,
  InvalidMagic,
  UnsupportedSchemaVersion,
  UnsupportedRulesetVersion,
  InvalidLength,
  StateTooLarge,
  InvalidIdentity,
  InvalidLogicalTime,
  InvalidEnum,
  InvalidBoolean,
  InvalidOrdering,
  DuplicateId,
  InvariantViolation,
  HashUnavailable,
};

struct BattleStateCodecError final {
  BattleStateCodecErrorCode code;
  std::size_t offset;
};

struct BattleStateEncodeResult final {
  Bytes bytes;
  std::optional<BattleStateCodecError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

struct BattleStateDecodeResult final {
  std::optional<battle::BattleDeterministicState> state;
  std::optional<BattleStateCodecError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

struct BattleResultEncodeResult final {
  Bytes bytes;
  std::optional<BattleStateCodecError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

// Terminal hash input deliberately excludes
// TerminalReceiptPayload::finalStateHash: that field is the hash output and
// including it would create a circular value. All other receipt fields,
// including the privacy-safe canonical Result and slot-keyed settlement
// receipts, are encoded below in explicit big-endian form.
[[nodiscard]] BattleResultEncodeResult encodeCanonicalTerminalState(
    std::span<const std::uint8_t> canonicalBattleStateBytes,
    const TerminalReceiptPayload &terminalReceipt);

[[nodiscard]] std::optional<Hash> canonicalBattleTerminalHash(
    std::span<const std::uint8_t> canonicalBattleStateBytes,
    const TerminalReceiptPayload &terminalReceipt);

// Encoding canonicalizes semantically unordered collections by their stable
// keys.  The decoder accepts only that canonical order, so a hand-mutated
// out-of-order byte stream is rejected rather than silently changing state.
[[nodiscard]] BattleStateEncodeResult
encodeBattleState(const battle::BattleDeterministicState &state);

[[nodiscard]] BattleStateDecodeResult
decodeBattleState(std::span<const std::uint8_t> encodedState);

// Result payloads intentionally use the same privacy-safe projection as the
// state image: slots and terminal values only, never AccountId, nickname, or
// SessionGeneration.  The payload has no native object padding or pointers.
[[nodiscard]] BattleResultEncodeResult encodeCanonicalBattleResult(
    const battle::BattleDeterministicFinalResult &result);

[[nodiscard]] std::optional<Hash>
battleStateHash(std::span<const std::uint8_t> canonicalStateBytes) noexcept;

} // namespace lol::battle_continuity
