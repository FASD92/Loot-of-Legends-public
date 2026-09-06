#include <lol/battle_continuity/RecoveryEnvelopeCodec.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <type_traits>

namespace lol::battle_continuity {
namespace {

using ByteView = std::span<const std::uint8_t>;

struct ReadCursor final {
  ByteView view;
  std::size_t position{0U};

  [[nodiscard]] std::size_t remaining() const noexcept {
    return view.size() - position;
  }

  [[nodiscard]] bool take(std::size_t count, ByteView &result) noexcept {
    if (count > remaining()) {
      return false;
    }
    result = view.subspan(position, count);
    position += count;
    return true;
  }

  [[nodiscard]] bool u8(std::uint8_t &value) noexcept {
    ByteView bytes;
    if (!take(sizeof(value), bytes)) {
      return false;
    }
    value = bytes.front();
    return true;
  }

  [[nodiscard]] bool u16(std::uint16_t &value) noexcept {
    ByteView bytes;
    if (!take(sizeof(value), bytes)) {
      return false;
    }
    value = static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(bytes[0]) << 8U) |
        static_cast<std::uint32_t>(bytes[1]));
    return true;
  }

  [[nodiscard]] bool u32(std::uint32_t &value) noexcept {
    ByteView bytes;
    if (!take(sizeof(value), bytes)) {
      return false;
    }
    value = (static_cast<std::uint32_t>(bytes[0]) << 24U) |
            (static_cast<std::uint32_t>(bytes[1]) << 16U) |
            (static_cast<std::uint32_t>(bytes[2]) << 8U) |
            static_cast<std::uint32_t>(bytes[3]);
    return true;
  }

  [[nodiscard]] bool u64(std::uint64_t &value) noexcept {
    ByteView bytes;
    if (!take(sizeof(value), bytes)) {
      return false;
    }
    value = (static_cast<std::uint64_t>(bytes[0]) << 56U) |
            (static_cast<std::uint64_t>(bytes[1]) << 48U) |
            (static_cast<std::uint64_t>(bytes[2]) << 40U) |
            (static_cast<std::uint64_t>(bytes[3]) << 32U) |
            (static_cast<std::uint64_t>(bytes[4]) << 24U) |
            (static_cast<std::uint64_t>(bytes[5]) << 16U) |
            (static_cast<std::uint64_t>(bytes[6]) << 8U) |
            static_cast<std::uint64_t>(bytes[7]);
    return true;
  }

  [[nodiscard]] bool string(std::string &value) {
    std::uint16_t length = 0U;
    if (!u16(length)) {
      return false;
    }
    ByteView bytes;
    if (!take(length, bytes)) {
      return false;
    }
    value.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    return true;
  }
};

void appendU16(Bytes &bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
  bytes.push_back(static_cast<std::uint8_t>(value));
}

void appendU32(Bytes &bytes, std::uint32_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
  bytes.push_back(static_cast<std::uint8_t>(value));
}

void appendU64(Bytes &bytes, std::uint64_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value >> 56U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 48U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 40U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 32U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
  bytes.push_back(static_cast<std::uint8_t>(value));
}

void appendString(Bytes &bytes, const std::string &value) {
  appendU16(bytes, static_cast<std::uint16_t>(value.size()));
  bytes.insert(bytes.end(), value.begin(), value.end());
}

bool isZero(const shared::AccountId &accountId) noexcept {
  return std::all_of(accountId.bytes().begin(), accountId.bytes().end(),
                     [](std::uint8_t byte) { return byte == 0U; });
}

[[nodiscard]] std::optional<RecoveryEnvelopeErrorCode>
validateIdentity(const BattleIdentity &identity) noexcept {
  const auto roomValue = identity.roomId.value();
  const auto roomOrigin = static_cast<std::uint32_t>(roomValue >> 32U);
  const auto roomOrdinal = static_cast<std::uint32_t>(roomValue);
  if (identity.originRecoveryEpoch == 0U || roomValue == 0U ||
      roomOrigin == 0U || roomOrdinal == 0U ||
      roomOrigin != identity.originRecoveryEpoch ||
      identity.battleInstanceId.value() == 0U) {
    return RecoveryEnvelopeErrorCode::InvalidIdentity;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<RecoveryEnvelopeErrorCode>
validateEnvelope(const BattleRecoveryEnvelope &envelope) noexcept {
  if (envelope.schemaVersion != kRecoveryEnvelopeSchemaVersion) {
    return RecoveryEnvelopeErrorCode::UnsupportedSchemaVersion;
  }
  if (validateIdentity(envelope.identity).has_value()) {
    return RecoveryEnvelopeErrorCode::InvalidIdentity;
  }
  if (envelope.capacity < 2U || envelope.capacity > 10U) {
    return RecoveryEnvelopeErrorCode::InvalidCapacity;
  }
  if (envelope.roomTitle.empty() ||
      envelope.roomTitle.size() > std::numeric_limits<std::uint16_t>::max()) {
    return RecoveryEnvelopeErrorCode::InvalidString;
  }
  if (envelope.participants.size() < 2U || envelope.participants.size() > 10U ||
      envelope.participants.size() > envelope.capacity) {
    return RecoveryEnvelopeErrorCode::InvalidParticipantCount;
  }

  bool hostFound = false;
  std::uint16_t previousSlot = 0U;
  for (std::size_t index = 0U; index < envelope.participants.size(); ++index) {
    const auto &participant = envelope.participants[index];
    if (participant.participantSlot == envelope.hostParticipantSlot) {
      hostFound = true;
    }
    if (participant.participantSlot == 0U ||
        participant.participantSlot > envelope.capacity) {
      return RecoveryEnvelopeErrorCode::InvalidParticipant;
    }
    if (index != 0U) {
      if (participant.participantSlot == previousSlot) {
        return RecoveryEnvelopeErrorCode::DuplicateParticipantSlot;
      }
      if (participant.participantSlot < previousSlot) {
        return RecoveryEnvelopeErrorCode::InvalidParticipantOrder;
      }
    }
    if (isZero(participant.accountId) || participant.sessionId.value() == 0U ||
        participant.previousGeneration.value() == 0U) {
      return RecoveryEnvelopeErrorCode::InvalidParticipant;
    }
    if (participant.nickname.empty() ||
        participant.nickname.size() >
            std::numeric_limits<std::uint16_t>::max()) {
      return RecoveryEnvelopeErrorCode::InvalidString;
    }
    for (std::size_t prior = 0U; prior < index; ++prior) {
      const auto &previous = envelope.participants[prior];
      if (participant.accountId == previous.accountId) {
        return RecoveryEnvelopeErrorCode::DuplicateAccountId;
      }
      if (participant.sessionId == previous.sessionId) {
        return RecoveryEnvelopeErrorCode::DuplicateSessionId;
      }
    }
    previousSlot = participant.participantSlot;
  }
  if (!hostFound) {
    return RecoveryEnvelopeErrorCode::InvalidHostParticipantSlot;
  }
  return std::nullopt;
}

[[nodiscard]] RecoveryEnvelopeError errorAt(RecoveryEnvelopeErrorCode code,
                                            std::size_t offset) noexcept {
  return RecoveryEnvelopeError{.code = code, .offset = offset};
}

} // namespace

RecoveryEnvelopeEncodeResult
encodeBattleRecoveryEnvelope(const BattleRecoveryEnvelope &envelope) {
  RecoveryEnvelopeEncodeResult result;
  if (const auto error = validateEnvelope(envelope); error.has_value()) {
    result.error = errorAt(*error, 0U);
    return result;
  }

  try {
    Bytes bytes;
    bytes.reserve(4U + 2U + 4U + 8U + 8U + 1U + 2U + 2U +
                  envelope.roomTitle.size() + 2U);
    appendU32(bytes, kRecoveryEnvelopeMagic);
    appendU16(bytes, envelope.schemaVersion);
    appendU32(bytes, envelope.identity.originRecoveryEpoch);
    appendU64(bytes, envelope.identity.roomId.value());
    appendU64(bytes, envelope.identity.battleInstanceId.value());
    bytes.push_back(envelope.capacity);
    appendU16(bytes, envelope.hostParticipantSlot);
    appendString(bytes, envelope.roomTitle);
    appendU16(bytes, static_cast<std::uint16_t>(envelope.participants.size()));
    for (const auto &participant : envelope.participants) {
      appendU16(bytes, participant.participantSlot);
      bytes.insert(bytes.end(), participant.accountId.bytes().begin(),
                   participant.accountId.bytes().end());
      appendU64(bytes, participant.sessionId.value());
      appendU64(bytes, participant.previousGeneration.value());
      appendString(bytes, participant.nickname);
    }
    if (bytes.size() > kMaximumRecoveryEnvelopePlaintextBytes) {
      result.error =
          errorAt(RecoveryEnvelopeErrorCode::PlaintextTooLarge, bytes.size());
      return result;
    }
    result.bytes = std::move(bytes);
  } catch (const std::bad_alloc &) {
    result.bytes.clear();
    result.error = errorAt(RecoveryEnvelopeErrorCode::AllocationFailure, 0U);
  } catch (...) {
    result.bytes.clear();
    result.error = errorAt(RecoveryEnvelopeErrorCode::AllocationFailure, 0U);
  }
  return result;
}

RecoveryEnvelopeDecodeResult
decodeBattleRecoveryEnvelope(std::span<const std::uint8_t> encoded) {
  RecoveryEnvelopeDecodeResult result;
  if (encoded.size() > kMaximumRecoveryEnvelopePlaintextBytes) {
    result.error = errorAt(RecoveryEnvelopeErrorCode::PlaintextTooLarge, 0U);
    return result;
  }

  try {
    ReadCursor cursor{.view = encoded};
    std::uint32_t magic = 0U;
    if (!cursor.u32(magic)) {
      result.error =
          errorAt(RecoveryEnvelopeErrorCode::Truncated, cursor.position);
      return result;
    }
    if (magic != kRecoveryEnvelopeMagic) {
      result.error = errorAt(RecoveryEnvelopeErrorCode::InvalidMagic, 0U);
      return result;
    }

    BattleRecoveryEnvelope envelope{
        .schemaVersion = kRecoveryEnvelopeSchemaVersion,
        .identity =
            BattleIdentity{.originRecoveryEpoch = 0U,
                           .roomId = shared::RoomId{0U},
                           .battleInstanceId = shared::BattleInstanceId{0U}},
        .roomTitle = {},
        .capacity = 0U,
        .hostParticipantSlot = 0U,
        .participants = {}};
    if (!cursor.u16(envelope.schemaVersion)) {
      result.error =
          errorAt(RecoveryEnvelopeErrorCode::Truncated, cursor.position);
      return result;
    }
    if (envelope.schemaVersion != kRecoveryEnvelopeSchemaVersion) {
      result.error =
          errorAt(RecoveryEnvelopeErrorCode::UnsupportedSchemaVersion, 4U);
      return result;
    }

    std::uint32_t originRecoveryEpoch = 0U;
    std::uint64_t roomId = 0U;
    std::uint64_t battleInstanceId = 0U;
    if (!cursor.u32(originRecoveryEpoch) || !cursor.u64(roomId) ||
        !cursor.u64(battleInstanceId)) {
      result.error =
          errorAt(RecoveryEnvelopeErrorCode::Truncated, cursor.position);
      return result;
    }
    envelope.identity = BattleIdentity{
        .originRecoveryEpoch = originRecoveryEpoch,
        .roomId = shared::RoomId{roomId},
        .battleInstanceId = shared::BattleInstanceId{battleInstanceId}};
    if (validateIdentity(envelope.identity).has_value()) {
      result.error = errorAt(RecoveryEnvelopeErrorCode::InvalidIdentity, 6U);
      return result;
    }

    if (!cursor.u8(envelope.capacity)) {
      result.error =
          errorAt(RecoveryEnvelopeErrorCode::Truncated, cursor.position);
      return result;
    }
    if (envelope.capacity < 2U || envelope.capacity > 10U) {
      result.error = errorAt(RecoveryEnvelopeErrorCode::InvalidCapacity,
                             cursor.position - 1U);
      return result;
    }

    if (!cursor.u16(envelope.hostParticipantSlot)) {
      result.error =
          errorAt(RecoveryEnvelopeErrorCode::Truncated, cursor.position);
      return result;
    }
    if (!cursor.string(envelope.roomTitle)) {
      result = RecoveryEnvelopeDecodeResult{
          .envelope = std::nullopt,
          .error =
              errorAt(RecoveryEnvelopeErrorCode::Truncated, cursor.position)};
      return result;
    }
    if (envelope.roomTitle.empty()) {
      result.error =
          errorAt(RecoveryEnvelopeErrorCode::InvalidString, cursor.position);
      return result;
    }

    std::uint16_t participantCount = 0U;
    if (!cursor.u16(participantCount)) {
      result.error =
          errorAt(RecoveryEnvelopeErrorCode::Truncated, cursor.position);
      return result;
    }
    if (participantCount < 2U || participantCount > 10U ||
        participantCount > envelope.capacity) {
      result = RecoveryEnvelopeDecodeResult{
          .envelope = std::nullopt,
          .error = errorAt(RecoveryEnvelopeErrorCode::InvalidParticipantCount,
                           cursor.position - sizeof(participantCount))};
      return result;
    }

    envelope.participants.reserve(participantCount);
    for (std::uint16_t index = 0U; index < participantCount; ++index) {
      RecoveryParticipant participant{
          .participantSlot = 0U,
          .accountId = shared::AccountId{shared::AccountId::Bytes{}},
          .sessionId = shared::SessionId{0U},
          .previousGeneration = shared::SessionGeneration{0U},
          .nickname = {}};
      if (!cursor.u16(participant.participantSlot)) {
        result.error =
            errorAt(RecoveryEnvelopeErrorCode::Truncated, cursor.position);
        return result;
      }
      ByteView accountBytes;
      if (!cursor.take(shared::AccountId::Bytes{}.size(), accountBytes)) {
        result.error =
            errorAt(RecoveryEnvelopeErrorCode::Truncated, cursor.position);
        return result;
      }
      shared::AccountId::Bytes copiedAccount{};
      std::copy(accountBytes.begin(), accountBytes.end(),
                copiedAccount.begin());
      participant.accountId = shared::AccountId{copiedAccount};
      std::uint64_t sessionId = 0U;
      std::uint64_t previousGeneration = 0U;
      if (!cursor.u64(sessionId) || !cursor.u64(previousGeneration) ||
          !cursor.string(participant.nickname)) {
        result.error =
            errorAt(RecoveryEnvelopeErrorCode::Truncated, cursor.position);
        return result;
      }
      participant.sessionId = shared::SessionId{sessionId};
      participant.previousGeneration =
          shared::SessionGeneration{previousGeneration};
      envelope.participants.push_back(std::move(participant));
    }

    if (cursor.position != encoded.size()) {
      result.error =
          errorAt(RecoveryEnvelopeErrorCode::TrailingBytes, cursor.position);
      return result;
    }
    if (const auto error = validateEnvelope(envelope); error.has_value()) {
      result.error = errorAt(*error, 0U);
      return result;
    }
    result.envelope = std::move(envelope);
  } catch (const std::bad_alloc &) {
    result.envelope.reset();
    result.error = errorAt(RecoveryEnvelopeErrorCode::AllocationFailure, 0U);
  } catch (...) {
    result.envelope.reset();
    result.error = errorAt(RecoveryEnvelopeErrorCode::AllocationFailure, 0U);
  }
  return result;
}

} // namespace lol::battle_continuity
