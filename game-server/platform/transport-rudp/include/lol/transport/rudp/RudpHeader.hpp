#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace lol::transport::rudp {

enum class RudpFlag : std::uint8_t {
  Unreliable = 0,
  Reliable = 1,
  AckOnly = 2,
  Heartbeat = 4,
};

struct RudpHeader final {
  RudpFlag flag;
  std::uint64_t sessionId;
  std::uint64_t sessionGeneration;
  std::uint32_t transportEpoch;
  std::uint32_t sequence;
  std::uint32_t ack;
  std::uint32_t ackBits;
  std::uint16_t messageId;

  bool operator==(const RudpHeader &) const = default;
};

enum class RudpHeaderError : std::uint8_t {
  None,
  DatagramTooShort,
  DatagramTooLarge,
  InvalidMagic,
  UnsupportedVersion,
  InvalidHeaderBytes,
  InvalidFlags,
  PayloadLengthMismatch,
  InvalidChecksum,
  InvalidHeaderField,
};

struct DecodedRudpDatagram final {
  RudpHeaderError error;
  std::optional<RudpHeader> header;
  std::span<const std::byte> payload;
};

class RudpHeaderCodec final {
public:
  [[nodiscard]] static std::optional<std::vector<std::byte>>
  encode(const RudpHeader &header, std::span<const std::byte> payload);
  [[nodiscard]] static DecodedRudpDatagram
  decode(std::span<const std::byte> datagram);
};

[[nodiscard]] bool isSequenceNewer(std::uint32_t candidate,
                                   std::uint32_t reference) noexcept;

} // namespace lol::transport::rudp
