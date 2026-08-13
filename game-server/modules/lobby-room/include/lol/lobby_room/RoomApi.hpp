#pragma once

#include <lol/lobby_room/RoomCommands.hpp>
#include <lol/lobby_room/RoomProjections.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lol::lobby_room {

struct CreateRoomResult;

class Room final {
public:
  Room(Room &&) noexcept = default;
  Room &operator=(Room &&) noexcept = default;
  Room(const Room &) = delete;
  Room &operator=(const Room &) = delete;

  [[nodiscard]] static CreateRoomResult create(CreateRoomCommand command);

  [[nodiscard]] RoomResultCode join(JoinRoomCommand command);
  [[nodiscard]] RoomResultCode leave(const LeaveRoomCommand &command);
  [[nodiscard]] RoomResultCode setReady(const SetReadyCommand &command);
  [[nodiscard]] RoomResultCode kick(const KickRoomMemberCommand &command);
  [[nodiscard]] HostStartEligibility
  prepareHostStart(const HostStartEligibilityCommand &command) const;
  [[nodiscard]] RoomResultCode
  commitLoading(const BattleAdmissionSnapshot &admission);
  [[nodiscard]] RoomResultCode commitInProgress();
  [[nodiscard]] RoomResultCode commitAwaitingSettlementDurability();
  [[nodiscard]] RoomResultCode reopenAfterLoadCancelled();
  [[nodiscard]] RoomResultCode reopenAfterSettlementDurability();

  [[nodiscard]] std::optional<RoomSummary> summary() const;
  [[nodiscard]] std::optional<RoomDetailProjection> detail() const;
  [[nodiscard]] RoomLifecycle lifecycle() const noexcept;
  [[nodiscard]] bool closed() const noexcept;

private:
  Room(shared::RoomId roomId, std::string title, std::uint8_t capacity,
       RoomMemberIdentity creator);

  shared::RoomId roomId_;
  std::string title_;
  std::uint8_t capacity_;
  RoomLifecycle lifecycle_{RoomLifecycle::Open};
  std::vector<RoomMemberSnapshot> members_;
};

struct CreateRoomResult final {
  RoomResultCode code;
  std::optional<Room> room;
};

} // namespace lol::lobby_room
