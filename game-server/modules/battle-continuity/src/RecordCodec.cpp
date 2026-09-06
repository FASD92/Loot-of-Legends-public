#include <lol/battle_continuity/RecordCodec.hpp>

#include <openssl/evp.h>

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace lol::battle_continuity {
namespace {

using ByteView = std::span<const std::uint8_t>;

struct PayloadResult final {
  Bytes bytes;
  std::optional<CodecErrorCode> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

struct ReadCursor final {
  ByteView view;
  std::size_t position{0};

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

  [[nodiscard]] bool u16(std::uint16_t &value) noexcept {
    ByteView valueBytes;
    if (!take(sizeof(value), valueBytes)) {
      return false;
    }
    value = static_cast<std::uint16_t>(
        (static_cast<std::uint32_t>(valueBytes[0]) << 8U) |
        static_cast<std::uint32_t>(valueBytes[1]));
    return true;
  }

  [[nodiscard]] bool u32(std::uint32_t &value) noexcept {
    ByteView valueBytes;
    if (!take(sizeof(value), valueBytes)) {
      return false;
    }
    value = (static_cast<std::uint32_t>(valueBytes[0]) << 24U) |
            (static_cast<std::uint32_t>(valueBytes[1]) << 16U) |
            (static_cast<std::uint32_t>(valueBytes[2]) << 8U) |
            static_cast<std::uint32_t>(valueBytes[3]);
    return true;
  }

  [[nodiscard]] bool u64(std::uint64_t &value) noexcept {
    ByteView valueBytes;
    if (!take(sizeof(value), valueBytes)) {
      return false;
    }
    value = (static_cast<std::uint64_t>(valueBytes[0]) << 56U) |
            (static_cast<std::uint64_t>(valueBytes[1]) << 48U) |
            (static_cast<std::uint64_t>(valueBytes[2]) << 40U) |
            (static_cast<std::uint64_t>(valueBytes[3]) << 32U) |
            (static_cast<std::uint64_t>(valueBytes[4]) << 24U) |
            (static_cast<std::uint64_t>(valueBytes[5]) << 16U) |
            (static_cast<std::uint64_t>(valueBytes[6]) << 8U) |
            static_cast<std::uint64_t>(valueBytes[7]);
    return true;
  }

  [[nodiscard]] bool takeBytes(std::size_t count, Bytes &value) noexcept {
    ByteView valueBytes;
    if (!take(count, valueBytes)) {
      return false;
    }
    value.assign(valueBytes.begin(), valueBytes.end());
    return true;
  }

  template <std::size_t N>
  [[nodiscard]] bool array(std::array<std::uint8_t, N> &value) noexcept {
    ByteView valueBytes;
    if (!take(N, valueBytes)) {
      return false;
    }
    std::copy(valueBytes.begin(), valueBytes.end(), value.begin());
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

void appendBytes(Bytes &target, ByteView bytes) {
  target.insert(target.end(), bytes.begin(), bytes.end());
}

template <std::size_t N>
void appendArray(Bytes &target, const std::array<std::uint8_t, N> &bytes) {
  appendBytes(target, bytes);
}

[[nodiscard]] bool isKnownRecordType(RecordType type) noexcept {
  switch (type) {
  case RecordType::BattleStart:
  case RecordType::CommandDecision:
  case RecordType::Checkpoint:
  case RecordType::TerminalReceipt:
  case RecordType::TickCommit:
    return true;
  }
  return false;
}

[[nodiscard]] RecordType
payloadRecordType(const RecordPayload &payload) noexcept {
  return std::visit(
      [](const auto &value) noexcept -> RecordType {
        using Payload = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Payload, BattleStartPayload>) {
          return RecordType::BattleStart;
        } else if constexpr (std::is_same_v<Payload, CommandDecisionPayload>) {
          return RecordType::CommandDecision;
        } else if constexpr (std::is_same_v<Payload, CheckpointPayload>) {
          return RecordType::Checkpoint;
        } else if constexpr (std::is_same_v<Payload, TerminalReceiptPayload>) {
          return RecordType::TerminalReceipt;
        } else {
          return RecordType::TickCommit;
        }
      },
      payload);
}

[[nodiscard]] std::optional<CodecErrorCode>
validateHeader(const RecordHeader &header) noexcept {
  const std::uint64_t roomValue = header.roomId.value();
  const std::uint64_t battleValue = header.battleInstanceId.value();
  const std::uint32_t roomOrigin = static_cast<std::uint32_t>(roomValue >> 32U);
  const std::uint32_t roomOrdinal = static_cast<std::uint32_t>(roomValue);

  if (header.recordSequence == 0U || header.originRecoveryEpoch == 0U ||
      header.writerRecoveryEpoch == 0U || roomValue == 0U ||
      battleValue == 0U || roomOrigin == 0U || roomOrdinal == 0U ||
      roomOrigin != header.originRecoveryEpoch) {
    return CodecErrorCode::InvalidIdentity;
  }
  if (!isKnownRecordType(header.recordType)) {
    return CodecErrorCode::UnsupportedRecordType;
  }
  if (header.logicalTick >
          std::numeric_limits<std::uint64_t>::max() / kTickNanos ||
      header.battleElapsedNanos != header.logicalTick * kTickNanos) {
    return CodecErrorCode::InvalidLogicalTime;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<CodecErrorCode>
validateBattleStart(const BattleStartPayload &payload) noexcept {
  if (payload.rulesetVersion != kSupportedBattleRulesetVersion) {
    return CodecErrorCode::UnsupportedRulesetVersion;
  }
  if (payload.battleSeed == 0U || payload.tickHertz != kTickHertz ||
      payload.participants.size() < 2U || payload.participants.size() > 10U) {
    return CodecErrorCode::InvalidPayload;
  }
  std::uint16_t previousSlot = 0U;
  for (const auto &participant : payload.participants) {
    if (participant.participantSlot == 0U ||
        participant.sessionId.value() == 0U ||
        participant.participantSlot <= previousSlot) {
      return CodecErrorCode::InvalidPayload;
    }
    previousSlot = participant.participantSlot;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<CodecErrorCode>
validateCommandDecision(const CommandDecisionPayload &payload) noexcept {
  if (payload.participantSlot > 10U ||
      payload.commandPayload.size() >
          std::numeric_limits<std::uint32_t>::max() ||
      payload.outcomePayload.size() >
          std::numeric_limits<std::uint32_t>::max() ||
      payload.roomRecoveryStateBytes.size() >
          std::numeric_limits<std::uint32_t>::max()) {
    return CodecErrorCode::InvalidPayload;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<CodecErrorCode>
validateCheckpoint(const CheckpointPayload &payload) noexcept {
  if (payload.checkpointSchemaVersion != kCheckpointSchemaVersion ||
      payload.canonicalStateBytes.size() >
          std::numeric_limits<std::uint32_t>::max()) {
    return CodecErrorCode::InvalidPayload;
  }
  const auto hash = canonicalStateHash(payload.canonicalStateBytes);
  if (!hash.has_value()) {
    return CodecErrorCode::HashUnavailable;
  }
  if (*hash != payload.stateHash) {
    return CodecErrorCode::CheckpointHashMismatch;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<CodecErrorCode>
validateTerminal(const TerminalReceiptPayload &payload,
                 const RecordHeader &header) noexcept {
  if (payload.settlementIntentCount != payload.settlements.size() ||
      payload.settlements.size() > std::numeric_limits<std::uint16_t>::max() ||
      payload.resultCommittedBattleElapsedNanos != header.battleElapsedNanos ||
      payload.canonicalResultPayload.size() >
          std::numeric_limits<std::uint32_t>::max()) {
    return CodecErrorCode::InvalidPayload;
  }
  std::uint16_t previousSlot = 0U;
  for (std::size_t index = 0U; index < payload.settlements.size(); ++index) {
    const auto &settlement = payload.settlements[index];
    if (settlement.participantSlot == 0U ||
        settlement.participantSlot <= previousSlot) {
      return CodecErrorCode::InvalidPayload;
    }
    for (std::size_t prior = 0U; prior < index; ++prior) {
      if (settlement.settlementId == payload.settlements[prior].settlementId) {
        return CodecErrorCode::InvalidPayload;
      }
    }
    previousSlot = settlement.participantSlot;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<CodecErrorCode>
validateTickCommit(const TickCommitPayload &payload) noexcept {
  if (payload.firstRecordSequence == 0U ||
      payload.lastDataRecordSequence == 0U ||
      payload.firstRecordSequence > payload.lastDataRecordSequence ||
      payload.recordCount == 0U) {
    return CodecErrorCode::InvalidPayload;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<CodecErrorCode>
validatePayload(const Record &record) noexcept {
  if (payloadRecordType(record.payload) != record.header.recordType) {
    return CodecErrorCode::InvalidPayload;
  }
  return std::visit(
      [&record](const auto &payload) -> std::optional<CodecErrorCode> {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, BattleStartPayload>) {
          if (record.header.logicalTick != 0U ||
              record.header.battleElapsedNanos != 0U) {
            return CodecErrorCode::InvalidLogicalTime;
          }
          return validateBattleStart(payload);
        } else if constexpr (std::is_same_v<Payload, CommandDecisionPayload>) {
          return validateCommandDecision(payload);
        } else if constexpr (std::is_same_v<Payload, CheckpointPayload>) {
          return validateCheckpoint(payload);
        } else if constexpr (std::is_same_v<Payload, TerminalReceiptPayload>) {
          return validateTerminal(payload, record.header);
        } else {
          return validateTickCommit(payload);
        }
      },
      record.payload);
}

[[nodiscard]] std::optional<Hash> dataStateHash(const Record &record) noexcept {
  return std::visit(
      [](const auto &payload) -> std::optional<Hash> {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, BattleStartPayload>) {
          return payload.initialStateHash;
        } else if constexpr (std::is_same_v<Payload, CommandDecisionPayload>) {
          return payload.postDecisionStateHash;
        } else if constexpr (std::is_same_v<Payload, CheckpointPayload>) {
          return payload.stateHash;
        } else if constexpr (std::is_same_v<Payload, TerminalReceiptPayload>) {
          return payload.finalStateHash;
        } else {
          return std::nullopt;
        }
      },
      record.payload);
}

PayloadResult encodePayload(const Record &record) {
  PayloadResult result;
  result.bytes.reserve(128U);
  std::visit(
      [&result](const auto &payload) {
        using Payload = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<Payload, BattleStartPayload>) {
          appendU32(result.bytes, payload.rulesetVersion);
          appendU64(result.bytes, payload.battleSeed);
          appendU32(result.bytes, payload.tickHertz);
          appendArray(result.bytes, payload.initialStateHash);
          appendU16(result.bytes,
                    static_cast<std::uint16_t>(payload.participants.size()));
          for (const auto &participant : payload.participants) {
            appendU16(result.bytes, participant.participantSlot);
            appendU64(result.bytes, participant.sessionId.value());
          }
        } else if constexpr (std::is_same_v<Payload, CommandDecisionPayload>) {
          appendU16(result.bytes, payload.participantSlot);
          appendU16(result.bytes, payload.commandKind);
          appendU64(result.bytes, payload.commandId.high);
          appendU64(result.bytes, payload.commandId.low);
          appendU16(result.bytes, payload.decisionCode);
          appendU32(result.bytes,
                    static_cast<std::uint32_t>(payload.commandPayload.size()));
          appendBytes(result.bytes, payload.commandPayload);
          appendU32(result.bytes,
                    static_cast<std::uint32_t>(payload.outcomePayload.size()));
          appendBytes(result.bytes, payload.outcomePayload);
          appendU32(result.bytes, static_cast<std::uint32_t>(
                                      payload.roomRecoveryStateBytes.size()));
          appendBytes(result.bytes, payload.roomRecoveryStateBytes);
          appendArray(result.bytes, payload.postDecisionStateHash);
        } else if constexpr (std::is_same_v<Payload, CheckpointPayload>) {
          appendU16(result.bytes, payload.checkpointSchemaVersion);
          appendU32(result.bytes, static_cast<std::uint32_t>(
                                      payload.canonicalStateBytes.size()));
          appendBytes(result.bytes, payload.canonicalStateBytes);
          appendArray(result.bytes, payload.stateHash);
        } else if constexpr (std::is_same_v<Payload, TerminalReceiptPayload>) {
          appendU16(result.bytes, payload.terminalReason);
          appendArray(result.bytes, payload.finalStateHash);
          appendU32(result.bytes, static_cast<std::uint32_t>(
                                      payload.canonicalResultPayload.size()));
          appendBytes(result.bytes, payload.canonicalResultPayload);
          appendU64(result.bytes, payload.resultCommittedUnixEpochMilliseconds);
          appendU64(result.bytes, payload.resultCommittedBattleElapsedNanos);
          appendArray(result.bytes, payload.settlementBatchId);
          appendU16(result.bytes, payload.settlementIntentCount);
          for (const auto &settlement : payload.settlements) {
            appendU16(result.bytes, settlement.participantSlot);
            appendArray(result.bytes, settlement.settlementId);
            appendArray(result.bytes, settlement.settlementPayloadHash);
          }
        } else {
          appendU64(result.bytes, payload.firstRecordSequence);
          appendU64(result.bytes, payload.lastDataRecordSequence);
          appendU32(result.bytes, payload.recordCount);
          appendArray(result.bytes, payload.committedStateHash);
        }
      },
      record.payload);
  return result;
}

RecordDecodeResult decodePayload(const RecordHeader &header,
                                 ByteView payloadBytes) {
  ReadCursor cursor{.view = payloadBytes};
  RecordDecodeResult result;
  switch (header.recordType) {
  case RecordType::BattleStart: {
    BattleStartPayload payload{};
    std::uint16_t participantCount = 0U;
    if (!cursor.u32(payload.rulesetVersion) ||
        !cursor.u64(payload.battleSeed) || !cursor.u32(payload.tickHertz) ||
        !cursor.array(payload.initialStateHash) ||
        !cursor.u16(participantCount) || participantCount < 2U ||
        participantCount > 10U) {
      result.error =
          CodecError{.code = CodecErrorCode::Truncated, .offset = 0U};
      return result;
    }
    payload.participants.reserve(participantCount);
    for (std::uint16_t index = 0U; index < participantCount; ++index) {
      std::uint16_t participantSlot = 0U;
      std::uint64_t sessionId = 0U;
      if (!cursor.u16(participantSlot) || !cursor.u64(sessionId)) {
        result.error =
            CodecError{.code = CodecErrorCode::Truncated, .offset = 0U};
        return result;
      }
      payload.participants.push_back(
          BattleStartParticipant{.participantSlot = participantSlot,
                                 .sessionId = shared::SessionId{sessionId}});
    }
    result.record = Record{.header = header, .payload = std::move(payload)};
    break;
  }
  case RecordType::CommandDecision: {
    CommandDecisionPayload payload{};
    std::uint32_t commandLength = 0U;
    std::uint32_t outcomeLength = 0U;
    std::uint32_t roomRecoveryStateLength = 0U;
    if (!cursor.u16(payload.participantSlot) ||
        !cursor.u16(payload.commandKind) ||
        !cursor.u64(payload.commandId.high) ||
        !cursor.u64(payload.commandId.low) ||
        !cursor.u16(payload.decisionCode) || !cursor.u32(commandLength) ||
        !cursor.takeBytes(commandLength, payload.commandPayload) ||
        !cursor.u32(outcomeLength) ||
        !cursor.takeBytes(outcomeLength, payload.outcomePayload) ||
        !cursor.u32(roomRecoveryStateLength) ||
        !cursor.takeBytes(roomRecoveryStateLength,
                          payload.roomRecoveryStateBytes) ||
        !cursor.array(payload.postDecisionStateHash)) {
      result.error =
          CodecError{.code = CodecErrorCode::Truncated, .offset = 0U};
      return result;
    }
    result.record = Record{.header = header, .payload = std::move(payload)};
    break;
  }
  case RecordType::Checkpoint: {
    CheckpointPayload payload{};
    std::uint32_t stateLength = 0U;
    if (!cursor.u16(payload.checkpointSchemaVersion) ||
        !cursor.u32(stateLength) ||
        !cursor.takeBytes(stateLength, payload.canonicalStateBytes) ||
        !cursor.array(payload.stateHash)) {
      result.error =
          CodecError{.code = CodecErrorCode::Truncated, .offset = 0U};
      return result;
    }
    result.record = Record{.header = header, .payload = std::move(payload)};
    break;
  }
  case RecordType::TerminalReceipt: {
    TerminalReceiptPayload payload{};
    std::uint32_t resultLength = 0U;
    if (!cursor.u16(payload.terminalReason) ||
        !cursor.array(payload.finalStateHash) || !cursor.u32(resultLength) ||
        !cursor.takeBytes(resultLength, payload.canonicalResultPayload) ||
        !cursor.u64(payload.resultCommittedUnixEpochMilliseconds) ||
        !cursor.u64(payload.resultCommittedBattleElapsedNanos) ||
        !cursor.array(payload.settlementBatchId) ||
        !cursor.u16(payload.settlementIntentCount)) {
      result.error =
          CodecError{.code = CodecErrorCode::Truncated, .offset = 0U};
      return result;
    }
    payload.settlements.reserve(payload.settlementIntentCount);
    for (std::uint16_t index = 0U; index < payload.settlementIntentCount;
         ++index) {
      SettlementReceipt settlement{};
      if (!cursor.u16(settlement.participantSlot) ||
          !cursor.array(settlement.settlementId) ||
          !cursor.array(settlement.settlementPayloadHash)) {
        result.error =
            CodecError{.code = CodecErrorCode::Truncated, .offset = 0U};
        return result;
      }
      payload.settlements.push_back(settlement);
    }
    result.record = Record{.header = header, .payload = std::move(payload)};
    break;
  }
  case RecordType::TickCommit: {
    TickCommitPayload payload{};
    if (!cursor.u64(payload.firstRecordSequence) ||
        !cursor.u64(payload.lastDataRecordSequence) ||
        !cursor.u32(payload.recordCount) ||
        !cursor.array(payload.committedStateHash)) {
      result.error =
          CodecError{.code = CodecErrorCode::Truncated, .offset = 0U};
      return result;
    }
    result.record = Record{.header = header, .payload = std::move(payload)};
    break;
  }
  }
  if (cursor.position != payloadBytes.size()) {
    result.record.reset();
    result.error =
        CodecError{.code = CodecErrorCode::InvalidLength, .offset = 0U};
  }
  return result;
}

[[nodiscard]] CodecError withOffset(CodecError error,
                                    std::size_t offset) noexcept {
  error.offset += offset;
  return error;
}

} // namespace

std::uint32_t crc32(ByteView bytes) noexcept {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (const std::uint8_t byte : bytes) {
    crc ^= static_cast<std::uint32_t>(byte);
    for (std::uint8_t bit = 0U; bit < 8U; ++bit) {
      const std::uint32_t mask =
          static_cast<std::uint32_t>(-(static_cast<std::int32_t>(crc & 1U)));
      crc = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

std::optional<Hash> canonicalStateHash(ByteView canonicalStateBytes) noexcept {
  Hash hash{};
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digestSize = 0U;
  if (EVP_Digest(canonicalStateBytes.data(), canonicalStateBytes.size(),
                 digest.data(), &digestSize, EVP_sha256(), nullptr) != 1 ||
      digestSize != hash.size()) {
    return std::nullopt;
  }
  std::copy_n(digest.begin(), hash.size(), hash.begin());
  return hash;
}

EncodeResult encodeRecord(const Record &record) {
  EncodeResult result;
  if (const auto error = validateHeader(record.header); error.has_value()) {
    result.error = CodecError{.code = *error, .offset = 0U};
    return result;
  }
  if (const auto error = validatePayload(record); error.has_value()) {
    result.error = CodecError{.code = *error, .offset = 0U};
    return result;
  }

  const PayloadResult payload = encodePayload(record);
  if (!payload.ok()) {
    result.error = CodecError{.code = *payload.error, .offset = 0U};
    return result;
  }
  if (payload.bytes.size() > kMaximumRecordBytes - kRecordEnvelopeBytes) {
    result.error =
        CodecError{.code = CodecErrorCode::RecordTooLarge, .offset = 0U};
    return result;
  }
  const std::size_t recordLength = kRecordEnvelopeBytes + payload.bytes.size();
  if (recordLength > std::numeric_limits<std::uint32_t>::max()) {
    result.error =
        CodecError{.code = CodecErrorCode::RecordTooLarge, .offset = 0U};
    return result;
  }

  result.bytes.reserve(recordLength);
  appendU32(result.bytes, kRecordMagic);
  appendU16(result.bytes, kRecordSchemaVersion);
  appendU16(result.bytes, static_cast<std::uint16_t>(record.header.recordType));
  appendU32(result.bytes, static_cast<std::uint32_t>(recordLength));
  appendU64(result.bytes, record.header.recordSequence);
  appendU32(result.bytes, record.header.originRecoveryEpoch);
  appendU32(result.bytes, record.header.writerRecoveryEpoch);
  appendU64(result.bytes, record.header.roomId.value());
  appendU64(result.bytes, record.header.battleInstanceId.value());
  appendU64(result.bytes, record.header.logicalTick);
  appendU64(result.bytes, record.header.battleElapsedNanos);
  appendU32(result.bytes, static_cast<std::uint32_t>(payload.bytes.size()));
  appendBytes(result.bytes, payload.bytes);
  appendU32(result.bytes, crc32(result.bytes));
  return result;
}

RecordDecodeResult decodeRecord(ByteView encodedRecord) {
  RecordDecodeResult result;
  if (encodedRecord.size() < kRecordEnvelopeBytes) {
    result.error = CodecError{.code = CodecErrorCode::Truncated, .offset = 0U};
    return result;
  }

  ReadCursor headerCursor{.view = encodedRecord};
  std::uint32_t magic = 0U;
  std::uint16_t schemaVersion = 0U;
  std::uint16_t rawRecordType = 0U;
  std::uint32_t recordLength = 0U;
  if (!headerCursor.u32(magic) || !headerCursor.u16(schemaVersion) ||
      !headerCursor.u16(rawRecordType) || !headerCursor.u32(recordLength)) {
    result.error = CodecError{.code = CodecErrorCode::Truncated, .offset = 0U};
    return result;
  }
  if (magic != kRecordMagic) {
    result.error =
        CodecError{.code = CodecErrorCode::InvalidMagic, .offset = 0U};
    return result;
  }
  if (schemaVersion != kRecordSchemaVersion) {
    result.error = CodecError{.code = CodecErrorCode::UnsupportedSchemaVersion,
                              .offset = 4U};
    return result;
  }
  const auto recordType = static_cast<RecordType>(rawRecordType);
  if (!isKnownRecordType(recordType)) {
    result.error =
        CodecError{.code = CodecErrorCode::UnsupportedRecordType, .offset = 6U};
    return result;
  }
  if (recordLength < kRecordEnvelopeBytes) {
    result.error =
        CodecError{.code = CodecErrorCode::InvalidLength, .offset = 8U};
    return result;
  }
  if (recordLength > kMaximumRecordBytes) {
    result.error =
        CodecError{.code = CodecErrorCode::RecordTooLarge, .offset = 8U};
    return result;
  }
  if (encodedRecord.size() < recordLength) {
    result.error = CodecError{.code = CodecErrorCode::Truncated,
                              .offset = encodedRecord.size()};
    return result;
  }
  if (encodedRecord.size() > recordLength) {
    result.error = CodecError{.code = CodecErrorCode::TrailingBytes,
                              .offset = recordLength};
    return result;
  }

  ReadCursor crcCursor{
      .view = encodedRecord.subspan(recordLength - sizeof(std::uint32_t))};
  std::uint32_t expectedCrc = 0U;
  if (!crcCursor.u32(expectedCrc) ||
      expectedCrc !=
          crc32(encodedRecord.first(recordLength - sizeof(std::uint32_t)))) {
    result.error = CodecError{.code = CodecErrorCode::ChecksumMismatch,
                              .offset = recordLength - sizeof(std::uint32_t)};
    return result;
  }

  ReadCursor cursor{.view = encodedRecord};
  std::uint32_t ignoredMagic = 0U;
  std::uint16_t ignoredSchema = 0U;
  std::uint16_t ignoredType = 0U;
  std::uint32_t ignoredLength = 0U;
  RecordHeader header{.recordType = recordType,
                      .recordSequence = 0U,
                      .originRecoveryEpoch = 0U,
                      .writerRecoveryEpoch = 0U,
                      .roomId = shared::RoomId{0U},
                      .battleInstanceId = shared::BattleInstanceId{0U},
                      .logicalTick = 0U,
                      .battleElapsedNanos = 0U};
  if (!cursor.u32(ignoredMagic) || !cursor.u16(ignoredSchema) ||
      !cursor.u16(ignoredType) || !cursor.u32(ignoredLength) ||
      !cursor.u64(header.recordSequence) ||
      !cursor.u32(header.originRecoveryEpoch) ||
      !cursor.u32(header.writerRecoveryEpoch)) {
    result.error = CodecError{.code = CodecErrorCode::Truncated, .offset = 0U};
    return result;
  }
  std::uint64_t roomId = 0U;
  std::uint64_t battleId = 0U;
  if (!cursor.u64(roomId) || !cursor.u64(battleId) ||
      !cursor.u64(header.logicalTick) ||
      !cursor.u64(header.battleElapsedNanos)) {
    result.error = CodecError{.code = CodecErrorCode::Truncated, .offset = 0U};
    return result;
  }
  std::uint32_t payloadLength = 0U;
  if (!cursor.u32(payloadLength)) {
    result.error = CodecError{.code = CodecErrorCode::Truncated, .offset = 60U};
    return result;
  }
  if (recordLength != kRecordEnvelopeBytes + payloadLength) {
    result.error =
        CodecError{.code = CodecErrorCode::InvalidLength, .offset = 8U};
    return result;
  }
  if (payloadLength > recordLength - kRecordEnvelopeBytes) {
    result.error =
        CodecError{.code = CodecErrorCode::InvalidLength, .offset = 60U};
    return result;
  }
  header.roomId = shared::RoomId{roomId};
  header.battleInstanceId = shared::BattleInstanceId{battleId};
  ByteView payloadBytes;
  if (!cursor.take(payloadLength, payloadBytes)) {
    result.error = CodecError{.code = CodecErrorCode::Truncated,
                              .offset = cursor.position};
    return result;
  }
  if (const auto error = validateHeader(header); error.has_value()) {
    result.error = CodecError{.code = *error, .offset = 0U};
    return result;
  }

  result = decodePayload(header, payloadBytes);
  if (!result.ok()) {
    return result;
  }
  if (const auto error = validatePayload(*result.record); error.has_value()) {
    result.record.reset();
    result.error = CodecError{.code = *error, .offset = kRecordEnvelopeBytes};
    return result;
  }
  return result;
}

JournalDecodeResult decodeJournal(ByteView encodedJournal) {
  JournalDecodeResult result;
  if (encodedJournal.size() > kMaximumJournalBytes) {
    result.error =
        CodecError{.code = CodecErrorCode::JournalTooLarge, .offset = 0U};
    return result;
  }

  std::size_t offset = 0U;
  std::uint64_t expectedSequence = 1U;
  std::optional<BattleIdentity> identity;
  std::uint64_t previousTick = 0U;
  std::optional<std::uint32_t> previousBatchWriterEpoch;
  std::optional<std::uint64_t> currentBatchFirstSequence;
  std::optional<std::uint32_t> currentBatchWriterEpoch;
  std::optional<std::uint64_t> currentBatchTick;
  std::size_t currentBatchDataCount = 0U;
  std::optional<Hash> currentBatchLastStateHash;
  std::vector<Record> stagedBatch;
  bool hasCommittedBatch = false;

  while (offset < encodedJournal.size()) {
    const std::size_t remaining = encodedJournal.size() - offset;
    if (remaining < 12U) {
      result.error =
          CodecError{.code = CodecErrorCode::Truncated, .offset = offset};
      return result;
    }
    ReadCursor lengthCursor{.view = encodedJournal.subspan(offset)};
    std::uint32_t recordLength = 0U;
    std::uint32_t ignoredMagic = 0U;
    std::uint16_t ignoredSchema = 0U;
    std::uint16_t ignoredType = 0U;
    if (!lengthCursor.u32(ignoredMagic) || !lengthCursor.u16(ignoredSchema) ||
        !lengthCursor.u16(ignoredType) || !lengthCursor.u32(recordLength)) {
      result.error =
          CodecError{.code = CodecErrorCode::Truncated, .offset = offset};
      return result;
    }
    if (recordLength > kMaximumRecordBytes) {
      result.error = CodecError{.code = CodecErrorCode::RecordTooLarge,
                                .offset = offset + 8U};
      return result;
    }
    if (recordLength < kRecordEnvelopeBytes) {
      result.error = CodecError{.code = CodecErrorCode::InvalidLength,
                                .offset = offset + 8U};
      return result;
    }
    if (remaining < recordLength) {
      result.error =
          CodecError{.code = CodecErrorCode::Truncated, .offset = offset};
      return result;
    }
    const auto decoded =
        decodeRecord(encodedJournal.subspan(offset, recordLength));
    if (!decoded.ok()) {
      result.error = withOffset(*decoded.error, offset);
      return result;
    }
    const Record &record = *decoded.record;
    if (record.header.recordSequence != expectedSequence) {
      result.error = CodecError{.code = CodecErrorCode::InvalidSequence,
                                .offset = offset + 12U};
      return result;
    }
    if (record.header.logicalTick < previousTick) {
      result.error = CodecError{.code = CodecErrorCode::InvalidLogicalTime,
                                .offset = offset + 44U};
      return result;
    }
    const BattleIdentity recordIdentity{
        .originRecoveryEpoch = record.header.originRecoveryEpoch,
        .roomId = record.header.roomId,
        .battleInstanceId = record.header.battleInstanceId};
    if (!identity.has_value()) {
      identity = recordIdentity;
      if (record.header.recordType != RecordType::BattleStart) {
        result.error = CodecError{.code = CodecErrorCode::InvalidSequence,
                                  .offset = offset + 6U};
        return result;
      }
    } else if (*identity != recordIdentity) {
      result.error = CodecError{.code = CodecErrorCode::IdentityMismatch,
                                .offset = offset + 20U};
      return result;
    }

    if (record.header.recordType == RecordType::BattleStart &&
        record.header.recordSequence != 1U) {
      result.error = CodecError{.code = CodecErrorCode::BatchInvariantViolation,
                                .offset = offset + 6U};
      return result;
    }

    if (!currentBatchFirstSequence.has_value()) {
      if (record.header.recordType == RecordType::TickCommit) {
        result.error =
            CodecError{.code = CodecErrorCode::BatchInvariantViolation,
                       .offset = offset + 6U};
        return result;
      }
      currentBatchFirstSequence = record.header.recordSequence;
      currentBatchWriterEpoch = record.header.writerRecoveryEpoch;
      currentBatchTick = record.header.logicalTick;
      currentBatchDataCount = 0U;
      currentBatchLastStateHash.reset();
      if (previousBatchWriterEpoch.has_value() &&
          *currentBatchWriterEpoch < *previousBatchWriterEpoch) {
        result.error = CodecError{.code = CodecErrorCode::WriterEpochRegression,
                                  .offset = offset + 24U};
        return result;
      }
    } else if (record.header.logicalTick != *currentBatchTick) {
      result.error = CodecError{.code = CodecErrorCode::BatchInvariantViolation,
                                .offset = offset + 44U};
      return result;
    } else if (record.header.writerRecoveryEpoch != *currentBatchWriterEpoch) {
      result.error = CodecError{
          .code = record.header.writerRecoveryEpoch < *currentBatchWriterEpoch
                      ? CodecErrorCode::WriterEpochRegression
                      : CodecErrorCode::WriterEpochPerBatch,
          .offset = offset + 24U};
      return result;
    }

    if (!hasCommittedBatch && stagedBatch.size() == 1U) {
      const auto *start =
          std::get_if<BattleStartPayload>(&stagedBatch.front().payload);
      const auto *initialCheckpoint =
          std::get_if<CheckpointPayload>(&record.payload);
      if (start == nullptr || initialCheckpoint == nullptr ||
          record.header.recordSequence != 2U ||
          record.header.logicalTick != 0U ||
          initialCheckpoint->stateHash != start->initialStateHash) {
        result.error =
            CodecError{.code = CodecErrorCode::BatchInvariantViolation,
                       .offset = offset + 6U};
        return result;
      }
    }

    if (record.header.recordType == RecordType::TickCommit) {
      const auto &commit = std::get<TickCommitPayload>(record.payload);
      bool terminalBatchValid = true;
      for (std::size_t index = 0U; index < stagedBatch.size(); ++index) {
        if (stagedBatch[index].header.recordType ==
                RecordType::TerminalReceipt &&
            (index + 2U != stagedBatch.size() ||
             stagedBatch.back().header.recordType != RecordType::Checkpoint)) {
          terminalBatchValid = false;
        }
      }
      const bool lastSequenceCanAdvance =
          commit.lastDataRecordSequence <
          std::numeric_limits<std::uint64_t>::max();
      const std::uint64_t expectedRecordCount =
          lastSequenceCanAdvance
              ? commit.lastDataRecordSequence - commit.firstRecordSequence + 1U
              : 0U;
      if ((!hasCommittedBatch &&
           (record.header.recordSequence != 3U || stagedBatch.size() != 2U ||
            stagedBatch[0].header.recordType != RecordType::BattleStart ||
            stagedBatch[1].header.recordType != RecordType::Checkpoint)) ||
          !terminalBatchValid || !currentBatchLastStateHash.has_value() ||
          commit.firstRecordSequence != *currentBatchFirstSequence ||
          !lastSequenceCanAdvance ||
          commit.lastDataRecordSequence + 1U != record.header.recordSequence ||
          expectedRecordCount != commit.recordCount ||
          currentBatchDataCount != commit.recordCount ||
          commit.committedStateHash != *currentBatchLastStateHash) {
        result.error =
            CodecError{.code = CodecErrorCode::BatchInvariantViolation,
                       .offset = offset + kRecordEnvelopeBytes};
        return result;
      }
      previousBatchWriterEpoch = currentBatchWriterEpoch;
      result.records.insert(result.records.end(), stagedBatch.begin(),
                            stagedBatch.end());
      result.records.push_back(record);
      result.committedBytes = offset + recordLength;
      stagedBatch.clear();
      hasCommittedBatch = true;
      currentBatchFirstSequence.reset();
      currentBatchWriterEpoch.reset();
      currentBatchTick.reset();
      currentBatchDataCount = 0U;
      currentBatchLastStateHash.reset();
    } else {
      const auto stateHash = dataStateHash(record);
      if (!stateHash.has_value()) {
        result.error =
            CodecError{.code = CodecErrorCode::BatchInvariantViolation,
                       .offset = offset + 6U};
        return result;
      }
      currentBatchLastStateHash = *stateHash;
      ++currentBatchDataCount;
      stagedBatch.push_back(record);
    }

    previousTick = record.header.logicalTick;
    offset += recordLength;
    if (expectedSequence == std::numeric_limits<std::uint64_t>::max()) {
      if (offset != encodedJournal.size()) {
        result.error = CodecError{.code = CodecErrorCode::InvalidSequence,
                                  .offset = offset};
        return result;
      }
      break;
    }
    ++expectedSequence;
  }

  if (currentBatchFirstSequence.has_value()) {
    result.error = CodecError{.code = CodecErrorCode::JournalNotCommitted,
                              .offset = encodedJournal.size()};
  }
  return result;
}

std::optional<HashDivergence>
firstDivergence(std::span<const HashObservation> expected,
                std::span<const HashObservation> actual) noexcept {
  const std::size_t count = std::max(expected.size(), actual.size());
  for (std::size_t index = 0U; index < count; ++index) {
    const bool expectedPresent = index < expected.size();
    const bool actualPresent = index < actual.size();
    if (expectedPresent && actualPresent &&
        expected[index].recordSequence == actual[index].recordSequence &&
        expected[index].logicalTick == actual[index].logicalTick &&
        expected[index].stateHash == actual[index].stateHash) {
      continue;
    }
    const auto &expectedValue =
        expectedPresent ? expected[index] : HashObservation{};
    const auto &actualValue = actualPresent ? actual[index] : HashObservation{};
    return HashDivergence{
        .index = index,
        .recordSequence = actualPresent ? actualValue.recordSequence
                                        : expectedValue.recordSequence,
        .logicalTick =
            actualPresent ? actualValue.logicalTick : expectedValue.logicalTick,
        .expectedHash = expectedValue.stateHash,
        .actualHash = actualValue.stateHash,
        .expectedPresent = expectedPresent,
        .actualPresent = actualPresent};
  }
  return std::nullopt;
}

} // namespace lol::battle_continuity
