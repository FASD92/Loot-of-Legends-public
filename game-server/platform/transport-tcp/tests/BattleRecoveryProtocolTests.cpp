#include <lol/transport/tcp/BattleRecoveryProtocol.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <vector>

namespace {

using lol::transport::tcp::BattleRecoveryNotice;
using lol::transport::tcp::BattleRecoveryProtocolCodec;
using lol::transport::tcp::BattleRecoveryReason;

std::vector<std::byte> bytes(const std::initializer_list<std::uint8_t> values) {
  std::vector<std::byte> result;
  result.reserve(values.size());
  for (const auto value : values) {
    result.push_back(static_cast<std::byte>(value));
  }
  return result;
}

bool exactGoldenVectors() {
  const auto resultFailure =
      BattleRecoveryProtocolCodec::encodeServerFrame(BattleRecoveryNotice{
          .roomId = 7u,
          .battleInstanceId = 9u,
          .reason = BattleRecoveryReason::ResultGenerationFailed,
      });
  const auto storagePending =
      BattleRecoveryProtocolCodec::encodeServerFrame(BattleRecoveryNotice{
          .roomId = 7u,
          .battleInstanceId = 10u,
          .reason = BattleRecoveryReason::SettlementRecoveryPending,
      });
  return resultFailure ==
             bytes({0x00, 0x00, 0x00, 0x16, 0x01, 0x00, 0x00, 0x00, 0x25,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 0x01}) &&
         storagePending ==
             bytes({0x00, 0x00, 0x00, 0x16, 0x01, 0x00, 0x00, 0x00, 0x25,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0a, 0x02});
}

bool invalidValuesAreRejected() {
  const auto encode = [](std::uint64_t roomId, std::uint64_t battleId,
                         BattleRecoveryReason reason) {
    return BattleRecoveryProtocolCodec::encodeServerFrame(BattleRecoveryNotice{
        .roomId = roomId,
        .battleInstanceId = battleId,
        .reason = reason,
    });
  };
  // Exercise the codec's defensive validation with a value that cannot be
  // produced by well-typed callers.
  // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
  const auto invalidReason = static_cast<BattleRecoveryReason>(3u);
  return !encode(0u, 9u, BattleRecoveryReason::ResultGenerationFailed)
              .has_value() &&
         !encode(7u, 0u, BattleRecoveryReason::ResultGenerationFailed)
              .has_value() &&
         !encode(7u, 9u, invalidReason).has_value();
}

} // namespace

int main() {
  return exactGoldenVectors() && invalidValuesAreRejected() ? EXIT_SUCCESS
                                                            : EXIT_FAILURE;
}
