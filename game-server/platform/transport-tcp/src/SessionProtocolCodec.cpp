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
constexpr std::uint32_t kBattleResumeSnapshotMessageId = 39;
constexpr std::uint32_t kResumeBattleSessionMessageId = 40;
constexpr std::uint32_t kBattleResumeSnapshotAppliedMessageId = 41;
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
  case AuthenticationRejectedReason::ResumeUnavailable:
    return true;
  }
  return false;
}

bool validPhase(BattleResumePhase phase) noexcept {
  return phase == BattleResumePhase::Combat ||
         phase == BattleResumePhase::Loot || phase == BattleResumePhase::Result;
}

bool validSnapshot(const BattleResumeSnapshot &snapshot) {
  if (snapshot.requestId == 0 || snapshot.snapshotId == 0 ||
      snapshot.roomId == 0 || snapshot.battleInstanceId == 0 ||
      snapshot.playerSessionId == 0 || snapshot.sessionGeneration == 0 ||
      !validPhase(snapshot.phase) || snapshot.players.size() < 2u ||
      snapshot.players.size() > 10u || snapshot.drops.size() > 10u ||
      (snapshot.phase == BattleResumePhase::Result) !=
          snapshot.result.has_value() ||
      (snapshot.phase == BattleResumePhase::Result &&
       snapshot.remainingMillis != 0u)) {
    return false;
  }
  std::vector<std::uint64_t> sessions;
  sessions.reserve(snapshot.players.size());
  bool containsPlayer = false;
  for (const auto &player : snapshot.players) {
    if (player.sessionId == 0 ||
        (!player.healthKnown &&
         (player.hitPoints != 0u || player.maximumHitPoints != 0u)) ||
        (player.healthKnown && (player.maximumHitPoints == 0u ||
                                player.hitPoints > player.maximumHitPoints)) ||
        std::ranges::find(sessions, player.sessionId) != sessions.end()) {
      return false;
    }
    sessions.push_back(player.sessionId);
    containsPlayer =
        containsPlayer || player.sessionId == snapshot.playerSessionId;
  }
  if (!containsPlayer) {
    return false;
  }
  if (snapshot.monster.has_value() &&
      (snapshot.monster->monsterId == 0u ||
       snapshot.monster->maximumHitPoints == 0u ||
       snapshot.monster->hitPoints > snapshot.monster->maximumHitPoints ||
       snapshot.monster->state > 3u)) {
    return false;
  }
  std::vector<std::uint64_t> drops;
  drops.reserve(snapshot.drops.size());
  for (const auto &drop : snapshot.drops) {
    if (drop.dropId == 0u || drop.itemId == 0u || drop.quantity == 0u ||
        drop.state > 2u || (drop.state == 1u) != (drop.ownerSessionId != 0u) ||
        std::ranges::find(drops, drop.dropId) != drops.end()) {
      return false;
    }
    drops.push_back(drop.dropId);
  }
  return !snapshot.result.has_value() ||
         (snapshot.result->roomId == snapshot.roomId &&
          snapshot.result->battleInstanceId == snapshot.battleInstanceId &&
          FinalResultProtocolCodec::encodeServerFrame(*snapshot.result)
              .has_value());
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
  case kResumeBattleSessionMessageId: {
    const auto requestId = reader.uint64();
    const auto sessionId = reader.uint64();
    const auto generation = reader.uint64();
    const auto credentialLength = reader.uint16();
    if (!requestId.has_value() || *requestId == 0 || !sessionId.has_value() ||
        *sessionId == 0 || !generation.has_value() || *generation == 0 ||
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
            ResumeBattleSession{.requestId = *requestId,
                                .previousSessionId = *sessionId,
                                .previousSessionGeneration = *generation,
                                .credential = std::move(*credential)}};
  }
  case kBattleResumeSnapshotAppliedMessageId: {
    const auto snapshotId = reader.uint64();
    if (!snapshotId.has_value() || *snapshotId == 0 ||
        reader.remaining() != 0) {
      return {CodecError::MalformedPayload, std::nullopt};
    }
    return {CodecError::None,
            BattleResumeSnapshotApplied{.snapshotId = *snapshotId}};
  }
  case kBattleResumeSnapshotMessageId: {
    BattleResumeSnapshot snapshot{};
    const auto requestId = reader.uint64();
    const auto snapshotId = reader.uint64();
    const auto roomId = reader.uint64();
    const auto battleId = reader.uint64();
    const auto playerSessionId = reader.uint64();
    const auto generation = reader.uint64();
    const auto phase = reader.uint8();
    const auto remainingMillis = reader.uint32();
    const auto serverTick = reader.uint32();
    const auto playerCount = reader.uint16();
    if (!requestId.has_value() || !snapshotId.has_value() ||
        !roomId.has_value() || !battleId.has_value() ||
        !playerSessionId.has_value() || !generation.has_value() ||
        !phase.has_value() || !remainingMillis.has_value() ||
        !serverTick.has_value() || !playerCount.has_value() ||
        *playerCount > 10u) {
      return {CodecError::MalformedPayload, std::nullopt};
    }
    snapshot.requestId = *requestId;
    snapshot.snapshotId = *snapshotId;
    snapshot.roomId = *roomId;
    snapshot.battleInstanceId = *battleId;
    snapshot.playerSessionId = *playerSessionId;
    snapshot.sessionGeneration = *generation;
    snapshot.phase = static_cast<BattleResumePhase>(*phase);
    snapshot.remainingMillis = *remainingMillis;
    snapshot.serverTick = *serverTick;
    snapshot.players.reserve(*playerCount);
    for (std::uint16_t index = 0; index < *playerCount; ++index) {
      const auto sessionId = reader.uint64();
      const auto x = reader.int32();
      const auto y = reader.int32();
      const auto healthKnown = reader.uint8();
      const auto hitPoints = reader.uint32();
      const auto maximumHitPoints = reader.uint32();
      const auto alive = reader.uint8();
      if (!sessionId.has_value() || !x.has_value() || !y.has_value() ||
          !healthKnown.has_value() || *healthKnown > 1u ||
          !hitPoints.has_value() || !maximumHitPoints.has_value() ||
          !alive.has_value() || *alive > 1u) {
        return {CodecError::MalformedPayload, std::nullopt};
      }
      snapshot.players.push_back(BattleResumePlayer{
          .sessionId = *sessionId,
          .positionXMillimeters = *x,
          .positionYMillimeters = *y,
          .healthKnown = *healthKnown == 1u,
          .hitPoints = *hitPoints,
          .maximumHitPoints = *maximumHitPoints,
          .alive = *alive == 1u,
      });
    }
    const auto hasMonster = reader.uint8();
    if (!hasMonster.has_value() || *hasMonster > 1u) {
      return {CodecError::MalformedPayload, std::nullopt};
    }
    if (*hasMonster == 1u) {
      const auto monsterId = reader.uint64();
      const auto x = reader.int32();
      const auto y = reader.int32();
      const auto hitPoints = reader.uint32();
      const auto maximumHitPoints = reader.uint32();
      const auto state = reader.uint8();
      if (!monsterId.has_value() || !x.has_value() || !y.has_value() ||
          !hitPoints.has_value() || !maximumHitPoints.has_value() ||
          !state.has_value()) {
        return {CodecError::MalformedPayload, std::nullopt};
      }
      snapshot.monster = BattleResumeMonster{
          .monsterId = *monsterId,
          .positionXMillimeters = *x,
          .positionYMillimeters = *y,
          .hitPoints = *hitPoints,
          .maximumHitPoints = *maximumHitPoints,
          .state = *state,
      };
    }
    const auto dropCount = reader.uint16();
    if (!dropCount.has_value() || *dropCount > 10u) {
      return {CodecError::MalformedPayload, std::nullopt};
    }
    snapshot.drops.reserve(*dropCount);
    for (std::uint16_t index = 0; index < *dropCount; ++index) {
      const auto dropId = reader.uint64();
      const auto itemId = reader.uint64();
      const auto quantity = reader.uint64();
      const auto x = reader.int32();
      const auto y = reader.int32();
      const auto state = reader.uint8();
      const auto ownerSessionId = reader.uint64();
      if (!dropId.has_value() || !itemId.has_value() || !quantity.has_value() ||
          !x.has_value() || !y.has_value() || !state.has_value() ||
          !ownerSessionId.has_value()) {
        return {CodecError::MalformedPayload, std::nullopt};
      }
      snapshot.drops.push_back(BattleResumeDrop{
          .dropId = *dropId,
          .itemId = *itemId,
          .quantity = *quantity,
          .positionXMillimeters = *x,
          .positionYMillimeters = *y,
          .state = *state,
          .ownerSessionId = *ownerSessionId,
      });
    }
    const auto score = reader.uint64();
    const auto hasResult = reader.uint8();
    if (!score.has_value() || !hasResult.has_value() || *hasResult > 1u) {
      return {CodecError::MalformedPayload, std::nullopt};
    }
    snapshot.score = *score;
    if (*hasResult == 1u) {
      const auto outcomeValue = reader.uint8();
      const auto entryCount = reader.uint16();
      if (!outcomeValue.has_value() || !entryCount.has_value() ||
          *entryCount > 10u) {
        return {CodecError::MalformedPayload, std::nullopt};
      }
      const auto outcome = static_cast<FinalResultOutcome>(*outcomeValue);
      std::vector<FinalResultEntry> entries;
      entries.reserve(*entryCount);
      for (std::uint16_t index = 0; index < *entryCount; ++index) {
        const auto sessionId = reader.uint64();
        const auto nicknameLength = reader.uint16();
        if (!sessionId.has_value() || !nicknameLength.has_value() ||
            *nicknameLength == 0u) {
          return {CodecError::MalformedPayload, std::nullopt};
        }
        auto nickname = reader.text(*nicknameLength);
        const auto exitStatus = reader.uint8();
        const auto finalAssetValue = reader.uint64();
        const auto rank = reader.uint32();
        const auto isTop = reader.uint8();
        if (!nickname.has_value() || !exitStatus.has_value() ||
            !finalAssetValue.has_value() || !rank.has_value() ||
            !isTop.has_value() || *isTop > 1u) {
          return {CodecError::MalformedPayload, std::nullopt};
        }
        entries.push_back(FinalResultEntry{
            .sessionId = *sessionId,
            .nickname = std::move(*nickname),
            .exitStatus = static_cast<FinalResultExitStatus>(*exitStatus),
            .finalAssetValue = *finalAssetValue,
            .rank = *rank,
            .isTop = *isTop == 1u,
        });
      }
      snapshot.result = FinalResult{
          .roomId = snapshot.roomId,
          .battleInstanceId = snapshot.battleInstanceId,
          .outcome = outcome,
          .entries = std::move(entries),
      };
    }
    if (reader.remaining() != 0u || !validSnapshot(snapshot)) {
      return {CodecError::MalformedPayload, std::nullopt};
    }
    return {CodecError::None, std::move(snapshot)};
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
        } else if constexpr (std::is_same_v<Message, ResumeBattleSession>) {
          valid = value.requestId != 0u && value.previousSessionId != 0u &&
                  value.previousSessionGeneration != 0u &&
                  validCredential(value.credential);
          if (!valid) {
            return;
          }
          payload.uint32(kResumeBattleSessionMessageId);
          payload.uint64(value.requestId);
          payload.uint64(value.previousSessionId);
          payload.uint64(value.previousSessionGeneration);
          payload.uint16(static_cast<std::uint16_t>(value.credential.size()));
          payload.text(value.credential);
        } else if constexpr (std::is_same_v<Message,
                                            BattleResumeSnapshotApplied>) {
          valid = value.snapshotId != 0u;
          if (!valid) {
            return;
          }
          payload.uint32(kBattleResumeSnapshotAppliedMessageId);
          payload.uint64(value.snapshotId);
        } else if constexpr (std::is_same_v<Message, BattleResumeSnapshot>) {
          valid = validSnapshot(value);
          if (!valid) {
            return;
          }
          payload.uint32(kBattleResumeSnapshotMessageId);
          payload.uint64(value.requestId);
          payload.uint64(value.snapshotId);
          payload.uint64(value.roomId);
          payload.uint64(value.battleInstanceId);
          payload.uint64(value.playerSessionId);
          payload.uint64(value.sessionGeneration);
          payload.uint8(static_cast<std::uint8_t>(value.phase));
          payload.uint32(value.remainingMillis);
          payload.uint32(value.serverTick);
          payload.uint16(static_cast<std::uint16_t>(value.players.size()));
          for (const auto &player : value.players) {
            payload.uint64(player.sessionId);
            payload.int32(player.positionXMillimeters);
            payload.int32(player.positionYMillimeters);
            payload.uint8(player.healthKnown ? 1u : 0u);
            payload.uint32(player.hitPoints);
            payload.uint32(player.maximumHitPoints);
            payload.uint8(player.alive ? 1u : 0u);
          }
          payload.uint8(value.monster.has_value() ? 1u : 0u);
          if (value.monster.has_value()) {
            payload.uint64(value.monster->monsterId);
            payload.int32(value.monster->positionXMillimeters);
            payload.int32(value.monster->positionYMillimeters);
            payload.uint32(value.monster->hitPoints);
            payload.uint32(value.monster->maximumHitPoints);
            payload.uint8(value.monster->state);
          }
          payload.uint16(static_cast<std::uint16_t>(value.drops.size()));
          for (const auto &drop : value.drops) {
            payload.uint64(drop.dropId);
            payload.uint64(drop.itemId);
            payload.uint64(drop.quantity);
            payload.int32(drop.positionXMillimeters);
            payload.int32(drop.positionYMillimeters);
            payload.uint8(drop.state);
            payload.uint64(drop.ownerSessionId);
          }
          payload.uint64(value.score);
          payload.uint8(value.result.has_value() ? 1u : 0u);
          if (value.result.has_value()) {
            payload.uint8(static_cast<std::uint8_t>(value.result->outcome));
            payload.uint16(
                static_cast<std::uint16_t>(value.result->entries.size()));
            for (const auto &entry : value.result->entries) {
              payload.uint64(entry.sessionId);
              payload.uint16(static_cast<std::uint16_t>(entry.nickname.size()));
              payload.text(entry.nickname);
              payload.uint8(static_cast<std::uint8_t>(entry.exitStatus));
              payload.uint64(entry.finalAssetValue);
              payload.uint32(entry.rank);
              payload.uint8(entry.isTop ? 1u : 0u);
            }
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
       !std::holds_alternative<AuthenticateGameSession>(*decoded.message) &&
       !std::holds_alternative<ResumeBattleSession>(*decoded.message))) {
    return DecodedPreAuthFrame::otherMessage();
  }
  if (decoded.error != CodecError::None || !decoded.message.has_value()) {
    return DecodedPreAuthFrame::malformed();
  }
  if (const auto *resume =
          std::get_if<ResumeBattleSession>(&*decoded.message)) {
    return DecodedPreAuthFrame::authenticate(NormalizedAuthRequest{
        .requestId = resume->requestId,
        .protocolMajor = kSessionProtocolMajor,
        .credential = resume->credential,
        .resumeRequested = true,
        .previousSessionId = resume->previousSessionId,
        .previousSessionGeneration = resume->previousSessionGeneration,
    });
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
