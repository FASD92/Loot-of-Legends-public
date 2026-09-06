#pragma once

#include <lol/transport/tcp/FinalResultProtocol.hpp>
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
  ResumeUnavailable = 7,
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

struct ResumeBattleSession final {
  std::uint64_t requestId;
  std::uint64_t previousSessionId;
  std::uint64_t previousSessionGeneration;
  std::string credential;
  bool operator==(const ResumeBattleSession &) const = default;
};

enum class BattleResumePhase : std::uint8_t {
  Combat = 1,
  Loot = 2,
  Result = 3,
};

struct BattleResumePlayer final {
  std::uint64_t sessionId;
  std::int32_t positionXMillimeters;
  std::int32_t positionYMillimeters;
  bool healthKnown;
  std::uint32_t hitPoints;
  std::uint32_t maximumHitPoints;
  bool alive;
  bool operator==(const BattleResumePlayer &) const = default;
};

struct BattleResumeMonster final {
  std::uint64_t monsterId;
  std::int32_t positionXMillimeters;
  std::int32_t positionYMillimeters;
  std::uint32_t hitPoints;
  std::uint32_t maximumHitPoints;
  std::uint8_t state;
  bool operator==(const BattleResumeMonster &) const = default;
};

struct BattleResumeDrop final {
  std::uint64_t dropId;
  std::uint64_t itemId;
  std::uint64_t quantity;
  std::int32_t positionXMillimeters;
  std::int32_t positionYMillimeters;
  std::uint8_t state;
  std::uint64_t ownerSessionId;
  bool operator==(const BattleResumeDrop &) const = default;
};

struct BattleResumeSnapshot final {
  std::uint64_t requestId;
  std::uint64_t snapshotId;
  std::uint64_t roomId;
  std::uint64_t battleInstanceId;
  std::uint64_t playerSessionId;
  std::uint64_t sessionGeneration;
  BattleResumePhase phase;
  std::uint32_t remainingMillis;
  std::uint32_t serverTick;
  std::vector<BattleResumePlayer> players;
  std::optional<BattleResumeMonster> monster;
  std::vector<BattleResumeDrop> drops;
  std::uint64_t score;
  std::optional<FinalResult> result;
  bool operator==(const BattleResumeSnapshot &) const = default;
};

struct BattleResumeSnapshotApplied final {
  std::uint64_t snapshotId;
  bool operator==(const BattleResumeSnapshotApplied &) const = default;
};

using SessionControlMessage =
    std::variant<AuthenticateGameSession, Welcome, AuthenticationRejected,
                 SessionReplaced, RequestRudpBindCapability, RudpBindCapability,
                 ResumeBattleSession, BattleResumeSnapshot,
                 BattleResumeSnapshotApplied>;

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
