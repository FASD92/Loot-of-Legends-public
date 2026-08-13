#include <lol/transport/rudp/RudpCodec.hpp>

#include <algorithm>
#include <type_traits>
#include <utility>

namespace lol::transport::rudp {
namespace {

constexpr std::uint16_t kBindHelloMessageId = 22;
constexpr std::uint16_t kBindAcceptedMessageId = 23;
constexpr std::uint16_t kHeartbeatMessageId = 24;

bool validHeader(const RudpHeader &header, const RudpControlMessage &message) {
  return std::visit(
      [&header](const auto &value) {
        using Message = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Message, RudpBindHello>) {
          return header.flag == RudpFlag::Reliable &&
                 header.messageId == kBindHelloMessageId &&
                 header.transportEpoch == 0;
        } else if constexpr (std::is_same_v<Message, RudpBindAccepted>) {
          return header.flag == RudpFlag::Reliable &&
                 header.messageId == kBindAcceptedMessageId &&
                 header.transportEpoch != 0;
        } else {
          return header.flag == RudpFlag::Heartbeat &&
                 header.messageId == kHeartbeatMessageId &&
                 header.transportEpoch != 0;
        }
      },
      message);
}

} // namespace

std::optional<std::vector<std::byte>>
RudpControlCodec::encode(const RudpHeader &header,
                         const RudpControlMessage &message) {
  if (!validHeader(header, message)) {
    return std::nullopt;
  }
  std::vector<std::byte> payload;
  if (const auto *hello = std::get_if<RudpBindHello>(&message)) {
    payload.assign(hello->capability.bytes.begin(),
                   hello->capability.bytes.end());
  }
  return RudpHeaderCodec::encode(header, payload);
}

DecodedRudpControl
RudpControlCodec::decode(std::span<const std::byte> datagram) {
  const auto decoded = RudpHeaderCodec::decode(datagram);
  if (decoded.error != RudpHeaderError::None || !decoded.header.has_value()) {
    return {.error = RudpControlCodecError::Header,
            .header = std::nullopt,
            .message = std::nullopt};
  }

  std::optional<RudpControlMessage> message;
  switch (decoded.header->messageId) {
  case kBindHelloMessageId: {
    if (decoded.header->flag != RudpFlag::Reliable ||
        decoded.header->transportEpoch != 0 ||
        decoded.payload.size() != RudpBindCapability::Bytes{}.size()) {
      return {.error = RudpControlCodecError::MalformedPayload,
              .header = decoded.header,
              .message = std::nullopt};
    }
    RudpBindCapability::Bytes capability{};
    std::ranges::copy(decoded.payload, capability.begin());
    message = RudpBindHello{RudpBindCapability{capability}};
    break;
  }
  case kBindAcceptedMessageId:
    if (decoded.header->flag != RudpFlag::Reliable ||
        decoded.header->transportEpoch == 0 || !decoded.payload.empty()) {
      return {.error = RudpControlCodecError::MalformedPayload,
              .header = decoded.header,
              .message = std::nullopt};
    }
    message = RudpBindAccepted{};
    break;
  case kHeartbeatMessageId:
    if (decoded.header->flag != RudpFlag::Heartbeat ||
        decoded.header->transportEpoch == 0 || !decoded.payload.empty()) {
      return {.error = RudpControlCodecError::MalformedPayload,
              .header = decoded.header,
              .message = std::nullopt};
    }
    message = RudpHeartbeat{};
    break;
  default:
    return {.error = RudpControlCodecError::UnsupportedMessage,
            .header = decoded.header,
            .message = std::nullopt};
  }
  return {.error = RudpControlCodecError::None,
          .header = decoded.header,
          .message = std::move(message)};
}

} // namespace lol::transport::rudp
