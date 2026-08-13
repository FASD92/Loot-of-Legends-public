#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lol::transport::tcp {

enum class BattleRecoveryReason : std::uint8_t {
  ResultGenerationFailed = 1,
  SettlementRecoveryPending = 2,
};

struct BattleRecoveryNotice final {
  std::uint64_t roomId;
  std::uint64_t battleInstanceId;
  BattleRecoveryReason reason;

  bool operator==(const BattleRecoveryNotice &) const = default;
};

class BattleRecoveryProtocolCodec final {
public:
  [[nodiscard]] static std::optional<std::vector<std::byte>>
  encodeServerFrame(const BattleRecoveryNotice &notice);
};

} // namespace lol::transport::tcp
