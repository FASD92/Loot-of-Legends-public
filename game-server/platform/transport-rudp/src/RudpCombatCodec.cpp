#include <lol/transport/rudp/RudpCombatCodec.hpp>

#include <bit>
#include <type_traits>

namespace lol::transport::rudp {
namespace {

constexpr std::uint16_t kAttackIntentMessageId = 27;
constexpr std::uint16_t kAttackTerminalResultMessageId = 28;
constexpr std::uint16_t kMonsterSpawnedMessageId = 29;
constexpr std::uint16_t kCombatTerminalEventMessageId = 30;
constexpr std::uint16_t kMonsterStateSnapshotMessageId = 31;
constexpr std::size_t kAttackIntentPayloadBytes = 32;
constexpr std::size_t kAttackTerminalResultPayloadBytes = 41;
constexpr std::size_t kMonsterSpawnedPayloadBytes = 51;
constexpr std::size_t kCombatTerminalEventPayloadBytes = 44;
constexpr std::size_t kMonsterStateSnapshotPayloadBytes = 29;
constexpr std::uint64_t kMonsterId = 1;
constexpr std::uint32_t kMaximumHitPoints = 1600;
constexpr std::uint32_t kAttackDamage = 20;
constexpr std::uint16_t kRulesetVersion = 1;

template <typename Integer>
void appendUnsigned(std::vector<std::byte> &bytes, Integer value) {
  static_assert(std::is_unsigned_v<Integer>);
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    const auto shift = (sizeof(Integer) - index - 1U) * 8U;
    bytes.push_back(
        static_cast<std::byte>(static_cast<std::uint8_t>(value >> shift)));
  }
}

template <typename Integer>
Integer readUnsigned(std::span<const std::byte> bytes, std::size_t offset) {
  static_assert(std::is_unsigned_v<Integer>);
  Integer value = 0;
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    value = static_cast<Integer>(
        (value << 8U) | static_cast<Integer>(std::to_integer<std::uint8_t>(
                            bytes[offset + index])));
  }
  return value;
}

bool nonzero(RudpCommandId value) noexcept {
  return value.high != 0 || value.low != 0;
}

bool nonzero(RudpEventId value) noexcept {
  return value.high != 0 || value.low != 0;
}

bool validHeader(const RudpHeader &header, RudpFlag flag,
                 std::uint16_t messageId) noexcept {
  return header.flag == flag && header.transportEpoch != 0 &&
         header.messageId == messageId;
}

bool validAttackResultCode(RudpAttackResultCode code) noexcept {
  return static_cast<std::uint16_t>(code) <=
         static_cast<std::uint16_t>(
             RudpAttackResultCode::TerminalAlreadyDecided);
}

bool validCombatOutcome(RudpCombatOutcome outcome) noexcept {
  return static_cast<std::uint8_t>(outcome) <=
         static_cast<std::uint8_t>(RudpCombatOutcome::CombatTimeout);
}

bool validHitPoints(std::uint32_t hitPoints) noexcept {
  return hitPoints <= kMaximumHitPoints && hitPoints % kAttackDamage == 0;
}

bool valid(const RudpAttackIntent &message) noexcept {
  return nonzero(message.commandId) && message.battleInstanceId != 0 &&
         message.targetHint != 0;
}

bool valid(const RudpAttackTerminalResult &message) noexcept {
  return nonzero(message.commandId) && message.battleInstanceId != 0 &&
         validAttackResultCode(message.resultCode) &&
         message.monsterId == kMonsterId &&
         validHitPoints(message.remainingHitPoints) &&
         message.rulesetVersion == kRulesetVersion &&
         validCombatOutcome(message.combatOutcome) &&
         (message.combatOutcome != RudpCombatOutcome::MonsterDefeated ||
          message.remainingHitPoints == 0);
}

bool valid(const RudpMonsterSpawned &message) noexcept {
  return nonzero(message.eventId) && message.battleInstanceId != 0 &&
         message.eventStreamKind == RudpEventStreamKind::CombatLifecycle &&
         message.eventSequence == 1 && message.monsterId == kMonsterId &&
         message.posXMillimeter == 0 && message.posYMillimeter == 0 &&
         message.maximumHitPoints == kMaximumHitPoints &&
         message.rulesetVersion == kRulesetVersion;
}

bool valid(const RudpCombatTerminalEvent &message) noexcept {
  return nonzero(message.eventId) && message.battleInstanceId != 0 &&
         message.eventStreamKind == RudpEventStreamKind::CombatLifecycle &&
         message.eventSequence == 2 &&
         (message.combatOutcome == RudpCombatOutcome::MonsterDefeated ||
          message.combatOutcome == RudpCombatOutcome::CombatTimeout) &&
         message.monsterId == kMonsterId &&
         message.rulesetVersion == kRulesetVersion;
}

bool valid(const RudpMonsterStateSnapshot &message) noexcept {
  if (message.battleInstanceId == 0 || message.snapshotSequence == 0 ||
      message.monsterId != kMonsterId || !validHitPoints(message.hitPoints)) {
    return false;
  }
  switch (message.monsterState) {
  case RudpMonsterState::Alive:
    return message.hitPoints > 0;
  case RudpMonsterState::Dying:
  case RudpMonsterState::Dead:
    return message.hitPoints == 0;
  case RudpMonsterState::TimedOut:
    return message.hitPoints > 0;
  }
  return false;
}

DecodedRudpCombat malformed(const RudpHeader &header) {
  return {.error = RudpCombatCodecError::MalformedPayload,
          .header = header,
          .message = std::nullopt};
}

} // namespace

std::optional<std::vector<std::byte>>
RudpCombatCodec::encode(const RudpHeader &header,
                        const RudpCombatMessage &message) {
  std::vector<std::byte> payload;
  const bool accepted = std::visit(
      [&header, &payload](const auto &value) {
        using Message = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Message, RudpAttackIntent>) {
          if (!validHeader(header, RudpFlag::Reliable,
                           kAttackIntentMessageId) ||
              !valid(value)) {
            return false;
          }
          payload.reserve(kAttackIntentPayloadBytes);
          appendUnsigned(payload, value.commandId.high);
          appendUnsigned(payload, value.commandId.low);
          appendUnsigned(payload, value.battleInstanceId);
          appendUnsigned(payload, value.targetHint);
        } else if constexpr (std::is_same_v<Message,
                                            RudpAttackTerminalResult>) {
          if (!validHeader(header, RudpFlag::Reliable,
                           kAttackTerminalResultMessageId) ||
              !valid(value)) {
            return false;
          }
          payload.reserve(kAttackTerminalResultPayloadBytes);
          appendUnsigned(payload, value.commandId.high);
          appendUnsigned(payload, value.commandId.low);
          appendUnsigned(payload, value.battleInstanceId);
          appendUnsigned(payload, static_cast<std::uint16_t>(value.resultCode));
          appendUnsigned(payload, value.monsterId);
          appendUnsigned(payload, value.remainingHitPoints);
          appendUnsigned(payload, value.rulesetVersion);
          appendUnsigned(payload,
                         static_cast<std::uint8_t>(value.combatOutcome));
        } else if constexpr (std::is_same_v<Message, RudpMonsterSpawned>) {
          if (!validHeader(header, RudpFlag::Reliable,
                           kMonsterSpawnedMessageId) ||
              !valid(value)) {
            return false;
          }
          payload.reserve(kMonsterSpawnedPayloadBytes);
          appendUnsigned(payload, value.eventId.high);
          appendUnsigned(payload, value.eventId.low);
          appendUnsigned(payload, value.battleInstanceId);
          appendUnsigned(payload,
                         static_cast<std::uint8_t>(value.eventStreamKind));
          appendUnsigned(payload, value.eventSequence);
          appendUnsigned(payload, value.monsterId);
          appendUnsigned(payload,
                         std::bit_cast<std::uint32_t>(value.posXMillimeter));
          appendUnsigned(payload,
                         std::bit_cast<std::uint32_t>(value.posYMillimeter));
          appendUnsigned(payload, value.maximumHitPoints);
          appendUnsigned(payload, value.rulesetVersion);
        } else if constexpr (std::is_same_v<Message, RudpCombatTerminalEvent>) {
          if (!validHeader(header, RudpFlag::Reliable,
                           kCombatTerminalEventMessageId) ||
              !valid(value)) {
            return false;
          }
          payload.reserve(kCombatTerminalEventPayloadBytes);
          appendUnsigned(payload, value.eventId.high);
          appendUnsigned(payload, value.eventId.low);
          appendUnsigned(payload, value.battleInstanceId);
          appendUnsigned(payload,
                         static_cast<std::uint8_t>(value.eventStreamKind));
          appendUnsigned(payload, value.eventSequence);
          appendUnsigned(payload,
                         static_cast<std::uint8_t>(value.combatOutcome));
          appendUnsigned(payload, value.monsterId);
          appendUnsigned(payload, value.serverTick);
          appendUnsigned(payload, value.rulesetVersion);
        } else {
          if (!validHeader(header, RudpFlag::Unreliable,
                           kMonsterStateSnapshotMessageId) ||
              !valid(value)) {
            return false;
          }
          payload.reserve(kMonsterStateSnapshotPayloadBytes);
          appendUnsigned(payload, value.battleInstanceId);
          appendUnsigned(payload, value.snapshotSequence);
          appendUnsigned(payload, value.serverTick);
          appendUnsigned(payload, value.monsterId);
          appendUnsigned(payload, value.hitPoints);
          appendUnsigned(payload,
                         static_cast<std::uint8_t>(value.monsterState));
        }
        return true;
      },
      message);
  return accepted ? RudpHeaderCodec::encode(header, payload) : std::nullopt;
}

DecodedRudpCombat RudpCombatCodec::decode(std::span<const std::byte> datagram) {
  const auto decoded = RudpHeaderCodec::decode(datagram);
  if (decoded.error != RudpHeaderError::None || !decoded.header.has_value()) {
    return {.error = RudpCombatCodecError::Header,
            .header = std::nullopt,
            .message = std::nullopt};
  }
  const auto &header = *decoded.header;
  if (header.messageId == kAttackIntentMessageId) {
    if (!validHeader(header, RudpFlag::Reliable, kAttackIntentMessageId) ||
        decoded.payload.size() != kAttackIntentPayloadBytes) {
      return malformed(header);
    }
    const RudpAttackIntent message{
        .commandId =
            RudpCommandId{
                .high = readUnsigned<std::uint64_t>(decoded.payload, 0),
                .low = readUnsigned<std::uint64_t>(decoded.payload, 8),
            },
        .battleInstanceId = readUnsigned<std::uint64_t>(decoded.payload, 16),
        .targetHint = readUnsigned<std::uint64_t>(decoded.payload, 24),
    };
    return valid(message)
               ? DecodedRudpCombat{.error = RudpCombatCodecError::None,
                                   .header = header,
                                   .message = message}
               : malformed(header);
  }
  if (header.messageId == kAttackTerminalResultMessageId) {
    if (!validHeader(header, RudpFlag::Reliable,
                     kAttackTerminalResultMessageId) ||
        decoded.payload.size() != kAttackTerminalResultPayloadBytes) {
      return malformed(header);
    }
    const RudpAttackTerminalResult message{
        .commandId =
            RudpCommandId{
                .high = readUnsigned<std::uint64_t>(decoded.payload, 0),
                .low = readUnsigned<std::uint64_t>(decoded.payload, 8),
            },
        .battleInstanceId = readUnsigned<std::uint64_t>(decoded.payload, 16),
        .resultCode = static_cast<RudpAttackResultCode>(
            readUnsigned<std::uint16_t>(decoded.payload, 24)),
        .monsterId = readUnsigned<std::uint64_t>(decoded.payload, 26),
        .remainingHitPoints = readUnsigned<std::uint32_t>(decoded.payload, 34),
        .rulesetVersion = readUnsigned<std::uint16_t>(decoded.payload, 38),
        .combatOutcome = static_cast<RudpCombatOutcome>(
            readUnsigned<std::uint8_t>(decoded.payload, 40)),
    };
    return valid(message)
               ? DecodedRudpCombat{.error = RudpCombatCodecError::None,
                                   .header = header,
                                   .message = message}
               : malformed(header);
  }
  if (header.messageId == kMonsterSpawnedMessageId) {
    if (!validHeader(header, RudpFlag::Reliable, kMonsterSpawnedMessageId) ||
        decoded.payload.size() != kMonsterSpawnedPayloadBytes) {
      return malformed(header);
    }
    const RudpMonsterSpawned message{
        .eventId =
            RudpEventId{
                .high = readUnsigned<std::uint64_t>(decoded.payload, 0),
                .low = readUnsigned<std::uint64_t>(decoded.payload, 8),
            },
        .battleInstanceId = readUnsigned<std::uint64_t>(decoded.payload, 16),
        .eventStreamKind = static_cast<RudpEventStreamKind>(
            readUnsigned<std::uint8_t>(decoded.payload, 24)),
        .eventSequence = readUnsigned<std::uint32_t>(decoded.payload, 25),
        .monsterId = readUnsigned<std::uint64_t>(decoded.payload, 29),
        .posXMillimeter = std::bit_cast<std::int32_t>(
            readUnsigned<std::uint32_t>(decoded.payload, 37)),
        .posYMillimeter = std::bit_cast<std::int32_t>(
            readUnsigned<std::uint32_t>(decoded.payload, 41)),
        .maximumHitPoints = readUnsigned<std::uint32_t>(decoded.payload, 45),
        .rulesetVersion = readUnsigned<std::uint16_t>(decoded.payload, 49),
    };
    return valid(message)
               ? DecodedRudpCombat{.error = RudpCombatCodecError::None,
                                   .header = header,
                                   .message = message}
               : malformed(header);
  }
  if (header.messageId == kCombatTerminalEventMessageId) {
    if (!validHeader(header, RudpFlag::Reliable,
                     kCombatTerminalEventMessageId) ||
        decoded.payload.size() != kCombatTerminalEventPayloadBytes) {
      return malformed(header);
    }
    const RudpCombatTerminalEvent message{
        .eventId =
            RudpEventId{
                .high = readUnsigned<std::uint64_t>(decoded.payload, 0),
                .low = readUnsigned<std::uint64_t>(decoded.payload, 8),
            },
        .battleInstanceId = readUnsigned<std::uint64_t>(decoded.payload, 16),
        .eventStreamKind = static_cast<RudpEventStreamKind>(
            readUnsigned<std::uint8_t>(decoded.payload, 24)),
        .eventSequence = readUnsigned<std::uint32_t>(decoded.payload, 25),
        .combatOutcome = static_cast<RudpCombatOutcome>(
            readUnsigned<std::uint8_t>(decoded.payload, 29)),
        .monsterId = readUnsigned<std::uint64_t>(decoded.payload, 30),
        .serverTick = readUnsigned<std::uint32_t>(decoded.payload, 38),
        .rulesetVersion = readUnsigned<std::uint16_t>(decoded.payload, 42),
    };
    return valid(message)
               ? DecodedRudpCombat{.error = RudpCombatCodecError::None,
                                   .header = header,
                                   .message = message}
               : malformed(header);
  }
  if (header.messageId != kMonsterStateSnapshotMessageId) {
    return {.error = RudpCombatCodecError::UnsupportedMessage,
            .header = header,
            .message = std::nullopt};
  }
  if (!validHeader(header, RudpFlag::Unreliable,
                   kMonsterStateSnapshotMessageId) ||
      decoded.payload.size() != kMonsterStateSnapshotPayloadBytes) {
    return malformed(header);
  }
  const RudpMonsterStateSnapshot message{
      .battleInstanceId = readUnsigned<std::uint64_t>(decoded.payload, 0),
      .snapshotSequence = readUnsigned<std::uint32_t>(decoded.payload, 8),
      .serverTick = readUnsigned<std::uint32_t>(decoded.payload, 12),
      .monsterId = readUnsigned<std::uint64_t>(decoded.payload, 16),
      .hitPoints = readUnsigned<std::uint32_t>(decoded.payload, 24),
      .monsterState = static_cast<RudpMonsterState>(
          readUnsigned<std::uint8_t>(decoded.payload, 28)),
  };
  return valid(message) ? DecodedRudpCombat{.error = RudpCombatCodecError::None,
                                            .header = header,
                                            .message = message}
                        : malformed(header);
}

} // namespace lol::transport::rudp
