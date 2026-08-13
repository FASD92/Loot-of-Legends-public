#include <lol/transport/rudp/RudpMovementCodec.hpp>

#include <bit>
#include <limits>
#include <type_traits>

namespace lol::transport::rudp {
namespace {

constexpr std::uint16_t kMoveIntentMessageId = 25;
constexpr std::uint16_t kStateSnapshotMessageId = 26;
constexpr std::size_t kMoveIntentPayloadBytes = 18;
constexpr std::size_t kSnapshotBasePayloadBytes = 18;
constexpr std::size_t kSnapshotPlayerBytes = 16;
constexpr std::size_t kMaximumPlayers = 10;
constexpr std::int32_t kMinimumPositionMillimeters = -10000;
constexpr std::int32_t kMaximumPositionMillimeters = 10000;

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

bool validHeader(const RudpHeader &header, std::uint16_t messageId) {
  return header.flag == RudpFlag::Unreliable && header.transportEpoch != 0 &&
         header.messageId == messageId;
}

bool validMove(const RudpMoveIntent &move) {
  return move.battleInstanceId != 0 && move.inputFlags == 0 &&
         move.desiredX != std::numeric_limits<std::int16_t>::min() &&
         move.desiredY != std::numeric_limits<std::int16_t>::min();
}

bool validSnapshot(const RudpStateSnapshot &snapshot) {
  if (snapshot.battleInstanceId == 0 ||
      snapshot.players.size() > kMaximumPlayers) {
    return false;
  }
  for (const auto &player : snapshot.players) {
    if (player.sessionId == 0 ||
        player.posXMillimeter < kMinimumPositionMillimeters ||
        player.posXMillimeter > kMaximumPositionMillimeters ||
        player.posYMillimeter < kMinimumPositionMillimeters ||
        player.posYMillimeter > kMaximumPositionMillimeters) {
      return false;
    }
  }
  return true;
}

DecodedRudpMovement malformed(const RudpHeader &header) {
  return {.error = RudpMovementCodecError::MalformedPayload,
          .header = header,
          .message = std::nullopt};
}

} // namespace

std::optional<std::vector<std::byte>>
RudpMovementCodec::encode(const RudpHeader &header,
                          const RudpMovementMessage &message) {
  std::vector<std::byte> payload;
  const bool valid = std::visit(
      [&header, &payload](const auto &value) {
        using Message = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Message, RudpMoveIntent>) {
          if (!validHeader(header, kMoveIntentMessageId) || !validMove(value)) {
            return false;
          }
          payload.reserve(kMoveIntentPayloadBytes);
          appendUnsigned(payload, value.battleInstanceId);
          appendUnsigned(payload, value.actionSequence);
          appendUnsigned(payload, std::bit_cast<std::uint16_t>(value.desiredX));
          appendUnsigned(payload, std::bit_cast<std::uint16_t>(value.desiredY));
          appendUnsigned(payload, value.inputFlags);
          return true;
        } else {
          if (!validHeader(header, kStateSnapshotMessageId) ||
              !validSnapshot(value)) {
            return false;
          }
          payload.reserve(kSnapshotBasePayloadBytes +
                          (value.players.size() * kSnapshotPlayerBytes));
          appendUnsigned(payload, value.battleInstanceId);
          appendUnsigned(payload, value.snapshotSequence);
          appendUnsigned(payload, value.serverTick);
          appendUnsigned(payload,
                         static_cast<std::uint16_t>(value.players.size()));
          for (const auto &player : value.players) {
            appendUnsigned(payload, player.sessionId);
            appendUnsigned(payload,
                           std::bit_cast<std::uint32_t>(player.posXMillimeter));
            appendUnsigned(payload,
                           std::bit_cast<std::uint32_t>(player.posYMillimeter));
          }
          return true;
        }
      },
      message);
  return valid ? RudpHeaderCodec::encode(header, payload) : std::nullopt;
}

DecodedRudpMovement
RudpMovementCodec::decode(std::span<const std::byte> datagram) {
  const auto decoded = RudpHeaderCodec::decode(datagram);
  if (decoded.error != RudpHeaderError::None || !decoded.header.has_value()) {
    return {.error = RudpMovementCodecError::Header,
            .header = std::nullopt,
            .message = std::nullopt};
  }

  if (decoded.header->messageId == kMoveIntentMessageId) {
    if (!validHeader(*decoded.header, kMoveIntentMessageId) ||
        decoded.payload.size() != kMoveIntentPayloadBytes) {
      return malformed(*decoded.header);
    }
    const RudpMoveIntent move{
        .battleInstanceId = readUnsigned<std::uint64_t>(decoded.payload, 0),
        .actionSequence = readUnsigned<std::uint32_t>(decoded.payload, 8),
        .desiredX = std::bit_cast<std::int16_t>(
            readUnsigned<std::uint16_t>(decoded.payload, 12)),
        .desiredY = std::bit_cast<std::int16_t>(
            readUnsigned<std::uint16_t>(decoded.payload, 14)),
        .inputFlags = readUnsigned<std::uint16_t>(decoded.payload, 16),
    };
    return validMove(move)
               ? DecodedRudpMovement{.error = RudpMovementCodecError::None,
                                     .header = decoded.header,
                                     .message = move}
               : malformed(*decoded.header);
  }

  if (decoded.header->messageId != kStateSnapshotMessageId) {
    return {.error = RudpMovementCodecError::UnsupportedMessage,
            .header = decoded.header,
            .message = std::nullopt};
  }
  if (!validHeader(*decoded.header, kStateSnapshotMessageId) ||
      decoded.payload.size() < kSnapshotBasePayloadBytes) {
    return malformed(*decoded.header);
  }
  const auto playerCount = readUnsigned<std::uint16_t>(decoded.payload, 16);
  if (playerCount > kMaximumPlayers ||
      decoded.payload.size() !=
          kSnapshotBasePayloadBytes +
              (static_cast<std::size_t>(playerCount) * kSnapshotPlayerBytes)) {
    return malformed(*decoded.header);
  }
  RudpStateSnapshot snapshot{
      .battleInstanceId = readUnsigned<std::uint64_t>(decoded.payload, 0),
      .snapshotSequence = readUnsigned<std::uint32_t>(decoded.payload, 8),
      .serverTick = readUnsigned<std::uint32_t>(decoded.payload, 12),
      .players = {},
  };
  snapshot.players.reserve(playerCount);
  for (std::size_t index = 0; index < playerCount; ++index) {
    const auto offset =
        kSnapshotBasePayloadBytes + (index * kSnapshotPlayerBytes);
    snapshot.players.push_back(RudpSnapshotPlayer{
        .sessionId = readUnsigned<std::uint64_t>(decoded.payload, offset),
        .posXMillimeter = std::bit_cast<std::int32_t>(
            readUnsigned<std::uint32_t>(decoded.payload, offset + 8)),
        .posYMillimeter = std::bit_cast<std::int32_t>(
            readUnsigned<std::uint32_t>(decoded.payload, offset + 12)),
    });
  }
  return validSnapshot(snapshot)
             ? DecodedRudpMovement{.error = RudpMovementCodecError::None,
                                   .header = decoded.header,
                                   .message = std::move(snapshot)}
             : malformed(*decoded.header);
}

} // namespace lol::transport::rudp
