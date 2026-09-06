#pragma once

#include <lol/battle_continuity/RecordCodec.hpp>
#include <lol/shared/Identifiers.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lol::battle_continuity {

// This is the plaintext payload encrypted by the platform sidecar.  It is
// deliberately separate from the sidecar envelope framing and contains no
// credentials or transport state.
inline constexpr std::uint32_t kRecoveryEnvelopeMagic = 0x4C425231U; // LBR1
inline constexpr std::uint16_t kRecoveryEnvelopeSchemaVersion = 1U;
inline constexpr std::size_t kMaximumRecoveryEnvelopePlaintextBytes = 1U << 20U;

struct RecoveryParticipant final {
  std::uint16_t participantSlot;
  shared::AccountId accountId;
  shared::SessionId sessionId;
  shared::SessionGeneration previousGeneration;
  std::string nickname;

  bool operator==(const RecoveryParticipant &) const = default;
};

struct BattleRecoveryEnvelope final {
  std::uint16_t schemaVersion = kRecoveryEnvelopeSchemaVersion;
  BattleIdentity identity;
  std::string roomTitle;
  std::uint8_t capacity;
  std::uint16_t hostParticipantSlot;
  std::vector<RecoveryParticipant> participants;

  bool operator==(const BattleRecoveryEnvelope &) const = default;
};

enum class RecoveryEnvelopeErrorCode : std::uint8_t {
  Truncated,
  TrailingBytes,
  InvalidMagic,
  UnsupportedSchemaVersion,
  InvalidIdentity,
  InvalidCapacity,
  InvalidParticipantCount,
  InvalidParticipant,
  InvalidParticipantOrder,
  DuplicateParticipantSlot,
  DuplicateAccountId,
  DuplicateSessionId,
  InvalidHostParticipantSlot,
  InvalidString,
  PlaintextTooLarge,
  AllocationFailure,
};

struct RecoveryEnvelopeError final {
  RecoveryEnvelopeErrorCode code;
  std::size_t offset;
};

struct RecoveryEnvelopeEncodeResult final {
  Bytes bytes;
  std::optional<RecoveryEnvelopeError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

struct RecoveryEnvelopeDecodeResult final {
  std::optional<BattleRecoveryEnvelope> envelope;
  std::optional<RecoveryEnvelopeError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

[[nodiscard]] RecoveryEnvelopeEncodeResult
encodeBattleRecoveryEnvelope(const BattleRecoveryEnvelope &envelope);

[[nodiscard]] RecoveryEnvelopeDecodeResult
decodeBattleRecoveryEnvelope(std::span<const std::uint8_t> encoded);

[[nodiscard]] inline RecoveryEnvelopeDecodeResult
decodeBattleRecoveryEnvelope(const Bytes &encoded) {
  return decodeBattleRecoveryEnvelope(
      std::span<const std::uint8_t>{encoded.data(), encoded.size()});
}

} // namespace lol::battle_continuity
