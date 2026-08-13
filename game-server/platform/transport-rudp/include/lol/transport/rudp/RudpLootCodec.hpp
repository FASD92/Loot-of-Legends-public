#pragma once

#include <lol/transport/rudp/RudpCombatCodec.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace lol::transport::rudp {

enum class RudpClaimLootResultCode : std::uint16_t {
  Ok = 0,
  NotEligible = 1,
  StaleSession = 2,
  StaleBattle = 3,
  InvalidDrop = 4,
  UnknownDrop = 5,
  OutOfRange = 6,
  AlreadyClaimed = 7,
  Overloaded = 8,
  CommandConflict = 9,
  CatalogRejected = 10,
  ResolutionClosed = 11,
};

enum class RudpLootResolutionState : std::uint8_t {
  NotStarted = 0,
  Open = 1,
  Resolved = 2,
};

enum class RudpLootDropState : std::uint8_t {
  Available = 0,
  Claimed = 1,
  Unclaimed = 2,
};

struct RudpClaimLootIntent final {
  RudpCommandId commandId;
  std::uint64_t battleInstanceId;
  std::uint64_t dropId;

  bool operator==(const RudpClaimLootIntent &) const = default;
};

struct RudpClaimLootTerminalResult final {
  RudpCommandId commandId;
  std::uint64_t battleInstanceId;
  std::uint64_t dropId;
  RudpClaimLootResultCode resultCode;

  bool operator==(const RudpClaimLootTerminalResult &) const = default;
};

struct RudpDropSpawned final {
  RudpEventId eventId;
  std::uint64_t battleInstanceId;
  RudpEventStreamKind eventStreamKind;
  std::uint32_t eventSequence;
  std::uint64_t dropId;
  std::uint64_t itemId;
  std::uint64_t quantity;
  std::int32_t posXMillimeter;
  std::int32_t posYMillimeter;
  std::uint16_t rulesetVersion;

  bool operator==(const RudpDropSpawned &) const = default;
};

struct RudpLootDropProjection final {
  std::uint64_t dropId;
  std::uint64_t itemId;
  std::uint64_t quantity;
  std::int32_t posXMillimeter;
  std::int32_t posYMillimeter;
  RudpLootDropState state;
  // Zero means no owner. Only Claimed may carry a nonzero SessionId.
  std::uint64_t ownerSessionId;

  bool operator==(const RudpLootDropProjection &) const = default;
};

struct RudpDropStateSnapshot final {
  std::uint64_t battleInstanceId;
  std::uint32_t snapshotSequence;
  RudpLootResolutionState resolutionState;
  std::vector<RudpLootDropProjection> drops;

  bool operator==(const RudpDropStateSnapshot &) const = default;
};

using RudpLootMessage =
    std::variant<RudpClaimLootIntent, RudpClaimLootTerminalResult,
                 RudpDropSpawned, RudpDropStateSnapshot>;

enum class RudpLootCodecError : std::uint8_t {
  None,
  Header,
  UnsupportedMessage,
  MalformedPayload,
};

struct DecodedRudpLoot final {
  RudpLootCodecError error{RudpLootCodecError::Header};
  std::optional<RudpHeader> header;
  std::optional<RudpLootMessage> message;
};

class RudpLootCodec final {
public:
  [[nodiscard]] static std::optional<std::vector<std::byte>>
  encode(const RudpHeader &header, const RudpLootMessage &message);
  [[nodiscard]] static DecodedRudpLoot
  decode(std::span<const std::byte> datagram);
};

} // namespace lol::transport::rudp
