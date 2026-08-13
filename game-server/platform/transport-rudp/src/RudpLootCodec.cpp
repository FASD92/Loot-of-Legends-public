#include <lol/transport/rudp/RudpLootCodec.hpp>

#include <algorithm>
#include <bit>
#include <type_traits>

namespace lol::transport::rudp {
namespace {

constexpr std::uint16_t kClaimLootIntentMessageId = 32;
constexpr std::uint16_t kClaimLootTerminalResultMessageId = 33;
constexpr std::uint16_t kDropSpawnedMessageId = 34;
constexpr std::uint16_t kDropStateSnapshotMessageId = 35;
constexpr std::size_t kClaimLootIntentPayloadBytes = 32;
constexpr std::size_t kClaimLootTerminalResultPayloadBytes = 34;
constexpr std::size_t kDropSpawnedPayloadBytes = 63;
constexpr std::size_t kDropStateSnapshotPrefixBytes = 15;
constexpr std::size_t kDropProjectionBytes = 41;
constexpr std::size_t kMaximumDrops = 10;
constexpr std::int32_t kArenaBoundMillimeters = 10000;
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

bool validResultCode(RudpClaimLootResultCode code) noexcept {
  return static_cast<std::uint16_t>(code) <=
         static_cast<std::uint16_t>(RudpClaimLootResultCode::ResolutionClosed);
}

bool validPosition(std::int32_t value) noexcept {
  return value >= -kArenaBoundMillimeters && value <= kArenaBoundMillimeters;
}

bool validItem(std::uint64_t itemId) noexcept {
  return itemId == 1 || itemId == 2;
}

bool valid(const RudpClaimLootIntent &message) noexcept {
  return nonzero(message.commandId) && message.battleInstanceId != 0;
}

bool valid(const RudpClaimLootTerminalResult &message) noexcept {
  return nonzero(message.commandId) && message.battleInstanceId != 0 &&
         validResultCode(message.resultCode) &&
         (message.resultCode != RudpClaimLootResultCode::Ok ||
          message.dropId != 0);
}

bool valid(const RudpDropSpawned &message) noexcept {
  return nonzero(message.eventId) && message.battleInstanceId != 0 &&
         message.eventStreamKind == RudpEventStreamKind::LootLifecycle &&
         message.eventSequence != 0 &&
         message.dropId == message.eventSequence &&
         message.dropId <= kMaximumDrops && validItem(message.itemId) &&
         message.quantity == 1 && validPosition(message.posXMillimeter) &&
         validPosition(message.posYMillimeter) &&
         message.rulesetVersion == kRulesetVersion;
}

bool valid(const RudpLootDropProjection &drop) noexcept {
  if (drop.dropId == 0 || drop.dropId > kMaximumDrops ||
      !validItem(drop.itemId) || drop.quantity != 1 ||
      !validPosition(drop.posXMillimeter) ||
      !validPosition(drop.posYMillimeter)) {
    return false;
  }
  switch (drop.state) {
  case RudpLootDropState::Available:
  case RudpLootDropState::Unclaimed:
    return drop.ownerSessionId == 0;
  case RudpLootDropState::Claimed:
    return drop.ownerSessionId != 0;
  }
  return false;
}

bool valid(const RudpDropStateSnapshot &message) noexcept {
  if (message.battleInstanceId == 0 || message.snapshotSequence == 0 ||
      message.drops.size() > kMaximumDrops) {
    return false;
  }
  if (message.resolutionState == RudpLootResolutionState::NotStarted) {
    return message.drops.empty();
  }
  if (message.drops.empty() ||
      (message.resolutionState != RudpLootResolutionState::Open &&
       message.resolutionState != RudpLootResolutionState::Resolved)) {
    return false;
  }
  for (std::size_t index = 0; index < message.drops.size(); ++index) {
    if (!valid(message.drops[index])) {
      return false;
    }
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (message.drops[previous].dropId == message.drops[index].dropId) {
        return false;
      }
    }
  }
  const bool hasAvailable = std::any_of(
      message.drops.begin(), message.drops.end(), [](const auto &drop) {
        return drop.state == RudpLootDropState::Available;
      });
  return (message.resolutionState == RudpLootResolutionState::Open) ==
         hasAvailable;
}

DecodedRudpLoot malformed(const RudpHeader &header) {
  return {.error = RudpLootCodecError::MalformedPayload,
          .header = header,
          .message = std::nullopt};
}

} // namespace

std::optional<std::vector<std::byte>>
RudpLootCodec::encode(const RudpHeader &header,
                      const RudpLootMessage &message) {
  std::vector<std::byte> payload;
  const bool accepted = std::visit(
      [&header, &payload](const auto &value) {
        using Message = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Message, RudpClaimLootIntent>) {
          if (!validHeader(header, RudpFlag::Reliable,
                           kClaimLootIntentMessageId) ||
              !valid(value)) {
            return false;
          }
          payload.reserve(kClaimLootIntentPayloadBytes);
          appendUnsigned(payload, value.commandId.high);
          appendUnsigned(payload, value.commandId.low);
          appendUnsigned(payload, value.battleInstanceId);
          appendUnsigned(payload, value.dropId);
        } else if constexpr (std::is_same_v<Message,
                                            RudpClaimLootTerminalResult>) {
          if (!validHeader(header, RudpFlag::Reliable,
                           kClaimLootTerminalResultMessageId) ||
              !valid(value)) {
            return false;
          }
          payload.reserve(kClaimLootTerminalResultPayloadBytes);
          appendUnsigned(payload, value.commandId.high);
          appendUnsigned(payload, value.commandId.low);
          appendUnsigned(payload, value.battleInstanceId);
          appendUnsigned(payload, value.dropId);
          appendUnsigned(payload, static_cast<std::uint16_t>(value.resultCode));
        } else if constexpr (std::is_same_v<Message, RudpDropSpawned>) {
          if (!validHeader(header, RudpFlag::Reliable, kDropSpawnedMessageId) ||
              !valid(value)) {
            return false;
          }
          payload.reserve(kDropSpawnedPayloadBytes);
          appendUnsigned(payload, value.eventId.high);
          appendUnsigned(payload, value.eventId.low);
          appendUnsigned(payload, value.battleInstanceId);
          appendUnsigned(payload,
                         static_cast<std::uint8_t>(value.eventStreamKind));
          appendUnsigned(payload, value.eventSequence);
          appendUnsigned(payload, value.dropId);
          appendUnsigned(payload, value.itemId);
          appendUnsigned(payload, value.quantity);
          appendUnsigned(payload,
                         std::bit_cast<std::uint32_t>(value.posXMillimeter));
          appendUnsigned(payload,
                         std::bit_cast<std::uint32_t>(value.posYMillimeter));
          appendUnsigned(payload, value.rulesetVersion);
        } else {
          if (!validHeader(header, RudpFlag::Unreliable,
                           kDropStateSnapshotMessageId) ||
              !valid(value)) {
            return false;
          }
          payload.reserve(kDropStateSnapshotPrefixBytes +
                          kDropProjectionBytes * value.drops.size());
          appendUnsigned(payload, value.battleInstanceId);
          appendUnsigned(payload, value.snapshotSequence);
          appendUnsigned(payload,
                         static_cast<std::uint8_t>(value.resolutionState));
          appendUnsigned(payload,
                         static_cast<std::uint16_t>(value.drops.size()));
          for (const auto &drop : value.drops) {
            appendUnsigned(payload, drop.dropId);
            appendUnsigned(payload, drop.itemId);
            appendUnsigned(payload, drop.quantity);
            appendUnsigned(payload,
                           std::bit_cast<std::uint32_t>(drop.posXMillimeter));
            appendUnsigned(payload,
                           std::bit_cast<std::uint32_t>(drop.posYMillimeter));
            appendUnsigned(payload, static_cast<std::uint8_t>(drop.state));
            appendUnsigned(payload, drop.ownerSessionId);
          }
        }
        return true;
      },
      message);
  return accepted ? RudpHeaderCodec::encode(header, payload) : std::nullopt;
}

DecodedRudpLoot RudpLootCodec::decode(std::span<const std::byte> datagram) {
  const auto decoded = RudpHeaderCodec::decode(datagram);
  if (decoded.error != RudpHeaderError::None || !decoded.header.has_value()) {
    return {.error = RudpLootCodecError::Header,
            .header = std::nullopt,
            .message = std::nullopt};
  }
  const auto &header = *decoded.header;
  if (header.messageId == kClaimLootIntentMessageId) {
    if (!validHeader(header, RudpFlag::Reliable, kClaimLootIntentMessageId) ||
        decoded.payload.size() != kClaimLootIntentPayloadBytes) {
      return malformed(header);
    }
    const RudpClaimLootIntent message{
        .commandId = {.high = readUnsigned<std::uint64_t>(decoded.payload, 0),
                      .low = readUnsigned<std::uint64_t>(decoded.payload, 8)},
        .battleInstanceId = readUnsigned<std::uint64_t>(decoded.payload, 16),
        .dropId = readUnsigned<std::uint64_t>(decoded.payload, 24),
    };
    return valid(message) ? DecodedRudpLoot{.error = RudpLootCodecError::None,
                                            .header = header,
                                            .message = message}
                          : malformed(header);
  }
  if (header.messageId == kClaimLootTerminalResultMessageId) {
    if (!validHeader(header, RudpFlag::Reliable,
                     kClaimLootTerminalResultMessageId) ||
        decoded.payload.size() != kClaimLootTerminalResultPayloadBytes) {
      return malformed(header);
    }
    const RudpClaimLootTerminalResult message{
        .commandId = {.high = readUnsigned<std::uint64_t>(decoded.payload, 0),
                      .low = readUnsigned<std::uint64_t>(decoded.payload, 8)},
        .battleInstanceId = readUnsigned<std::uint64_t>(decoded.payload, 16),
        .dropId = readUnsigned<std::uint64_t>(decoded.payload, 24),
        .resultCode = static_cast<RudpClaimLootResultCode>(
            readUnsigned<std::uint16_t>(decoded.payload, 32)),
    };
    return valid(message) ? DecodedRudpLoot{.error = RudpLootCodecError::None,
                                            .header = header,
                                            .message = message}
                          : malformed(header);
  }
  if (header.messageId == kDropSpawnedMessageId) {
    if (!validHeader(header, RudpFlag::Reliable, kDropSpawnedMessageId) ||
        decoded.payload.size() != kDropSpawnedPayloadBytes) {
      return malformed(header);
    }
    const RudpDropSpawned message{
        .eventId = {.high = readUnsigned<std::uint64_t>(decoded.payload, 0),
                    .low = readUnsigned<std::uint64_t>(decoded.payload, 8)},
        .battleInstanceId = readUnsigned<std::uint64_t>(decoded.payload, 16),
        .eventStreamKind = static_cast<RudpEventStreamKind>(
            readUnsigned<std::uint8_t>(decoded.payload, 24)),
        .eventSequence = readUnsigned<std::uint32_t>(decoded.payload, 25),
        .dropId = readUnsigned<std::uint64_t>(decoded.payload, 29),
        .itemId = readUnsigned<std::uint64_t>(decoded.payload, 37),
        .quantity = readUnsigned<std::uint64_t>(decoded.payload, 45),
        .posXMillimeter = std::bit_cast<std::int32_t>(
            readUnsigned<std::uint32_t>(decoded.payload, 53)),
        .posYMillimeter = std::bit_cast<std::int32_t>(
            readUnsigned<std::uint32_t>(decoded.payload, 57)),
        .rulesetVersion = readUnsigned<std::uint16_t>(decoded.payload, 61),
    };
    return valid(message) ? DecodedRudpLoot{.error = RudpLootCodecError::None,
                                            .header = header,
                                            .message = message}
                          : malformed(header);
  }
  if (header.messageId != kDropStateSnapshotMessageId) {
    return {.error = RudpLootCodecError::UnsupportedMessage,
            .header = header,
            .message = std::nullopt};
  }
  if (!validHeader(header, RudpFlag::Unreliable, kDropStateSnapshotMessageId) ||
      decoded.payload.size() < kDropStateSnapshotPrefixBytes) {
    return malformed(header);
  }
  const auto dropCount = readUnsigned<std::uint16_t>(decoded.payload, 13);
  if (dropCount > kMaximumDrops ||
      decoded.payload.size() !=
          kDropStateSnapshotPrefixBytes + kDropProjectionBytes * dropCount) {
    return malformed(header);
  }
  RudpDropStateSnapshot message{
      .battleInstanceId = readUnsigned<std::uint64_t>(decoded.payload, 0),
      .snapshotSequence = readUnsigned<std::uint32_t>(decoded.payload, 8),
      .resolutionState = static_cast<RudpLootResolutionState>(
          readUnsigned<std::uint8_t>(decoded.payload, 12)),
      .drops = {},
  };
  message.drops.reserve(dropCount);
  for (std::size_t index = 0; index < dropCount; ++index) {
    const auto offset =
        kDropStateSnapshotPrefixBytes + index * kDropProjectionBytes;
    message.drops.push_back(RudpLootDropProjection{
        .dropId = readUnsigned<std::uint64_t>(decoded.payload, offset),
        .itemId = readUnsigned<std::uint64_t>(decoded.payload, offset + 8),
        .quantity = readUnsigned<std::uint64_t>(decoded.payload, offset + 16),
        .posXMillimeter = std::bit_cast<std::int32_t>(
            readUnsigned<std::uint32_t>(decoded.payload, offset + 24)),
        .posYMillimeter = std::bit_cast<std::int32_t>(
            readUnsigned<std::uint32_t>(decoded.payload, offset + 28)),
        .state = static_cast<RudpLootDropState>(
            readUnsigned<std::uint8_t>(decoded.payload, offset + 32)),
        .ownerSessionId =
            readUnsigned<std::uint64_t>(decoded.payload, offset + 33),
    });
  }
  return valid(message) ? DecodedRudpLoot{.error = RudpLootCodecError::None,
                                          .header = header,
                                          .message = std::move(message)}
                        : malformed(header);
}

} // namespace lol::transport::rudp
