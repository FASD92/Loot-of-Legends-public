#pragma once

#include <lol/transport/tcp/TcpConnection.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace lol::transport::tcp {

inline constexpr std::uint8_t kSessionProtocolMajor = 1;

struct AuthenticateGameSession final {
  std::uint64_t requestId;
  std::string credential;
  bool operator==(const AuthenticateGameSession &) const = default;
};

struct Welcome final {
  std::uint64_t requestId;
  std::uint64_t sessionId;
  std::uint64_t sessionGeneration;
  std::uint64_t serverTimeUnixMillis;
  std::string nickname;
  bool operator==(const Welcome &) const = default;
};

enum class AuthenticationRejectedReason : std::uint16_t {
  Invalid = 1,
  Expired = 2,
  AlreadyConsumed = 3,
  WrongAudience = 4,
  DependencyUnavailable = 5,
  PreAuthCommand = 6,
};

struct AuthenticationRejected final {
  std::uint64_t requestId;
  AuthenticationRejectedReason reason;
  bool operator==(const AuthenticationRejected &) const = default;
};

enum class SessionReplacedReason : std::uint16_t { SameAccountLogin = 1 };

struct SessionReplaced final {
  SessionReplacedReason reason;
  bool operator==(const SessionReplaced &) const = default;
};

struct RequestRudpBindCapability final {
  std::uint64_t requestId;
  bool operator==(const RequestRudpBindCapability &) const = default;
};

struct RudpBindCapability final {
  std::uint64_t requestId;
  std::uint32_t ttlMillis;
  std::array<std::byte, 32> capability;
  bool operator==(const RudpBindCapability &) const = default;
};

using SessionControlMessage =
    std::variant<AuthenticateGameSession, Welcome, AuthenticationRejected,
                 SessionReplaced, RequestRudpBindCapability,
                 RudpBindCapability>;

enum class CodecError {
  None,
  PartialFrame,
  FrameLengthMismatch,
  UnsupportedVersion,
  UnsupportedMessage,
  MalformedPayload,
};

struct DecodedSessionFrame final {
  CodecError error;
  std::optional<SessionControlMessage> message;
};

class SessionProtocolCodec final {
public:
  [[nodiscard]] static std::optional<std::vector<std::byte>>
  encodeFrame(const SessionControlMessage &message);
  [[nodiscard]] static DecodedSessionFrame
  decodeFrame(std::span<const std::byte> frame);
  [[nodiscard]] static DecodedPreAuthFrame
  decodePreAuthPayload(std::span<const std::byte> payload);
};

} // namespace lol::transport::tcp
