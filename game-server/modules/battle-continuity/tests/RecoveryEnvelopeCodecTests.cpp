#include <lol/battle_continuity/RecoveryEnvelopeCodec.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>

namespace {

using namespace lol::battle_continuity;
using lol::shared::AccountId;
using lol::shared::BattleInstanceId;
using lol::shared::RoomId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;

constexpr std::uint32_t kOriginEpoch = 7U;
constexpr std::uint64_t kRoomValue =
    (static_cast<std::uint64_t>(kOriginEpoch) << 32U) | 11U;
constexpr std::uint64_t kBattleValue = 31U;

AccountId account(std::uint8_t suffix) {
  AccountId::Bytes bytes{};
  bytes.back() = suffix;
  return AccountId{bytes};
}

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

void appendParticipant(Bytes &bytes, const RecoveryParticipant &participant) {
  appendU16(bytes, participant.participantSlot);
  bytes.insert(bytes.end(), participant.accountId.bytes().begin(),
               participant.accountId.bytes().end());
  appendU64(bytes, participant.sessionId.value());
  appendU64(bytes, participant.previousGeneration.value());
  appendString(bytes, participant.nickname);
}

BattleRecoveryEnvelope validEnvelope() {
  return BattleRecoveryEnvelope{
      .schemaVersion = kRecoveryEnvelopeSchemaVersion,
      .identity =
          BattleIdentity{.originRecoveryEpoch = kOriginEpoch,
                         .roomId = RoomId{kRoomValue},
                         .battleInstanceId = BattleInstanceId{kBattleValue}},
      .roomTitle = "arena",
      .capacity = 2U,
      .hostParticipantSlot = 1U,
      .participants = {
          RecoveryParticipant{.participantSlot = 1U,
                              .accountId = account(1U),
                              .sessionId = SessionId{100U},
                              .previousGeneration = SessionGeneration{3U},
                              .nickname = "one"},
          RecoveryParticipant{.participantSlot = 2U,
                              .accountId = account(2U),
                              .sessionId = SessionId{200U},
                              .previousGeneration = SessionGeneration{4U},
                              .nickname = "two"},
      }};
}

Bytes expectedBytes() {
  const auto envelope = validEnvelope();
  Bytes bytes;
  appendU32(bytes, kRecoveryEnvelopeMagic);
  appendU16(bytes, kRecoveryEnvelopeSchemaVersion);
  appendU32(bytes, kOriginEpoch);
  appendU64(bytes, kRoomValue);
  appendU64(bytes, kBattleValue);
  bytes.push_back(2U);
  appendU16(bytes, 1U);
  appendString(bytes, "arena");
  appendU16(bytes, 2U);
  appendParticipant(bytes, envelope.participants[0]);
  appendParticipant(bytes, envelope.participants[1]);
  return bytes;
}

bool rejected(const Bytes &bytes, RecoveryEnvelopeErrorCode code) {
  const auto decoded = decodeBattleRecoveryEnvelope(bytes);
  return !decoded.ok() && decoded.error.has_value() &&
         decoded.error->code == code;
}

bool roundTrip() {
  const auto source = validEnvelope();
  const auto encoded = encodeBattleRecoveryEnvelope(source);
  if (!encoded.ok()) {
    return false;
  }
  const auto decoded = decodeBattleRecoveryEnvelope(encoded.bytes);
  return decoded.ok() && decoded.envelope.has_value() &&
         *decoded.envelope == source;
}

bool deterministicBytesAndDeclaredFieldsOnly() {
  const auto source = validEnvelope();
  const auto first = encodeBattleRecoveryEnvelope(source);
  const auto second = encodeBattleRecoveryEnvelope(source);
  return first.ok() && second.ok() && first.bytes == second.bytes &&
         first.bytes == expectedBytes();
}

bool malformedFramingRejected() {
  const auto encoded = encodeBattleRecoveryEnvelope(validEnvelope());
  if (!encoded.ok() || encoded.bytes.empty()) {
    return false;
  }
  const auto truncated = Bytes{encoded.bytes.begin(), encoded.bytes.end() - 1};
  if (!rejected(truncated, RecoveryEnvelopeErrorCode::Truncated)) {
    return false;
  }
  auto trailing = encoded.bytes;
  trailing.push_back(0U);
  if (!rejected(trailing, RecoveryEnvelopeErrorCode::TrailingBytes)) {
    return false;
  }
  auto unknownSchema = encoded.bytes;
  unknownSchema[5] = 2U;
  return rejected(unknownSchema,
                  RecoveryEnvelopeErrorCode::UnsupportedSchemaVersion);
}

bool duplicateParticipantFieldsRejected() {
  const auto encoded = encodeBattleRecoveryEnvelope(validEnvelope());
  if (!encoded.ok()) {
    return false;
  }

  // Header is 29 bytes, title is 7 bytes, count is 2 bytes, and the first
  // participant occupies 39 bytes in this hand-checked fixture.
  constexpr std::size_t kSecondParticipantOffset = 77U;
  constexpr std::size_t kSecondAccountOffset = 79U;
  constexpr std::size_t kSecondSessionOffset = 95U;

  auto duplicateSlot = encoded.bytes;
  duplicateSlot[kSecondParticipantOffset] = 0U;
  duplicateSlot[kSecondParticipantOffset + 1U] = 1U;
  if (!rejected(duplicateSlot,
                RecoveryEnvelopeErrorCode::DuplicateParticipantSlot)) {
    return false;
  }

  auto duplicateAccount = encoded.bytes;
  std::copy_n(duplicateAccount.begin() + 40U, 16U,
              duplicateAccount.begin() + kSecondAccountOffset);
  if (!rejected(duplicateAccount,
                RecoveryEnvelopeErrorCode::DuplicateAccountId)) {
    return false;
  }

  auto duplicateSession = encoded.bytes;
  std::copy_n(duplicateSession.begin() + 56U, 8U,
              duplicateSession.begin() + kSecondSessionOffset);
  return rejected(duplicateSession,
                  RecoveryEnvelopeErrorCode::DuplicateSessionId);
}

bool invalidHostAndIdentityRejected() {
  const auto encoded = encodeBattleRecoveryEnvelope(validEnvelope());
  if (!encoded.ok()) {
    return false;
  }

  auto invalidHost = encoded.bytes;
  invalidHost[27] = 0U;
  invalidHost[28] = 9U;
  if (!rejected(invalidHost,
                RecoveryEnvelopeErrorCode::InvalidHostParticipantSlot)) {
    return false;
  }

  auto invalidIdentity = encoded.bytes;
  invalidIdentity[9] = 0U;
  invalidIdentity[10] = 0U;
  invalidIdentity[11] = 0U;
  invalidIdentity[12] = 1U;
  return rejected(invalidIdentity, RecoveryEnvelopeErrorCode::InvalidIdentity);
}

bool run(const char *name, bool (*test)()) {
  const bool passed = test();
  if (!passed) {
    std::cerr << "FAIL: " << name << '\n';
  }
  return passed;
}

} // namespace

int main() {
  return run("round trip", roundTrip) &&
                 run("deterministic bytes and declared fields",
                     deterministicBytesAndDeclaredFieldsOnly) &&
                 run("malformed framing", malformedFramingRejected) &&
                 run("duplicate participant fields",
                     duplicateParticipantFieldsRejected) &&
                 run("invalid host and identity",
                     invalidHostAndIdentityRejected)
             ? 0
             : 1;
}
