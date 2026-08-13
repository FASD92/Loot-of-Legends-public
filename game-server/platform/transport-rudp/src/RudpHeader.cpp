#include <lol/transport/rudp/RudpHeader.hpp>

#include <limits>
#include <type_traits>

namespace lol::transport::rudp {
namespace {

constexpr std::uint32_t kMagic = 0x4C4F4C32U;
constexpr std::uint8_t kProtocolMajor = 1;
constexpr std::uint16_t kHeaderBytes = 48;
constexpr std::size_t kMaximumDatagramBytes = 1200;
constexpr std::size_t kCrcOffset = 44;
constexpr std::uint16_t kHeartbeatMessageId = 24;
constexpr std::uint32_t kCrcPolynomial = 0xEDB88320U;

template <typename Integer>
Integer readBigEndian(std::span<const std::byte> bytes, std::size_t offset) {
  static_assert(std::is_unsigned_v<Integer>);
  Integer value = 0;
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    value = static_cast<Integer>(
        (value << 8U) | static_cast<Integer>(std::to_integer<std::uint8_t>(
                            bytes[offset + index])));
  }
  return value;
}

template <typename Integer>
void writeBigEndian(std::vector<std::byte> &bytes, std::size_t offset,
                    Integer value) {
  static_assert(std::is_unsigned_v<Integer>);
  for (std::size_t index = 0; index < sizeof(Integer); ++index) {
    const std::size_t shift = (sizeof(Integer) - index - 1U) * 8U;
    bytes[offset + index] =
        static_cast<std::byte>(static_cast<std::uint8_t>(value >> shift));
  }
}

std::uint32_t checksum(std::span<const std::byte> datagram) {
  std::uint32_t value = std::numeric_limits<std::uint32_t>::max();
  for (std::size_t index = 0; index < datagram.size(); ++index) {
    const std::uint8_t byte =
        index >= kCrcOffset && index < kCrcOffset + sizeof(std::uint32_t)
            ? 0
            : std::to_integer<std::uint8_t>(datagram[index]);
    value ^= byte;
    for (std::uint8_t bit = 0; bit < 8; ++bit) {
      value = (value >> 1U) ^ ((value & 1U) != 0U ? kCrcPolynomial : 0U);
    }
  }
  return value ^ std::numeric_limits<std::uint32_t>::max();
}

std::optional<RudpFlag> parseFlag(std::uint8_t raw) {
  switch (raw) {
  case 0:
    return RudpFlag::Unreliable;
  case 1:
    return RudpFlag::Reliable;
  case 2:
    return RudpFlag::AckOnly;
  case 4:
    return RudpFlag::Heartbeat;
  default:
    return std::nullopt;
  }
}

bool validFields(const RudpHeader &header, std::size_t payloadBytes) {
  if (header.sessionId == 0 || header.sessionGeneration == 0 ||
      header.sequence == 0) {
    return false;
  }
  switch (header.flag) {
  case RudpFlag::Unreliable:
  case RudpFlag::Reliable:
    return header.messageId != 0;
  case RudpFlag::AckOnly:
    return header.messageId == 0 && payloadBytes == 0;
  case RudpFlag::Heartbeat:
    return header.messageId == kHeartbeatMessageId && payloadBytes == 0;
  }
  return false;
}

DecodedRudpDatagram failure(RudpHeaderError error) {
  return {.error = error, .header = std::nullopt, .payload = {}};
}

} // namespace

std::optional<std::vector<std::byte>>
RudpHeaderCodec::encode(const RudpHeader &header,
                        std::span<const std::byte> payload) {
  if (payload.size() > kMaximumDatagramBytes - kHeaderBytes ||
      !validFields(header, payload.size())) {
    return std::nullopt;
  }

  std::vector<std::byte> datagram(kHeaderBytes, std::byte{0});
  datagram.insert(datagram.end(), payload.begin(), payload.end());
  writeBigEndian(datagram, 0, kMagic);
  datagram[4] = static_cast<std::byte>(kProtocolMajor);
  datagram[5] = static_cast<std::byte>(header.flag);
  writeBigEndian(datagram, 6, kHeaderBytes);
  writeBigEndian(datagram, 8, header.sessionId);
  writeBigEndian(datagram, 16, header.sessionGeneration);
  writeBigEndian(datagram, 24, header.transportEpoch);
  writeBigEndian(datagram, 28, header.sequence);
  writeBigEndian(datagram, 32, header.ack);
  writeBigEndian(datagram, 36, header.ackBits);
  writeBigEndian(datagram, 40, header.messageId);
  writeBigEndian(datagram, 42, static_cast<std::uint16_t>(payload.size()));
  writeBigEndian(datagram, kCrcOffset, checksum(datagram));
  return datagram;
}

DecodedRudpDatagram
RudpHeaderCodec::decode(std::span<const std::byte> datagram) {
  if (datagram.size() < kHeaderBytes) {
    return failure(RudpHeaderError::DatagramTooShort);
  }
  if (datagram.size() > kMaximumDatagramBytes) {
    return failure(RudpHeaderError::DatagramTooLarge);
  }
  if (readBigEndian<std::uint32_t>(datagram, 0) != kMagic) {
    return failure(RudpHeaderError::InvalidMagic);
  }
  if (std::to_integer<std::uint8_t>(datagram[4]) != kProtocolMajor) {
    return failure(RudpHeaderError::UnsupportedVersion);
  }
  if (readBigEndian<std::uint16_t>(datagram, 6) != kHeaderBytes) {
    return failure(RudpHeaderError::InvalidHeaderBytes);
  }
  const auto flag = parseFlag(std::to_integer<std::uint8_t>(datagram[5]));
  if (!flag.has_value()) {
    return failure(RudpHeaderError::InvalidFlags);
  }
  const auto payloadBytes = readBigEndian<std::uint16_t>(datagram, 42);
  if (payloadBytes != datagram.size() - kHeaderBytes) {
    return failure(RudpHeaderError::PayloadLengthMismatch);
  }
  if (readBigEndian<std::uint32_t>(datagram, kCrcOffset) !=
      checksum(datagram)) {
    return failure(RudpHeaderError::InvalidChecksum);
  }

  const RudpHeader header{
      .flag = *flag,
      .sessionId = readBigEndian<std::uint64_t>(datagram, 8),
      .sessionGeneration = readBigEndian<std::uint64_t>(datagram, 16),
      .transportEpoch = readBigEndian<std::uint32_t>(datagram, 24),
      .sequence = readBigEndian<std::uint32_t>(datagram, 28),
      .ack = readBigEndian<std::uint32_t>(datagram, 32),
      .ackBits = readBigEndian<std::uint32_t>(datagram, 36),
      .messageId = readBigEndian<std::uint16_t>(datagram, 40),
  };
  if (!validFields(header, payloadBytes)) {
    return failure(RudpHeaderError::InvalidHeaderField);
  }
  return {.error = RudpHeaderError::None,
          .header = header,
          .payload = datagram.subspan(kHeaderBytes)};
}

bool isSequenceNewer(std::uint32_t candidate,
                     std::uint32_t reference) noexcept {
  return candidate != reference &&
         static_cast<std::uint32_t>(candidate - reference) < 0x80000000U;
}

} // namespace lol::transport::rudp
