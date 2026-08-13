#include <lol/transport/tcp/FinalResultProtocol.hpp>

#include "ProtocolWire.hpp"

#include <algorithm>
#include <limits>
#include <set>

namespace lol::transport::tcp {
namespace {

constexpr std::uint8_t kProtocolMajor = 1;
constexpr std::uint32_t kFinalResultMessageId = 36;
constexpr std::size_t kMinimumEntries = 2;
constexpr std::size_t kMaximumEntries = 10;

bool validEntry(const FinalResultEntry &entry, FinalResultOutcome outcome) {
  if (entry.sessionId == 0 || entry.nickname.empty() ||
      entry.nickname.size() > std::numeric_limits<std::uint16_t>::max() ||
      !wire::validUtf8(entry.nickname)) {
    return false;
  }
  switch (entry.exitStatus) {
  case FinalResultExitStatus::TerminalPresent:
  case FinalResultExitStatus::TerminalExited:
    break;
  default:
    return false;
  }
  if (outcome == FinalResultOutcome::MonsterDefeated) {
    return entry.rank != 0 && entry.isTop == (entry.rank == 1);
  }
  return entry.rank == 0 && !entry.isTop;
}

bool valid(const FinalResult &result) {
  if (result.roomId == 0 || result.battleInstanceId == 0 ||
      result.entries.size() < kMinimumEntries ||
      result.entries.size() > kMaximumEntries) {
    return false;
  }
  switch (result.outcome) {
  case FinalResultOutcome::MonsterDefeated:
  case FinalResultOutcome::CombatTimeout:
  case FinalResultOutcome::CancelledNoActiveParticipants:
    break;
  default:
    return false;
  }
  std::set<std::uint64_t> sessions;
  return std::ranges::all_of(result.entries, [&](const auto &entry) {
    return validEntry(entry, result.outcome) &&
           sessions.insert(entry.sessionId).second;
  });
}

} // namespace

std::optional<std::vector<std::byte>>
FinalResultProtocolCodec::encodeServerFrame(const FinalResult &result) {
  if (!valid(result)) {
    return std::nullopt;
  }

  wire::Writer payload;
  payload.uint8(kProtocolMajor);
  payload.uint32(kFinalResultMessageId);
  payload.uint64(result.roomId);
  payload.uint64(result.battleInstanceId);
  payload.uint8(static_cast<std::uint8_t>(result.outcome));
  payload.uint16(static_cast<std::uint16_t>(result.entries.size()));
  for (const auto &entry : result.entries) {
    payload.uint64(entry.sessionId);
    payload.uint16(static_cast<std::uint16_t>(entry.nickname.size()));
    payload.text(entry.nickname);
    payload.uint8(static_cast<std::uint8_t>(entry.exitStatus));
    payload.uint64(entry.finalAssetValue);
    payload.uint32(entry.rank);
    payload.uint8(entry.isTop ? 1u : 0u);
  }
  if (payload.bytes().size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  wire::Writer frame;
  frame.uint32(static_cast<std::uint32_t>(payload.bytes().size()));
  for (const auto byte : payload.bytes()) {
    frame.uint8(std::to_integer<std::uint8_t>(byte));
  }
  return frame.take();
}

} // namespace lol::transport::tcp
