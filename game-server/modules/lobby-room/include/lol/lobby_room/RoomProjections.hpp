#pragma once

#include <lol/lobby_room/RoomCommands.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lol::lobby_room {

enum class RoomLifecycle : std::uint8_t {
  Open,
  Loading,
  InProgress,
  AwaitingSettlementDurability,
};

struct RoomMemberSnapshot final {
  shared::AccountId accountId;
  shared::SessionId sessionId;
  shared::SessionGeneration sessionGeneration;
  std::string nickname;
  bool ready;

  bool operator==(const RoomMemberSnapshot &) const = default;
};

struct RoomSummary final {
  shared::RoomId roomId;
  std::string title;
  std::uint8_t memberCount;
  std::uint8_t capacity;

  bool operator==(const RoomSummary &) const = default;
};

struct RoomDetailProjection final {
  shared::RoomId roomId;
  RoomLifecycle lifecycle;
  std::string title;
  std::uint8_t capacity;
  shared::SessionId hostSessionId;
  shared::SessionGeneration hostSessionGeneration;
  std::vector<RoomMemberSnapshot> members;

  bool operator==(const RoomDetailProjection &) const = default;
};

struct BattleAdmissionSnapshot final {
  shared::RoomId roomId;
  shared::SessionId host;
  std::vector<RoomMemberSnapshot> members;

  bool operator==(const BattleAdmissionSnapshot &) const = default;
};

struct HostStartEligibility final {
  RoomResultCode code;
  std::optional<BattleAdmissionSnapshot> admission;
};

} // namespace lol::lobby_room
