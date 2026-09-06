#pragma once

#include <lol/battle/BattleDeterministicState.hpp>
#include <lol/battle_continuity/BattleStateCodec.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace lol::battle_continuity {

// LBRC (Loot Battle Recovery Composite), version 1.  The outer image binds
// the existing canonical battle image to the room recovery projection.  All
// numeric values are encoded explicitly in big-endian order.
inline constexpr std::uint32_t kRecoveryStateMagic = 0x4C425243U;
inline constexpr std::uint16_t kRecoveryStateSchemaVersion = 1U;
inline constexpr std::uint32_t kRoomRecoveryStateMagic = 0x4C425252U;
inline constexpr std::uint16_t kRoomRecoveryStateSchemaVersion = 1U;

enum class RoomRecoveryPhase : std::uint8_t {
  Open = 0U,
  Loading = 1U,
  InProgress = 2U,
  AwaitingSettlementDurability = 3U,
};

struct RoomRecoveryState final {
  shared::RoomId roomId;
  std::uint8_t capacity;
  std::uint16_t hostParticipantSlot;
  std::vector<std::uint16_t> memberSlots;
  RoomRecoveryPhase phase;
  std::uint64_t nextBattleOrdinal;

  bool operator==(const RoomRecoveryState &) const = default;
};

struct RecoveryState final {
  battle::BattleDeterministicState battle;
  RoomRecoveryState room;

  bool operator==(const RecoveryState &) const = default;
};

enum class RecoveryStateCodecErrorCode : std::uint8_t {
  Truncated,
  TrailingBytes,
  InvalidMagic,
  UnsupportedSchemaVersion,
  InvalidLength,
  StateTooLarge,
  InvalidRoomId,
  RoomIdMismatch,
  InvalidCapacity,
  InvalidMemberCount,
  InvalidMemberSlot,
  InvalidMemberOrder,
  InvalidHostParticipantSlot,
  InvalidPhase,
  InvalidNextBattleOrdinal,
  BattleStateRejected,
  HashUnavailable,
};

struct RecoveryStateCodecError final {
  RecoveryStateCodecErrorCode code;
  std::size_t offset;
};

struct RoomRecoveryStateEncodeResult final {
  Bytes bytes;
  std::optional<RecoveryStateCodecError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

struct RoomRecoveryStateDecodeResult final {
  std::optional<RoomRecoveryState> state;
  std::optional<RecoveryStateCodecError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

struct RecoveryStateEncodeResult final {
  Bytes bytes;
  std::optional<RecoveryStateCodecError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

struct RecoveryStateDecodeResult final {
  std::optional<RecoveryState> state;
  std::optional<RecoveryStateCodecError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

[[nodiscard]] RoomRecoveryStateEncodeResult
encodeRoomRecoveryState(const RoomRecoveryState &state);

[[nodiscard]] RoomRecoveryStateDecodeResult
decodeRoomRecoveryState(std::span<const std::uint8_t> encodedState);

[[nodiscard]] RecoveryStateEncodeResult encodeRecoveryState(
    const battle::BattleDeterministicState &battleState,
    const RoomRecoveryState &roomState);

[[nodiscard]] RecoveryStateDecodeResult
decodeRecoveryState(std::span<const std::uint8_t> encodedState);

// Hashes the composite bytes.  No placeholder is returned when the crypto
// backend is unavailable; callers decode before accepting an image.
[[nodiscard]] std::optional<Hash>
recoveryStateHash(std::span<const std::uint8_t> encodedState) noexcept;

// Terminal preimages use the composite image as their state prefix while
// retaining the existing canonical, privacy-safe receipt suffix.
[[nodiscard]] BattleResultEncodeResult encodeCanonicalRecoveryTerminalState(
    std::span<const std::uint8_t> canonicalRecoveryStateBytes,
    const TerminalReceiptPayload &terminalReceipt);

[[nodiscard]] std::optional<Hash> canonicalRecoveryTerminalHash(
    std::span<const std::uint8_t> canonicalRecoveryStateBytes,
    const TerminalReceiptPayload &terminalReceipt);

} // namespace lol::battle_continuity
