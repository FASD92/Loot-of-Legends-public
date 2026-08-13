#include <lol/lobby_room/RoomApi.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

namespace lol::lobby_room {
namespace {

constexpr std::uint8_t kMinimumCapacity = 2;
constexpr std::uint8_t kMaximumCapacity = 10;
constexpr std::size_t kMaximumTitleBytes = 48;

bool asciiWhitespace(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
         value == '\f' || value == '\v';
}

bool validUtf8(std::string_view text, bool rejectControls) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto lead = static_cast<unsigned char>(text[index]);
    if (lead <= 0x7fU) {
      if (rejectControls && (lead <= 0x1fU || lead == 0x7fU)) {
        return false;
      }
      ++index;
      continue;
    }

    std::size_t continuationCount = 0;
    unsigned char secondMinimum = 0x80U;
    unsigned char secondMaximum = 0xbfU;
    std::uint32_t codePoint = 0;
    if (lead >= 0xc2U && lead <= 0xdfU) {
      continuationCount = 1;
      codePoint = static_cast<std::uint32_t>(lead & 0x1fU);
    } else if (lead >= 0xe0U && lead <= 0xefU) {
      continuationCount = 2;
      codePoint = static_cast<std::uint32_t>(lead & 0x0fU);
      if (lead == 0xe0U) {
        secondMinimum = 0xa0U;
      } else if (lead == 0xedU) {
        secondMaximum = 0x9fU;
      }
    } else if (lead >= 0xf0U && lead <= 0xf4U) {
      continuationCount = 3;
      codePoint = static_cast<std::uint32_t>(lead & 0x07U);
      if (lead == 0xf0U) {
        secondMinimum = 0x90U;
      } else if (lead == 0xf4U) {
        secondMaximum = 0x8fU;
      }
    } else {
      return false;
    }

    if (text.size() - index <= continuationCount) {
      return false;
    }
    const auto second = static_cast<unsigned char>(text[index + 1]);
    if (second < secondMinimum || second > secondMaximum) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuationCount; ++offset) {
      const auto continuation =
          static_cast<unsigned char>(text[index + offset]);
      if (continuation < 0x80U || continuation > 0xbfU) {
        return false;
      }
      codePoint =
          (codePoint << 6U) | static_cast<std::uint32_t>(continuation & 0x3fU);
    }
    if (rejectControls && codePoint >= 0x7fU && codePoint <= 0x9fU) {
      return false;
    }
    index += continuationCount + 1;
  }
  return true;
}

std::optional<std::string> normalizeTitle(std::string title) {
  std::size_t first = 0;
  while (first < title.size() && asciiWhitespace(title[first])) {
    ++first;
  }
  std::size_t last = title.size();
  while (last > first && asciiWhitespace(title[last - 1])) {
    --last;
  }
  std::string normalized = title.substr(first, last - first);
  if (normalized.empty() || normalized.size() > kMaximumTitleBytes ||
      !validUtf8(normalized, true)) {
    return std::nullopt;
  }
  return normalized;
}

bool validMember(const RoomMemberIdentity &member) noexcept {
  const bool validAccount = std::any_of(
      member.accountId.bytes().begin(), member.accountId.bytes().end(),
      [](std::uint8_t value) { return value != 0; });
  return validAccount && member.sessionId.value() != 0 &&
         member.generation.value() != 0 && !member.nickname.empty() &&
         member.nickname.size() <= std::numeric_limits<std::uint16_t>::max() &&
         validUtf8(member.nickname, false);
}

} // namespace

Room::Room(shared::RoomId roomId, std::string title, std::uint8_t capacity,
           RoomMemberIdentity creator)
    : roomId_(roomId), title_(std::move(title)), capacity_(capacity),
      members_{RoomMemberSnapshot{
          .accountId = creator.accountId,
          .sessionId = creator.sessionId,
          .sessionGeneration = creator.generation,
          .nickname = std::move(creator.nickname),
          .ready = false,
      }} {}

CreateRoomResult Room::create(CreateRoomCommand command) {
  auto title = normalizeTitle(std::move(command.title));
  if (command.roomId.value() == 0 || command.capacity < kMinimumCapacity ||
      command.capacity > kMaximumCapacity || !validMember(command.creator) ||
      !title.has_value()) {
    return {RoomResultCode::InvalidArgument, std::nullopt};
  }
  return {RoomResultCode::Ok,
          Room{command.roomId, std::move(*title), command.capacity,
               std::move(command.creator)}};
}

RoomResultCode Room::join(JoinRoomCommand command) {
  if (!validMember(command.member)) {
    return RoomResultCode::InvalidArgument;
  }
  if (closed() || lifecycle_ != RoomLifecycle::Open) {
    return RoomResultCode::RoomClosed;
  }
  const auto existing = std::find_if(
      members_.begin(), members_.end(), [&command](const auto &member) {
        return member.sessionId == command.member.sessionId;
      });
  if (existing != members_.end()) {
    return existing->sessionGeneration == command.member.generation
               ? RoomResultCode::Ok
               : RoomResultCode::StaleSession;
  }
  if (members_.size() >= capacity_) {
    return RoomResultCode::RoomFull;
  }
  members_.push_back(RoomMemberSnapshot{
      .accountId = command.member.accountId,
      .sessionId = command.member.sessionId,
      .sessionGeneration = command.member.generation,
      .nickname = std::move(command.member.nickname),
      .ready = false,
  });
  return RoomResultCode::Ok;
}

RoomResultCode Room::leave(const LeaveRoomCommand &command) {
  const auto member = std::find_if(members_.begin(), members_.end(),
                                   [&command](const auto &item) {
                                     return item.sessionId == command.sessionId;
                                   });
  if (member == members_.end()) {
    return RoomResultCode::NotInRoom;
  }
  if (member->sessionGeneration != command.generation) {
    return RoomResultCode::StaleSession;
  }
  members_.erase(member);
  return RoomResultCode::Ok;
}

RoomResultCode Room::setReady(const SetReadyCommand &command) {
  if (lifecycle_ != RoomLifecycle::Open) {
    return RoomResultCode::RoomClosed;
  }
  const auto member = std::find_if(members_.begin(), members_.end(),
                                   [&command](const auto &item) {
                                     return item.sessionId == command.sessionId;
                                   });
  if (member == members_.end()) {
    return RoomResultCode::NotInRoom;
  }
  if (member->sessionGeneration != command.generation) {
    return RoomResultCode::StaleSession;
  }
  member->ready = command.ready;
  return RoomResultCode::Ok;
}

RoomResultCode Room::kick(const KickRoomMemberCommand &command) {
  if (lifecycle_ != RoomLifecycle::Open) {
    return RoomResultCode::RoomClosed;
  }
  const auto actor = std::find_if(
      members_.begin(), members_.end(), [&command](const auto &member) {
        return member.sessionId == command.actorSessionId;
      });
  if (actor == members_.end() || actor != members_.begin()) {
    return RoomResultCode::NotHost;
  }
  if (actor->sessionGeneration != command.actorGeneration) {
    return RoomResultCode::StaleSession;
  }
  if (command.actorSessionId == command.targetSessionId) {
    return RoomResultCode::InvalidTarget;
  }
  const auto target = std::find_if(
      members_.begin(), members_.end(), [&command](const auto &member) {
        return member.sessionId == command.targetSessionId;
      });
  if (target == members_.end() ||
      target->sessionGeneration != command.targetGeneration) {
    return RoomResultCode::InvalidTarget;
  }
  members_.erase(target);
  return RoomResultCode::Ok;
}

HostStartEligibility
Room::prepareHostStart(const HostStartEligibilityCommand &command) const {
  if (lifecycle_ != RoomLifecycle::Open) {
    return {RoomResultCode::RoomClosed, std::nullopt};
  }
  const auto actor = std::find_if(
      members_.begin(), members_.end(), [&command](const auto &member) {
        return member.sessionId == command.actorSessionId;
      });
  if (actor == members_.end() || actor != members_.begin()) {
    return {RoomResultCode::NotHost, std::nullopt};
  }
  if (actor->sessionGeneration != command.actorGeneration) {
    return {RoomResultCode::StaleSession, std::nullopt};
  }
  if (members_.size() < kMinimumCapacity) {
    return {RoomResultCode::NotEnoughPlayers, std::nullopt};
  }
  if (!std::all_of(members_.begin(), members_.end(),
                   [](const auto &member) { return member.ready; })) {
    return {RoomResultCode::NotAllReady, std::nullopt};
  }
  return {RoomResultCode::Ok, BattleAdmissionSnapshot{
                                  .roomId = roomId_,
                                  .host = members_.front().sessionId,
                                  .members = members_,
                              }};
}

RoomResultCode Room::commitLoading(const BattleAdmissionSnapshot &admission) {
  if (closed() || lifecycle_ != RoomLifecycle::Open) {
    return RoomResultCode::RoomClosed;
  }
  if (admission.roomId != roomId_ ||
      admission.host != members_.front().sessionId ||
      admission.members != members_) {
    return RoomResultCode::InvalidArgument;
  }
  lifecycle_ = RoomLifecycle::Loading;
  return RoomResultCode::Ok;
}

RoomResultCode Room::commitInProgress() {
  if (lifecycle_ == RoomLifecycle::InProgress) {
    return RoomResultCode::Ok;
  }
  if (closed() || lifecycle_ != RoomLifecycle::Loading) {
    return RoomResultCode::RoomClosed;
  }
  lifecycle_ = RoomLifecycle::InProgress;
  return RoomResultCode::Ok;
}

RoomResultCode Room::commitAwaitingSettlementDurability() {
  if (lifecycle_ == RoomLifecycle::AwaitingSettlementDurability) {
    return RoomResultCode::Ok;
  }
  if (lifecycle_ != RoomLifecycle::InProgress) {
    return RoomResultCode::RoomClosed;
  }
  lifecycle_ = RoomLifecycle::AwaitingSettlementDurability;
  return RoomResultCode::Ok;
}

RoomResultCode Room::reopenAfterLoadCancelled() {
  if (lifecycle_ == RoomLifecycle::Open) {
    return RoomResultCode::Ok;
  }
  if (lifecycle_ != RoomLifecycle::Loading) {
    return RoomResultCode::RoomClosed;
  }
  lifecycle_ = RoomLifecycle::Open;
  for (auto &member : members_) {
    member.ready = false;
  }
  return RoomResultCode::Ok;
}

RoomResultCode Room::reopenAfterSettlementDurability() {
  if (lifecycle_ == RoomLifecycle::Open) {
    return RoomResultCode::Ok;
  }
  if (lifecycle_ != RoomLifecycle::AwaitingSettlementDurability) {
    return RoomResultCode::RoomClosed;
  }
  lifecycle_ = RoomLifecycle::Open;
  for (auto &member : members_) {
    member.ready = false;
  }
  return RoomResultCode::Ok;
}

std::optional<RoomSummary> Room::summary() const {
  if (closed()) {
    return std::nullopt;
  }
  return RoomSummary{
      .roomId = roomId_,
      .title = title_,
      .memberCount = static_cast<std::uint8_t>(members_.size()),
      .capacity = capacity_,
  };
}

std::optional<RoomDetailProjection> Room::detail() const {
  if (closed()) {
    return std::nullopt;
  }
  return RoomDetailProjection{
      .roomId = roomId_,
      .lifecycle = lifecycle_,
      .title = title_,
      .capacity = capacity_,
      .hostSessionId = members_.front().sessionId,
      .hostSessionGeneration = members_.front().sessionGeneration,
      .members = members_,
  };
}

RoomLifecycle Room::lifecycle() const noexcept { return lifecycle_; }

bool Room::closed() const noexcept { return members_.empty(); }

} // namespace lol::lobby_room
