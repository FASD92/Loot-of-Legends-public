#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace lol::transport::tcp {

struct RoomSummary final {
  std::uint64_t roomId;
  std::string title;
  std::uint8_t memberCount;
  std::uint8_t capacity;

  bool operator==(const RoomSummary &) const = default;
};

struct RoomMember final {
  std::uint64_t sessionId;
  std::uint64_t sessionGeneration;
  std::string nickname;
  bool ready;

  bool operator==(const RoomMember &) const = default;
};

struct LobbyEntrySnapshot final {
  std::uint64_t sessionId;
  std::uint64_t sessionGeneration;
  std::string nickname;
  std::vector<RoomSummary> rooms;

  bool operator==(const LobbyEntrySnapshot &) const = default;
};

struct LobbyRoomListUpdate final {
  std::vector<RoomSummary> rooms;

  bool operator==(const LobbyRoomListUpdate &) const = default;
};

struct CreateRoom final {
  std::uint64_t requestId;
  std::string title;
  std::uint8_t capacity;

  bool operator==(const CreateRoom &) const = default;
};

struct JoinRoom final {
  std::uint64_t requestId;
  std::uint64_t roomId;

  bool operator==(const JoinRoom &) const = default;
};

struct LeaveRoom final {
  std::uint64_t requestId;

  bool operator==(const LeaveRoom &) const = default;
};

struct SetReady final {
  std::uint64_t requestId;
  bool ready;

  bool operator==(const SetReady &) const = default;
};

struct KickRoomMember final {
  std::uint64_t requestId;
  std::uint64_t targetSessionId;
  std::uint64_t targetSessionGeneration;

  bool operator==(const KickRoomMember &) const = default;
};

struct RoomCommandResponse final {
  std::uint64_t requestId;
  std::uint16_t resultCode;

  bool operator==(const RoomCommandResponse &) const = default;
};

struct RoomDetailProjection final {
  std::uint64_t roomId;
  std::string title;
  std::uint8_t capacity;
  std::uint64_t hostSessionId;
  std::uint64_t hostSessionGeneration;
  std::vector<RoomMember> members;

  bool operator==(const RoomDetailProjection &) const = default;
};

using LobbyRoomClientMessage =
    std::variant<CreateRoom, JoinRoom, LeaveRoom, SetReady, KickRoomMember>;
using LobbyRoomServerMessage =
    std::variant<LobbyEntrySnapshot, LobbyRoomListUpdate, RoomCommandResponse,
                 RoomDetailProjection>;

enum class LobbyRoomCodecError : std::uint8_t {
  None,
  PartialFrame,
  FrameLengthMismatch,
  UnsupportedVersion,
  UnsupportedMessage,
  WrongDirection,
  ReservedMessage,
  MalformedPayload,
};

struct DecodedLobbyRoomFrame final {
  LobbyRoomCodecError error;
  std::optional<LobbyRoomClientMessage> message;
};

class LobbyRoomProtocolCodec final {
public:
  [[nodiscard]] static std::optional<std::vector<std::byte>>
  encodeClientFrame(const LobbyRoomClientMessage &message);
  [[nodiscard]] static std::optional<std::vector<std::byte>>
  encodeServerFrame(const LobbyRoomServerMessage &message);
  [[nodiscard]] static DecodedLobbyRoomFrame
  decodeClientFrame(std::span<const std::byte> frame);
};

} // namespace lol::transport::tcp
