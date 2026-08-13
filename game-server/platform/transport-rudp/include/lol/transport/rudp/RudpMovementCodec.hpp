#pragma once

#include <lol/transport/rudp/RudpHeader.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace lol::transport::rudp {

struct RudpMoveIntent final {
  std::uint64_t battleInstanceId;
  std::uint32_t actionSequence;
  std::int16_t desiredX;
  std::int16_t desiredY;
  std::uint16_t inputFlags;

  bool operator==(const RudpMoveIntent &) const = default;
};

struct RudpSnapshotPlayer final {
  std::uint64_t sessionId;
  std::int32_t posXMillimeter;
  std::int32_t posYMillimeter;

  bool operator==(const RudpSnapshotPlayer &) const = default;
};

struct RudpStateSnapshot final {
  std::uint64_t battleInstanceId;
  std::uint32_t snapshotSequence;
  std::uint32_t serverTick;
  std::vector<RudpSnapshotPlayer> players;

  bool operator==(const RudpStateSnapshot &) const = default;
};

using RudpMovementMessage = std::variant<RudpMoveIntent, RudpStateSnapshot>;

enum class RudpMovementCodecError : std::uint8_t {
  None,
  Header,
  UnsupportedMessage,
  MalformedPayload,
};

struct DecodedRudpMovement final {
  RudpMovementCodecError error{RudpMovementCodecError::Header};
  std::optional<RudpHeader> header;
  std::optional<RudpMovementMessage> message;
};

class RudpMovementCodec final {
public:
  [[nodiscard]] static std::optional<std::vector<std::byte>>
  encode(const RudpHeader &header, const RudpMovementMessage &message);
  [[nodiscard]] static DecodedRudpMovement
  decode(std::span<const std::byte> datagram);
};

} // namespace lol::transport::rudp
