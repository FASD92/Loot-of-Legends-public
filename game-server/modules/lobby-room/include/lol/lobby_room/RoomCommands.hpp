#pragma once

#include <lol/shared/Identifiers.hpp>

#include <cstdint>
#include <string>

namespace lol::lobby_room {

enum class RoomResultCode : std::uint16_t {
  Ok = 0,
  InvalidArgument = 1,
  AlreadyInRoom = 2,
  RoomNotFound = 3,
  RoomClosed = 4,
  RoomFull = 5,
  NotInRoom = 6,
  NotHost = 7,
  NotEnoughPlayers = 8,
  NotAllReady = 9,
  InvalidTarget = 10,
  RoomOverloaded = 11,
  StaleSession = 12,
};

struct RoomMemberIdentity final {
  shared::AccountId accountId;
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
  std::string nickname;

  bool operator==(const RoomMemberIdentity &) const = default;
};

struct CreateRoomCommand final {
  shared::RoomId roomId;
  std::string title;
  std::uint8_t capacity;
  RoomMemberIdentity creator;
};

struct JoinRoomCommand final {
  RoomMemberIdentity member;
};

struct LeaveRoomCommand final {
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
};

struct SetReadyCommand final {
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
  bool ready;
};

struct KickRoomMemberCommand final {
  shared::SessionId actorSessionId;
  shared::SessionGeneration actorGeneration;
  shared::SessionId targetSessionId;
  shared::SessionGeneration targetGeneration;
};

struct HostStartEligibilityCommand final {
  shared::SessionId actorSessionId;
  shared::SessionGeneration actorGeneration;
};

} // namespace lol::lobby_room
