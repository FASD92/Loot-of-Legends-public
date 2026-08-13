#pragma once

#include <lol/transport/rudp/RudpHeader.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace lol::transport::rudp {

struct RudpBindCapability final {
  using Bytes = std::array<std::byte, 32>;

  Bytes bytes;
  bool operator==(const RudpBindCapability &) const = default;
};

struct RudpBindHello final {
  RudpBindCapability capability;
  bool operator==(const RudpBindHello &) const = default;
};

struct RudpBindAccepted final {
  bool operator==(const RudpBindAccepted &) const = default;
};

struct RudpHeartbeat final {
  bool operator==(const RudpHeartbeat &) const = default;
};

using RudpControlMessage =
    std::variant<RudpBindHello, RudpBindAccepted, RudpHeartbeat>;

enum class RudpControlCodecError : std::uint8_t {
  None,
  Header,
  UnsupportedMessage,
  MalformedPayload,
};

struct DecodedRudpControl final {
  RudpControlCodecError error;
  std::optional<RudpHeader> header;
  std::optional<RudpControlMessage> message;
};

class RudpControlCodec final {
public:
  [[nodiscard]] static std::optional<std::vector<std::byte>>
  encode(const RudpHeader &header, const RudpControlMessage &message);
  [[nodiscard]] static DecodedRudpControl
  decode(std::span<const std::byte> datagram);
};

} // namespace lol::transport::rudp
