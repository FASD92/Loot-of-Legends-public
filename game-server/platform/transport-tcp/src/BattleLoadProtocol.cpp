#include <lol/transport/tcp/BattleLoadProtocol.hpp>

#include "ProtocolWire.hpp"

#include <algorithm>
#include <limits>
#include <type_traits>
#include <utility>

namespace lol::transport::tcp {
namespace {

using wire::Reader;
using wire::validUtf8;
using wire::Writer;

constexpr std::uint8_t kProtocolMajor = 1;
constexpr std::uint32_t kHostStartRequestMessageId = 14;
constexpr std::uint32_t kBattleCommandResponseMessageId = 15;
constexpr std::uint32_t kArenaLoadEntryMessageId = 16;
constexpr std::uint32_t kArenaLoadCompleteMessageId = 17;
constexpr std::uint32_t kArenaGameplayStartMessageId = 18;
constexpr std::uint32_t kArenaLoadCancelledMessageId = 19;
constexpr std::size_t kFrameHeaderBytes = 4;
constexpr std::size_t kEnvelopeBytes = 5;
constexpr std::size_t kMinimumParticipants = 2;
constexpr std::size_t kMaximumParticipants = 10;
constexpr std::uint16_t kMaximumResultCode = 12;
constexpr std::uint16_t kNotEnoughReadyReason = 1;

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

bool validParticipant(const BattleParticipant &participant) {
  return participant.sessionId != 0 && participant.sessionGeneration != 0 &&
         !participant.nickname.empty() &&
         participant.nickname.size() <=
             std::numeric_limits<std::uint16_t>::max() &&
         validUtf8(participant.nickname);
}

bool writeParticipants(Writer &payload,
                       const std::vector<BattleParticipant> &participants) {
  if (participants.size() < kMinimumParticipants ||
      participants.size() > kMaximumParticipants) {
    return false;
  }
  for (std::size_t index = 0; index < participants.size(); ++index) {
    const auto &participant = participants[index];
    if (!validParticipant(participant) ||
        std::any_of(participants.begin() +
                        static_cast<std::ptrdiff_t>(index + 1),
                    participants.end(), [&](const auto &other) {
                      return other.sessionId == participant.sessionId;
                    })) {
      return false;
    }
    payload.uint64(participant.sessionId);
    payload.uint64(participant.sessionGeneration);
    payload.uint16(static_cast<std::uint16_t>(participant.nickname.size()));
    payload.text(participant.nickname);
  }
  return true;
}

DecodedBattleLoadFrame decodePayload(std::span<const std::byte> payload) {
  if (payload.size() < kEnvelopeBytes) {
    return {BattleLoadCodecError::MalformedPayload, std::nullopt};
  }
  Reader reader{payload};
  const auto protocolMajor = reader.uint8();
  const auto messageId = reader.uint32();
  if (!protocolMajor.has_value() || !messageId.has_value()) {
    return {BattleLoadCodecError::MalformedPayload, std::nullopt};
  }
  if (*protocolMajor != kProtocolMajor) {
    return {BattleLoadCodecError::UnsupportedVersion, std::nullopt};
  }

  switch (*messageId) {
  case kHostStartRequestMessageId: {
    const auto requestId = reader.uint64();
    if (!requestId.has_value() || *requestId == 0 || reader.remaining() != 0) {
      return {BattleLoadCodecError::MalformedPayload, std::nullopt};
    }
    return {BattleLoadCodecError::None,
            HostStartRequest{.requestId = *requestId}};
  }
  case kArenaLoadCompleteMessageId: {
    const auto requestId = reader.uint64();
    const auto roomId = reader.uint64();
    const auto battleInstanceId = reader.uint64();
    if (!requestId.has_value() || *requestId == 0 || !roomId.has_value() ||
        *roomId == 0 || !battleInstanceId.has_value() ||
        *battleInstanceId == 0 || reader.remaining() != 0) {
      return {BattleLoadCodecError::MalformedPayload, std::nullopt};
    }
    return {BattleLoadCodecError::None,
            ArenaLoadComplete{
                .requestId = *requestId,
                .roomId = *roomId,
                .battleInstanceId = *battleInstanceId,
            }};
  }
  case kBattleCommandResponseMessageId:
  case kArenaLoadEntryMessageId:
  case kArenaGameplayStartMessageId:
  case kArenaLoadCancelledMessageId:
    return {BattleLoadCodecError::WrongDirection, std::nullopt};
  default:
    return {BattleLoadCodecError::UnsupportedMessage, std::nullopt};
  }
}

} // namespace

std::optional<std::vector<std::byte>>
BattleLoadProtocolCodec::encodeClientFrame(
    const BattleLoadClientMessage &message) {
  Writer payload;
  payload.uint8(kProtocolMajor);
  bool valid = true;
  std::visit(
      [&payload, &valid](const auto &request) {
        valid = request.requestId != 0;
        if constexpr (std::is_same_v<std::remove_cvref_t<decltype(request)>,
                                     HostStartRequest>) {
          if (!valid) {
            return;
          }
          payload.uint32(kHostStartRequestMessageId);
          payload.uint64(request.requestId);
        } else {
          valid = valid && request.roomId != 0 && request.battleInstanceId != 0;
          if (!valid) {
            return;
          }
          payload.uint32(kArenaLoadCompleteMessageId);
          payload.uint64(request.requestId);
          payload.uint64(request.roomId);
          payload.uint64(request.battleInstanceId);
        }
      },
      message);
  return valid ? finishFrame(std::move(payload)) : std::nullopt;
}

std::optional<std::vector<std::byte>>
BattleLoadProtocolCodec::encodeServerFrame(
    const BattleLoadServerMessage &message) {
  Writer payload;
  payload.uint8(kProtocolMajor);
  bool valid = true;
  std::visit(
      [&payload, &valid](const auto &serverMessage) {
        using Message = std::remove_cvref_t<decltype(serverMessage)>;
        if constexpr (std::is_same_v<Message, BattleCommandResponse>) {
          valid = serverMessage.requestId != 0 &&
                  serverMessage.resultCode <= kMaximumResultCode;
          if (!valid) {
            return;
          }
          payload.uint32(kBattleCommandResponseMessageId);
          payload.uint64(serverMessage.requestId);
          payload.uint16(serverMessage.resultCode);
        } else if constexpr (std::is_same_v<Message, ArenaLoadEntry>) {
          valid =
              serverMessage.roomId != 0 && serverMessage.battleInstanceId != 0;
          if (!valid) {
            return;
          }
          payload.uint32(kArenaLoadEntryMessageId);
          payload.uint64(serverMessage.roomId);
          payload.uint64(serverMessage.battleInstanceId);
        } else if constexpr (std::is_same_v<Message, ArenaGameplayStart>) {
          valid =
              serverMessage.roomId != 0 && serverMessage.battleInstanceId != 0;
          if (!valid) {
            return;
          }
          payload.uint32(kArenaGameplayStartMessageId);
          payload.uint64(serverMessage.roomId);
          payload.uint64(serverMessage.battleInstanceId);
          payload.uint8(
              static_cast<std::uint8_t>(serverMessage.participants.size()));
          valid = writeParticipants(payload, serverMessage.participants);
        } else {
          valid = serverMessage.roomId != 0 &&
                  serverMessage.battleInstanceId != 0 &&
                  serverMessage.reasonCode == kNotEnoughReadyReason;
          if (!valid) {
            return;
          }
          payload.uint32(kArenaLoadCancelledMessageId);
          payload.uint64(serverMessage.roomId);
          payload.uint64(serverMessage.battleInstanceId);
          payload.uint16(serverMessage.reasonCode);
        }
      },
      message);
  return valid ? finishFrame(std::move(payload)) : std::nullopt;
}

DecodedBattleLoadFrame
BattleLoadProtocolCodec::decodeClientFrame(std::span<const std::byte> frame) {
  if (frame.size() < kFrameHeaderBytes) {
    return {BattleLoadCodecError::PartialFrame, std::nullopt};
  }
  Reader reader{frame};
  const auto payloadLength = reader.uint32();
  if (!payloadLength.has_value() || *payloadLength > reader.remaining()) {
    return {BattleLoadCodecError::PartialFrame, std::nullopt};
  }
  if (*payloadLength != reader.remaining()) {
    return {BattleLoadCodecError::FrameLengthMismatch, std::nullopt};
  }
  return decodePayload(frame.subspan(kFrameHeaderBytes));
}

} // namespace lol::transport::tcp
