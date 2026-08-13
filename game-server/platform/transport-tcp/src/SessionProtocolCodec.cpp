#include <lol/transport/tcp/SessionProtocolCodec.hpp>

#include "ProtocolWire.hpp"

#include <algorithm>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace lol::transport::tcp {
namespace {

constexpr std::uint32_t kAuthenticateMessageId = 1;
constexpr std::uint32_t kWelcomeMessageId = 2;
constexpr std::uint32_t kAuthenticationRejectedMessageId = 3;
constexpr std::uint32_t kSessionReplacedMessageId = 4;
constexpr std::uint32_t kRequestRudpBindCapabilityMessageId = 20;
constexpr std::uint32_t kRudpBindCapabilityMessageId = 21;
constexpr std::size_t kFrameHeaderBytes = 4;
constexpr std::size_t kEnvelopeBytes = 5;
constexpr std::size_t kCredentialBytes = 43;
constexpr std::uint32_t kRudpCapabilityTtlMillis = 15000;

using wire::Reader;
using wire::validUtf8;
using wire::Writer;

bool validCredential(std::string_view credential) {
  if (credential.size() != kCredentialBytes) {
    return false;
  }
  for (const char character : credential) {
    if (!((character >= 'A' && character <= 'Z') ||
          (character >= 'a' && character <= 'z') ||
          (character >= '0' && character <= '9') || character == '-' ||
          character == '_')) {
      return false;
    }
  }
  return true;
}

bool validReason(AuthenticationRejectedReason reason) noexcept {
  switch (reason) {
  case AuthenticationRejectedReason::Invalid:
  case AuthenticationRejectedReason::Expired:
  case AuthenticationRejectedReason::AlreadyConsumed:
  case AuthenticationRejectedReason::WrongAudience:
  case AuthenticationRejectedReason::DependencyUnavailable:
  case AuthenticationRejectedReason::PreAuthCommand:
    return true;
  }
  return false;
}

bool validReason(SessionReplacedReason reason) noexcept {
  return reason == SessionReplacedReason::SameAccountLogin;
}

bool validCapability(const std::array<std::byte, 32> &capability) {
  return std::ranges::any_of(
      capability, [](std::byte value) { return value != std::byte{0}; });
}

DecodedSessionFrame decodePayload(std::span<const std::byte> payload) {
  if (payload.size() < kEnvelopeBytes) {
    return {CodecError::MalformedPayload, std::nullopt};
  }
  Reader reader{payload};
  const auto protocolMajor = reader.uint8();
  const auto messageId = reader.uint32();
  if (!protocolMajor.has_value() || !messageId.has_value()) {
    return {CodecError::MalformedPayload, std::nullopt};
  }
  if (*protocolMajor != kSessionProtocolMajor) {
    return {CodecError::UnsupportedVersion, std::nullopt};
  }

  switch (*messageId) {
  case kAuthenticateMessageId: {
    const auto requestId = reader.uint64();
    const auto credentialLength = reader.uint16();
    if (!requestId.has_value() || *requestId == 0 ||
        !credentialLength.has_value() ||
        *credentialLength != kCredentialBytes ||
        reader.remaining() != *credentialLength) {
      return {CodecError::MalformedPayload, std::nullopt};
    }
    auto credential = reader.text(*credentialLength);
    if (!credential.has_value() || !validCredential(*credential)) {
      return {CodecError::MalformedPayload, std::nullopt};
    }
    return {CodecError::None,
            AuthenticateGameSession{*requestId, std::move(*credential)}};
  }
  case kWelcomeMessageId: {
    const auto requestId = reader.uint64();
    const auto sessionId = reader.uint64();
    const auto generation = reader.uint64();
    const auto serverTime = reader.uint64();
    const auto nicknameLength = reader.uint16();
    if (!requestId.has_value() || *requestId == 0 || !sessionId.has_value() ||
        *sessionId == 0 || !generation.has_value() || *generation == 0 ||
        !serverTime.has_value() || !nicknameLength.has_value() ||
        *nicknameLength == 0 || reader.remaining() != *nicknameLength) {
      return {CodecError::MalformedPayload, std::nullopt};
    }
    auto nickname = reader.text(*nicknameLength);
    if (!nickname.has_value() || !validUtf8(*nickname)) {
      return {CodecError::MalformedPayload, std::nullopt};
    }
    return {CodecError::None, Welcome{*requestId, *sessionId, *generation,
                                      *serverTime, std::move(*nickname)}};
  }
  case kAuthenticationRejectedMessageId: {
    const auto requestId = reader.uint64();
    const auto reasonValue = reader.uint16();
    if (!requestId.has_value() || *requestId == 0 || !reasonValue.has_value() ||
        reader.remaining() != 0) {
      return {CodecError::MalformedPayload, std::nullopt};
    }
    const auto reason = static_cast<AuthenticationRejectedReason>(*reasonValue);
    if (!validReason(reason)) {
      return {CodecError::MalformedPayload, std::nullopt};
    }
    return {CodecError::None, AuthenticationRejected{*requestId, reason}};
  }
  case kSessionReplacedMessageId: {
    const auto reasonValue = reader.uint16();
    if (!reasonValue.has_value() || reader.remaining() != 0) {
      return {CodecError::MalformedPayload, std::nullopt};
    }
    const auto reason = static_cast<SessionReplacedReason>(*reasonValue);
    if (!validReason(reason)) {
      return {CodecError::MalformedPayload, std::nullopt};
    }
    return {CodecError::None, SessionReplaced{reason}};
  }
  case kRequestRudpBindCapabilityMessageId: {
    const auto requestId = reader.uint64();
    if (!requestId.has_value() || *requestId == 0 || reader.remaining() != 0) {
      return {CodecError::MalformedPayload, std::nullopt};
    }
    return {CodecError::None,
            RequestRudpBindCapability{.requestId = *requestId}};
  }
  case kRudpBindCapabilityMessageId: {
    const auto requestId = reader.uint64();
    const auto ttlMillis = reader.uint32();
    std::array<std::byte, 32> capability{};
    for (auto &byte : capability) {
      const auto value = reader.uint8();
      if (!value.has_value()) {
        return {CodecError::MalformedPayload, std::nullopt};
      }
      byte = static_cast<std::byte>(*value);
    }
    if (!requestId.has_value() || *requestId == 0 || !ttlMillis.has_value() ||
        *ttlMillis != kRudpCapabilityTtlMillis || reader.remaining() != 0 ||
        !validCapability(capability)) {
      return {CodecError::MalformedPayload, std::nullopt};
    }
    return {CodecError::None, RudpBindCapability{.requestId = *requestId,
                                                 .ttlMillis = *ttlMillis,
                                                 .capability = capability}};
  }
  default:
    return {CodecError::UnsupportedMessage, std::nullopt};
  }
}

} // namespace

std::optional<std::vector<std::byte>>
SessionProtocolCodec::encodeFrame(const SessionControlMessage &message) {
  Writer payload;
  payload.uint8(kSessionProtocolMajor);
  bool valid = true;
  std::visit(
      [&payload, &valid](const auto &value) {
        using Message = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Message, AuthenticateGameSession>) {
          valid = value.requestId != 0 && validCredential(value.credential);
          if (!valid) {
            return;
          }
          payload.uint32(kAuthenticateMessageId);
          payload.uint64(value.requestId);
          payload.uint16(static_cast<std::uint16_t>(value.credential.size()));
          payload.text(value.credential);
        } else if constexpr (std::is_same_v<Message, Welcome>) {
          valid = value.requestId != 0 && value.sessionId != 0 &&
                  value.sessionGeneration != 0 && !value.nickname.empty() &&
                  value.nickname.size() <=
                      std::numeric_limits<std::uint16_t>::max() &&
                  validUtf8(value.nickname);
          if (!valid) {
            return;
          }
          payload.uint32(kWelcomeMessageId);
          payload.uint64(value.requestId);
          payload.uint64(value.sessionId);
          payload.uint64(value.sessionGeneration);
          payload.uint64(value.serverTimeUnixMillis);
          payload.uint16(static_cast<std::uint16_t>(value.nickname.size()));
          payload.text(value.nickname);
        } else if constexpr (std::is_same_v<Message, AuthenticationRejected>) {
          valid = value.requestId != 0 && validReason(value.reason);
          if (!valid) {
            return;
          }
          payload.uint32(kAuthenticationRejectedMessageId);
          payload.uint64(value.requestId);
          payload.uint16(static_cast<std::uint16_t>(value.reason));
        } else if constexpr (std::is_same_v<Message, SessionReplaced>) {
          valid = validReason(value.reason);
          if (!valid) {
            return;
          }
          payload.uint32(kSessionReplacedMessageId);
          payload.uint16(static_cast<std::uint16_t>(value.reason));
        } else if constexpr (std::is_same_v<Message,
                                            RequestRudpBindCapability>) {
          valid = value.requestId != 0;
          if (!valid) {
            return;
          }
          payload.uint32(kRequestRudpBindCapabilityMessageId);
          payload.uint64(value.requestId);
        } else if constexpr (std::is_same_v<Message, RudpBindCapability>) {
          valid = value.requestId != 0 &&
                  value.ttlMillis == kRudpCapabilityTtlMillis &&
                  validCapability(value.capability);
          if (!valid) {
            return;
          }
          payload.uint32(kRudpBindCapabilityMessageId);
          payload.uint64(value.requestId);
          payload.uint32(value.ttlMillis);
          for (const std::byte byte : value.capability) {
            payload.uint8(std::to_integer<std::uint8_t>(byte));
          }
        }
      },
      message);
  if (!valid ||
      payload.bytes().size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }

  Writer frame;
  frame.uint32(static_cast<std::uint32_t>(payload.bytes().size()));
  for (const std::byte byte : payload.bytes()) {
    frame.uint8(std::to_integer<std::uint8_t>(byte));
  }
  return frame.take();
}

DecodedSessionFrame
SessionProtocolCodec::decodeFrame(std::span<const std::byte> frame) {
  if (frame.size() < kFrameHeaderBytes) {
    return {CodecError::PartialFrame, std::nullopt};
  }
  Reader reader{frame};
  const auto payloadLength = reader.uint32();
  if (!payloadLength.has_value()) {
    return {CodecError::PartialFrame, std::nullopt};
  }
  if (*payloadLength > reader.remaining()) {
    return {CodecError::PartialFrame, std::nullopt};
  }
  if (*payloadLength != reader.remaining()) {
    return {CodecError::FrameLengthMismatch, std::nullopt};
  }
  return decodePayload(frame.subspan(kFrameHeaderBytes));
}

DecodedPreAuthFrame
SessionProtocolCodec::decodePreAuthPayload(std::span<const std::byte> payload) {
  const DecodedSessionFrame decoded = decodePayload(payload);
  if (decoded.error == CodecError::UnsupportedMessage ||
      (decoded.message.has_value() &&
       !std::holds_alternative<AuthenticateGameSession>(*decoded.message))) {
    return DecodedPreAuthFrame::otherMessage();
  }
  if (decoded.error != CodecError::None || !decoded.message.has_value()) {
    return DecodedPreAuthFrame::malformed();
  }
  const auto &authenticate =
      std::get<AuthenticateGameSession>(*decoded.message);
  return DecodedPreAuthFrame::authenticate(NormalizedAuthRequest{
      .requestId = authenticate.requestId,
      .protocolMajor = kSessionProtocolMajor,
      .credential = authenticate.credential,
  });
}

} // namespace lol::transport::tcp
