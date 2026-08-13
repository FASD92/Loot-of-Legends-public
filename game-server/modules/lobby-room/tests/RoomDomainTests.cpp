#include <lol/lobby_room/RoomApi.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

namespace {

using lol::lobby_room::CreateRoomCommand;
using lol::lobby_room::HostStartEligibilityCommand;
using lol::lobby_room::JoinRoomCommand;
using lol::lobby_room::KickRoomMemberCommand;
using lol::lobby_room::LeaveRoomCommand;
using lol::lobby_room::Room;
using lol::lobby_room::RoomMemberIdentity;
using lol::lobby_room::RoomResultCode;
using lol::lobby_room::SetReadyCommand;
using lol::shared::AccountId;
using lol::shared::RoomId;
using lol::shared::SessionGeneration;
using lol::shared::SessionId;

AccountId account(std::uint64_t suffix) {
  AccountId::Bytes bytes{};
  bytes.back() = static_cast<std::uint8_t>(suffix);
  return AccountId{bytes};
}

RoomMemberIdentity member(std::uint64_t sessionId, std::uint64_t generation,
                          std::string nickname) {
  return RoomMemberIdentity{
      .accountId = account(sessionId),
      .sessionId = SessionId{sessionId},
      .generation = SessionGeneration{generation},
      .nickname = std::move(nickname),
  };
}

auto create(std::string title = "room", std::uint8_t capacity = 3) {
  return Room::create(CreateRoomCommand{
      .roomId = RoomId{7},
      .title = std::move(title),
      .capacity = capacity,
      .creator = member(1, 1, "host"),
  });
}

bool invalidCreate(std::string title, std::uint8_t capacity = 3,
                   std::uint64_t roomId = 7) {
  const auto result = Room::create(CreateRoomCommand{
      .roomId = RoomId{roomId},
      .title = std::move(title),
      .capacity = capacity,
      .creator = member(1, 1, "host"),
  });
  return result.code == RoomResultCode::InvalidArgument &&
         !result.room.has_value();
}

bool createValidatesAndNormalizesRoom() {
  auto created = create(" \troom\r\n", 2);
  if (created.code != RoomResultCode::Ok || !created.room.has_value()) {
    return false;
  }
  const auto detail = created.room->detail();
  const auto summary = created.room->summary();
  if (!detail.has_value() || detail->roomId != RoomId{7} ||
      detail->title != "room" || detail->capacity != 2 ||
      detail->hostSessionId != SessionId{1} ||
      detail->hostSessionGeneration != SessionGeneration{1} ||
      detail->members.size() != 1 || detail->members.front().ready ||
      !summary.has_value() || summary->roomId != RoomId{7} ||
      summary->title != "room" || summary->memberCount != 1 ||
      summary->capacity != 2) {
    return false;
  }

  const std::string embeddedNul{"bad\0title", 9};
  const std::string invalidUtf8(1, static_cast<char>(0xff));
  const std::string c1Control = std::string{"bad"} + "\xc2\x80";
  return invalidCreate(" ") && invalidCreate(std::string(49, 'x')) &&
         invalidCreate("room", 1) && invalidCreate("room", 11) &&
         invalidCreate("room", 3, 0) && invalidCreate("bad\nname") &&
         invalidCreate(embeddedNul) && invalidCreate(invalidUtf8) &&
         invalidCreate(c1Control) &&
         create("가나다", 10).code == RoomResultCode::Ok;
}

bool joinCapacityAndDuplicateAreAuthoritative() {
  auto created = create("room", 2);
  if (!created.room.has_value()) {
    return false;
  }
  Room room = std::move(*created.room);
  const JoinRoomCommand second{member(2, 1, "second")};
  if (room.join(second) != RoomResultCode::Ok ||
      room.join(second) != RoomResultCode::Ok ||
      room.join(JoinRoomCommand{member(3, 1, "third")}) !=
          RoomResultCode::RoomFull) {
    return false;
  }
  const auto detail = room.detail();
  if (!detail.has_value() || detail->members.size() != 2) {
    return false;
  }

  auto roomy = create("room", 10);
  if (!roomy.room.has_value() ||
      roomy.room->join(JoinRoomCommand{member(2, 2, "second")}) !=
          RoomResultCode::Ok ||
      roomy.room->join(JoinRoomCommand{member(2, 3, "replacement")}) !=
          RoomResultCode::StaleSession ||
      roomy.room->join(JoinRoomCommand{member(0, 1, "invalid")}) !=
          RoomResultCode::InvalidArgument) {
    return false;
  }
  return true;
}

bool leaveReadyHostTransferAndClose() {
  auto created = create();
  if (!created.room.has_value()) {
    return false;
  }
  Room room = std::move(*created.room);
  if (room.join(JoinRoomCommand{member(2, 1, "second")}) !=
          RoomResultCode::Ok ||
      room.join(JoinRoomCommand{member(3, 1, "third")}) != RoomResultCode::Ok ||
      room.setReady(SetReadyCommand{SessionId{9}, SessionGeneration{1},
                                    true}) != RoomResultCode::NotInRoom ||
      room.setReady(SetReadyCommand{SessionId{2}, SessionGeneration{2},
                                    true}) != RoomResultCode::StaleSession ||
      room.setReady(SetReadyCommand{SessionId{2}, SessionGeneration{1},
                                    true}) != RoomResultCode::Ok ||
      room.setReady(SetReadyCommand{SessionId{2}, SessionGeneration{1},
                                    true}) != RoomResultCode::Ok) {
    return false;
  }

  if (room.leave(LeaveRoomCommand{SessionId{1}, SessionGeneration{1}}) !=
          RoomResultCode::Ok ||
      room.leave(LeaveRoomCommand{SessionId{1}, SessionGeneration{1}}) !=
          RoomResultCode::NotInRoom) {
    return false;
  }
  auto detail = room.detail();
  if (!detail.has_value() || detail->hostSessionId != SessionId{2} ||
      detail->members.size() != 2 || !detail->members.front().ready) {
    return false;
  }

  if (room.leave(LeaveRoomCommand{SessionId{2}, SessionGeneration{1}}) !=
          RoomResultCode::Ok ||
      room.detail()->hostSessionId != SessionId{3} ||
      room.leave(LeaveRoomCommand{SessionId{3}, SessionGeneration{1}}) !=
          RoomResultCode::Ok ||
      !room.closed() || room.detail().has_value() ||
      room.summary().has_value() ||
      room.join(JoinRoomCommand{member(4, 1, "late")}) !=
          RoomResultCode::RoomClosed) {
    return false;
  }
  return true;
}

bool kickChecksHostAndTarget() {
  auto created = create();
  if (!created.room.has_value()) {
    return false;
  }
  Room room = std::move(*created.room);
  if (room.join(JoinRoomCommand{member(2, 1, "second")}) !=
          RoomResultCode::Ok ||
      room.join(JoinRoomCommand{member(3, 1, "third")}) != RoomResultCode::Ok) {
    return false;
  }

  if (room.kick(KickRoomMemberCommand{SessionId{2}, SessionGeneration{1},
                                      SessionId{3}, SessionGeneration{1}}) !=
          RoomResultCode::NotHost ||
      room.kick(KickRoomMemberCommand{SessionId{1}, SessionGeneration{2},
                                      SessionId{3}, SessionGeneration{1}}) !=
          RoomResultCode::StaleSession ||
      room.kick(KickRoomMemberCommand{SessionId{1}, SessionGeneration{1},
                                      SessionId{1}, SessionGeneration{1}}) !=
          RoomResultCode::InvalidTarget ||
      room.kick(KickRoomMemberCommand{SessionId{1}, SessionGeneration{1},
                                      SessionId{3}, SessionGeneration{2}}) !=
          RoomResultCode::InvalidTarget ||
      room.kick(KickRoomMemberCommand{SessionId{1}, SessionGeneration{1},
                                      SessionId{9}, SessionGeneration{1}}) !=
          RoomResultCode::InvalidTarget ||
      room.kick(KickRoomMemberCommand{SessionId{1}, SessionGeneration{1},
                                      SessionId{2}, SessionGeneration{1}}) !=
          RoomResultCode::Ok ||
      room.kick(KickRoomMemberCommand{SessionId{1}, SessionGeneration{1},
                                      SessionId{2}, SessionGeneration{1}}) !=
          RoomResultCode::InvalidTarget) {
    return false;
  }
  const auto detail = room.detail();
  return detail.has_value() && detail->members.size() == 2 &&
         detail->members[1].sessionId == SessionId{3};
}

bool hostStartEligibilityIsPureAndOrdered() {
  auto created = create();
  if (!created.room.has_value()) {
    return false;
  }
  Room room = std::move(*created.room);
  const HostStartEligibilityCommand host{SessionId{1}, SessionGeneration{1}};
  if (room.prepareHostStart(host).code != RoomResultCode::NotEnoughPlayers ||
      room.join(JoinRoomCommand{member(2, 1, "second")}) !=
          RoomResultCode::Ok ||
      room.prepareHostStart(
              HostStartEligibilityCommand{SessionId{2}, SessionGeneration{1}})
              .code != RoomResultCode::NotHost ||
      room.prepareHostStart(host).code != RoomResultCode::NotAllReady ||
      room.setReady(SetReadyCommand{SessionId{1}, SessionGeneration{1},
                                    true}) != RoomResultCode::Ok ||
      room.setReady(SetReadyCommand{SessionId{2}, SessionGeneration{1},
                                    true}) != RoomResultCode::Ok) {
    return false;
  }

  const auto beforeEligibility = room.detail();
  const auto eligible = room.prepareHostStart(host);
  if (eligible.code != RoomResultCode::Ok || !eligible.admission.has_value() ||
      eligible.admission->roomId != RoomId{7} ||
      eligible.admission->host != SessionId{1} ||
      eligible.admission->members.size() != 2 ||
      eligible.admission->members[0].sessionId != SessionId{1} ||
      eligible.admission->members[1].sessionId != SessionId{2} ||
      room.detail() != beforeEligibility) {
    return false;
  }

  if (room.join(JoinRoomCommand{member(3, 1, "third")}) != RoomResultCode::Ok ||
      room.prepareHostStart(host).code != RoomResultCode::NotAllReady ||
      room.setReady(SetReadyCommand{SessionId{3}, SessionGeneration{1},
                                    true}) != RoomResultCode::Ok ||
      room.prepareHostStart(host).code != RoomResultCode::Ok) {
    return false;
  }
  return room.detail()->members.size() == 3;
}

} // namespace

int main() {
  if (!createValidatesAndNormalizesRoom() ||
      !joinCapacityAndDuplicateAreAuthoritative() ||
      !leaveReadyHostTransferAndClose() || !kickChecksHostAndTarget() ||
      !hostStartEligibilityIsPureAndOrdered()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
