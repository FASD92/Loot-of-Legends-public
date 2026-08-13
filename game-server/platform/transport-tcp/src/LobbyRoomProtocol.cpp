#include <lol/transport/tcp/LobbyRoomProtocol.hpp>

#include "ProtocolWire.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace lol::transport::tcp {
namespace {

using wire::Reader;
using wire::validUtf8;
using wire::Writer;

constexpr std::uint8_t kProtocolMajor = 1;
constexpr std::uint32_t kLobbyEntrySnapshotMessageId = 5;
constexpr std::uint32_t kLobbyRoomListUpdateMessageId = 6;
constexpr std::uint32_t kCreateRoomMessageId = 7;
constexpr std::uint32_t kJoinRoomMessageId = 8;
constexpr std::uint32_t kLeaveRoomMessageId = 9;
constexpr std::uint32_t kSetReadyMessageId = 10;
constexpr std::uint32_t kKickRoomMemberMessageId = 11;
constexpr std::uint32_t kRoomCommandResponseMessageId = 12;
constexpr std::uint32_t kRoomDetailProjectionMessageId = 13;
constexpr std::uint32_t kHostStartRequestMessageId = 14;
constexpr std::size_t kFrameHeaderBytes = 4;
constexpr std::size_t kEnvelopeBytes = 5;
constexpr std::size_t kMaximumTitleBytes = 48;
constexpr std::uint8_t kMinimumRoomCapacity = 2;
constexpr std::uint8_t kMaximumRoomCapacity = 10;
constexpr std::uint16_t kMaximumRoomResultCode = 12;

bool asciiWhitespace(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
         value == '\f' || value == '\v';
}

std::optional<std::string> normalizeTitle(std::string_view title) {
  std::size_t first = 0;
  while (first < title.size() && asciiWhitespace(title[first])) {
    ++first;
  }
  std::size_t last = title.size();
  while (last > first && asciiWhitespace(title[last - 1])) {
    --last;
  }
  std::string normalized{title.substr(first, last - first)};
  if (normalized.empty() || normalized.size() > kMaximumTitleBytes ||
      !validUtf8(normalized, true)) {
    return std::nullopt;
  }
  return normalized;
}

bool validNickname(std::string_view nickname) noexcept {
  return !nickname.empty() &&
         nickname.size() <= std::numeric_limits<std::uint16_t>::max() &&
         validUtf8(nickname);
}

bool writeSummary(Writer &payload, const RoomSummary &summary) {
  const auto title = normalizeTitle(summary.title);
  if (summary.roomId == 0 || !title.has_value() || summary.memberCount == 0 ||
      summary.memberCount > kMaximumRoomCapacity ||
      summary.capacity < kMinimumRoomCapacity ||
      summary.capacity > kMaximumRoomCapacity ||
      summary.memberCount > summary.capacity) {
    return false;
  }
  payload.uint64(summary.roomId);
  payload.uint8(static_cast<std::uint8_t>(title->size()));
  payload.text(*title);
  payload.uint8(summary.memberCount);
  payload.uint8(summary.capacity);
  return true;
}

bool writeMember(Writer &payload, const RoomMember &member) {
  if (member.sessionId == 0 || member.sessionGeneration == 0 ||
      !validNickname(member.nickname)) {
    return false;
  }
  payload.uint64(member.sessionId);
  payload.uint64(member.sessionGeneration);
  payload.uint16(static_cast<std::uint16_t>(member.nickname.size()));
  payload.text(member.nickname);
  payload.uint8(member.ready ? 1 : 0);
  return true;
}

bool writeRoomList(Writer &payload, const std::vector<RoomSummary> &rooms) {
  if (rooms.size() > std::numeric_limits<std::uint16_t>::max()) {
    return false;
  }
  payload.uint16(static_cast<std::uint16_t>(rooms.size()));
  return std::ranges::all_of(rooms, [&payload](const auto &room) {
    return writeSummary(payload, room);
  });
}

bool writeDetail(Writer &payload, const RoomDetailProjection &detail) {
  const auto title = normalizeTitle(detail.title);
  if (detail.roomId == 0 || !title.has_value() ||
      detail.capacity < kMinimumRoomCapacity ||
      detail.capacity > kMaximumRoomCapacity || detail.hostSessionId == 0 ||
      detail.hostSessionGeneration == 0 || detail.members.empty() ||
      detail.members.size() > detail.capacity ||
      detail.members.size() > kMaximumRoomCapacity) {
    return false;
  }
  const bool hostPresent =
      std::ranges::any_of(detail.members, [&detail](const auto &member) {
        return member.sessionId == detail.hostSessionId &&
               member.sessionGeneration == detail.hostSessionGeneration;
      });
  if (!hostPresent) {
    return false;
  }

  payload.uint64(detail.roomId);
  payload.uint8(static_cast<std::uint8_t>(title->size()));
  payload.text(*title);
  payload.uint8(detail.capacity);
  payload.uint64(detail.hostSessionId);
  payload.uint64(detail.hostSessionGeneration);
  payload.uint8(static_cast<std::uint8_t>(detail.members.size()));
  return std::ranges::all_of(detail.members, [&payload](const auto &member) {
    return writeMember(payload, member);
  });
}

std::optional<std::vector<std::byte>> finishFrame(Writer payload) {
  if (payload.bytes().size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  Writer frame;
  frame.uint32(static_cast<std::uint32_t>(payload.bytes().size()));
  for (const std::byte byte : payload.bytes()) {
    frame.uint8(std::to_integer<std::uint8_t>(byte));
  }
  return frame.take();
}

DecodedLobbyRoomFrame decodePayload(std::span<const std::byte> payload) {
  if (payload.size() < kEnvelopeBytes) {
    return {LobbyRoomCodecError::MalformedPayload, std::nullopt};
  }
  Reader reader{payload};
  const auto protocolMajor = reader.uint8();
  const auto messageId = reader.uint32();
  if (!protocolMajor.has_value() || !messageId.has_value()) {
    return {LobbyRoomCodecError::MalformedPayload, std::nullopt};
  }
  if (*protocolMajor != kProtocolMajor) {
    return {LobbyRoomCodecError::UnsupportedVersion, std::nullopt};
  }

  switch (*messageId) {
  case kCreateRoomMessageId: {
    const auto requestId = reader.uint64();
    const auto titleLength = reader.uint8();
    if (!requestId.has_value() || *requestId == 0 || !titleLength.has_value() ||
        *titleLength == 0 || *titleLength > kMaximumTitleBytes ||
        reader.remaining() != static_cast<std::size_t>(*titleLength) + 1) {
      return {LobbyRoomCodecError::MalformedPayload, std::nullopt};
    }
    auto title = reader.text(*titleLength);
    const auto capacity = reader.uint8();
    auto normalized = title.has_value() ? normalizeTitle(*title) : std::nullopt;
    if (!normalized.has_value() || !capacity.has_value() ||
        *capacity < kMinimumRoomCapacity || *capacity > kMaximumRoomCapacity ||
        reader.remaining() != 0) {
      return {LobbyRoomCodecError::MalformedPayload, std::nullopt};
    }
    return {LobbyRoomCodecError::None, CreateRoom{
                                           .requestId = *requestId,
                                           .title = std::move(*normalized),
                                           .capacity = *capacity,
                                       }};
  }
  case kJoinRoomMessageId: {
    const auto requestId = reader.uint64();
    const auto roomId = reader.uint64();
    if (!requestId.has_value() || *requestId == 0 || !roomId.has_value() ||
        *roomId == 0 || reader.remaining() != 0) {
      return {LobbyRoomCodecError::MalformedPayload, std::nullopt};
    }
    return {LobbyRoomCodecError::None,
            JoinRoom{.requestId = *requestId, .roomId = *roomId}};
  }
  case kLeaveRoomMessageId: {
    const auto requestId = reader.uint64();
    if (!requestId.has_value() || *requestId == 0 || reader.remaining() != 0) {
      return {LobbyRoomCodecError::MalformedPayload, std::nullopt};
    }
    return {LobbyRoomCodecError::None, LeaveRoom{.requestId = *requestId}};
  }
  case kSetReadyMessageId: {
    const auto requestId = reader.uint64();
    const auto ready = reader.uint8();
    if (!requestId.has_value() || *requestId == 0 || !ready.has_value() ||
        *ready > 1 || reader.remaining() != 0) {
      return {LobbyRoomCodecError::MalformedPayload, std::nullopt};
    }
    return {LobbyRoomCodecError::None,
            SetReady{.requestId = *requestId, .ready = *ready == 1}};
  }
  case kKickRoomMemberMessageId: {
    const auto requestId = reader.uint64();
    const auto targetSessionId = reader.uint64();
    const auto targetGeneration = reader.uint64();
    if (!requestId.has_value() || *requestId == 0 ||
        !targetSessionId.has_value() || *targetSessionId == 0 ||
        !targetGeneration.has_value() || *targetGeneration == 0 ||
        reader.remaining() != 0) {
      return {LobbyRoomCodecError::MalformedPayload, std::nullopt};
    }
    return {LobbyRoomCodecError::None,
            KickRoomMember{
                .requestId = *requestId,
                .targetSessionId = *targetSessionId,
                .targetSessionGeneration = *targetGeneration,
            }};
  }
  case kLobbyEntrySnapshotMessageId:
  case kLobbyRoomListUpdateMessageId:
  case kRoomCommandResponseMessageId:
  case kRoomDetailProjectionMessageId:
    return {LobbyRoomCodecError::WrongDirection, std::nullopt};
  case kHostStartRequestMessageId:
    return {LobbyRoomCodecError::ReservedMessage, std::nullopt};
  default:
    return {LobbyRoomCodecError::UnsupportedMessage, std::nullopt};
  }
}

} // namespace

std::optional<std::vector<std::byte>> LobbyRoomProtocolCodec::encodeClientFrame(
    const LobbyRoomClientMessage &message) {
  Writer payload;
  payload.uint8(kProtocolMajor);
  bool valid = true;
  std::visit(
      [&payload, &valid](const auto &request) {
        using Request = std::remove_cvref_t<decltype(request)>;
        valid = request.requestId != 0;
        if constexpr (std::is_same_v<Request, CreateRoom>) {
          const auto title = normalizeTitle(request.title);
          valid = valid && title.has_value() &&
                  request.capacity >= kMinimumRoomCapacity &&
                  request.capacity <= kMaximumRoomCapacity;
          if (!valid) {
            return;
          }
          payload.uint32(kCreateRoomMessageId);
          payload.uint64(request.requestId);
          payload.uint8(static_cast<std::uint8_t>(title->size()));
          payload.text(*title);
          payload.uint8(request.capacity);
        } else if constexpr (std::is_same_v<Request, JoinRoom>) {
          valid = valid && request.roomId != 0;
          if (!valid) {
            return;
          }
          payload.uint32(kJoinRoomMessageId);
          payload.uint64(request.requestId);
          payload.uint64(request.roomId);
        } else if constexpr (std::is_same_v<Request, LeaveRoom>) {
          if (!valid) {
            return;
          }
          payload.uint32(kLeaveRoomMessageId);
          payload.uint64(request.requestId);
        } else if constexpr (std::is_same_v<Request, SetReady>) {
          if (!valid) {
            return;
          }
          payload.uint32(kSetReadyMessageId);
          payload.uint64(request.requestId);
          payload.uint8(request.ready ? 1 : 0);
        } else {
          valid = valid && request.targetSessionId != 0 &&
                  request.targetSessionGeneration != 0;
          if (!valid) {
            return;
          }
          payload.uint32(kKickRoomMemberMessageId);
          payload.uint64(request.requestId);
          payload.uint64(request.targetSessionId);
          payload.uint64(request.targetSessionGeneration);
        }
      },
      message);
  return valid ? finishFrame(std::move(payload)) : std::nullopt;
}

std::optional<std::vector<std::byte>> LobbyRoomProtocolCodec::encodeServerFrame(
    const LobbyRoomServerMessage &message) {
  Writer payload;
  payload.uint8(kProtocolMajor);
  bool valid = true;
  std::visit(
      [&payload, &valid](const auto &serverMessage) {
        using Message = std::remove_cvref_t<decltype(serverMessage)>;
        if constexpr (std::is_same_v<Message, LobbyEntrySnapshot>) {
          valid = serverMessage.sessionId != 0 &&
                  serverMessage.sessionGeneration != 0 &&
                  validNickname(serverMessage.nickname);
          if (!valid) {
            return;
          }
          payload.uint32(kLobbyEntrySnapshotMessageId);
          payload.uint64(serverMessage.sessionId);
          payload.uint64(serverMessage.sessionGeneration);
          payload.uint16(
              static_cast<std::uint16_t>(serverMessage.nickname.size()));
          payload.text(serverMessage.nickname);
          valid = writeRoomList(payload, serverMessage.rooms);
        } else if constexpr (std::is_same_v<Message, LobbyRoomListUpdate>) {
          payload.uint32(kLobbyRoomListUpdateMessageId);
          valid = writeRoomList(payload, serverMessage.rooms);
        } else if constexpr (std::is_same_v<Message, RoomCommandResponse>) {
          valid = serverMessage.requestId != 0 &&
                  serverMessage.resultCode <= kMaximumRoomResultCode;
          if (!valid) {
            return;
          }
          payload.uint32(kRoomCommandResponseMessageId);
          payload.uint64(serverMessage.requestId);
          payload.uint16(serverMessage.resultCode);
        } else {
          payload.uint32(kRoomDetailProjectionMessageId);
          valid = writeDetail(payload, serverMessage);
        }
      },
      message);
  return valid ? finishFrame(std::move(payload)) : std::nullopt;
}

DecodedLobbyRoomFrame
LobbyRoomProtocolCodec::decodeClientFrame(std::span<const std::byte> frame) {
  if (frame.size() < kFrameHeaderBytes) {
    return {LobbyRoomCodecError::PartialFrame, std::nullopt};
  }
  Reader reader{frame};
  const auto payloadLength = reader.uint32();
  if (!payloadLength.has_value() || *payloadLength > reader.remaining()) {
    return {LobbyRoomCodecError::PartialFrame, std::nullopt};
  }
  if (*payloadLength != reader.remaining()) {
    return {LobbyRoomCodecError::FrameLengthMismatch, std::nullopt};
  }
  return decodePayload(frame.subspan(kFrameHeaderBytes));
}

} // namespace lol::transport::tcp
