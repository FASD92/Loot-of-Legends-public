#include <lol/transport/tcp/BattleRecoveryProtocol.hpp>

#include "ProtocolWire.hpp"

namespace lol::transport::tcp {
namespace {

constexpr std::uint8_t kProtocolMajor = 1;
constexpr std::uint32_t kBattleRecoveryNoticeMessageId = 37;

bool valid(BattleRecoveryReason reason) noexcept {
  return reason == BattleRecoveryReason::ResultGenerationFailed ||
         reason == BattleRecoveryReason::SettlementRecoveryPending;
}

} // namespace

std::optional<std::vector<std::byte>>
BattleRecoveryProtocolCodec::encodeServerFrame(
    const BattleRecoveryNotice &notice) {
  if (notice.roomId == 0u || notice.battleInstanceId == 0u ||
      !valid(notice.reason)) {
    return std::nullopt;
  }

  wire::Writer payload;
  payload.uint8(kProtocolMajor);
  payload.uint32(kBattleRecoveryNoticeMessageId);
  payload.uint64(notice.roomId);
  payload.uint64(notice.battleInstanceId);
  payload.uint8(static_cast<std::uint8_t>(notice.reason));

  wire::Writer frame;
  frame.uint32(static_cast<std::uint32_t>(payload.bytes().size()));
  for (const auto byte : payload.bytes()) {
    frame.uint8(std::to_integer<std::uint8_t>(byte));
  }
  return frame.take();
}

} // namespace lol::transport::tcp
