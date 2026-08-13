#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace lol::transport::tcp {

struct HostStartRequest final {
  std::uint64_t requestId;

  bool operator==(const HostStartRequest &) const = default;
};

struct ArenaLoadComplete final {
  std::uint64_t requestId;
  std::uint64_t roomId;
  std::uint64_t battleInstanceId;

  bool operator==(const ArenaLoadComplete &) const = default;
};

struct BattleCommandResponse final {
  std::uint64_t requestId;
  std::uint16_t resultCode;

  bool operator==(const BattleCommandResponse &) const = default;
};

struct ArenaLoadEntry final {
  std::uint64_t roomId;
  std::uint64_t battleInstanceId;

  bool operator==(const ArenaLoadEntry &) const = default;
};

struct BattleParticipant final {
  std::uint64_t sessionId;
  std::uint64_t sessionGeneration;
  std::string nickname;

  bool operator==(const BattleParticipant &) const = default;
};

struct ArenaGameplayStart final {
  std::uint64_t roomId;
  std::uint64_t battleInstanceId;
  std::vector<BattleParticipant> participants;

  bool operator==(const ArenaGameplayStart &) const = default;
};

struct ArenaLoadCancelled final {
  std::uint64_t roomId;
  std::uint64_t battleInstanceId;
  std::uint16_t reasonCode;

  bool operator==(const ArenaLoadCancelled &) const = default;
};

using BattleLoadClientMessage =
    std::variant<HostStartRequest, ArenaLoadComplete>;
using BattleLoadServerMessage =
    std::variant<BattleCommandResponse, ArenaLoadEntry, ArenaGameplayStart,
                 ArenaLoadCancelled>;

enum class BattleLoadCodecError : std::uint8_t {
  None,
  PartialFrame,
  FrameLengthMismatch,
  UnsupportedVersion,
  UnsupportedMessage,
  WrongDirection,
  MalformedPayload,
};

struct DecodedBattleLoadFrame final {
  BattleLoadCodecError error;
  std::optional<BattleLoadClientMessage> message;
};

class BattleLoadProtocolCodec final {
public:
  [[nodiscard]] static std::optional<std::vector<std::byte>>
  encodeClientFrame(const BattleLoadClientMessage &message);
  [[nodiscard]] static std::optional<std::vector<std::byte>>
  encodeServerFrame(const BattleLoadServerMessage &message);
  [[nodiscard]] static DecodedBattleLoadFrame
  decodeClientFrame(std::span<const std::byte> frame);
};

} // namespace lol::transport::tcp
