#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lol::transport::tcp {

enum class FinalResultOutcome : std::uint8_t {
  MonsterDefeated = 1,
  CombatTimeout = 2,
  CancelledNoActiveParticipants = 3,
};

enum class FinalResultExitStatus : std::uint8_t {
  TerminalPresent = 1,
  TerminalExited = 2,
};

struct FinalResultEntry final {
  std::uint64_t sessionId;
  std::string nickname;
  FinalResultExitStatus exitStatus;
  std::uint64_t finalAssetValue;
  // Zero is the canonical wire representation of no rank.
  std::uint32_t rank;
  bool isTop;

  bool operator==(const FinalResultEntry &) const = default;
};

struct FinalResult final {
  std::uint64_t roomId;
  std::uint64_t battleInstanceId;
  FinalResultOutcome outcome;
  std::vector<FinalResultEntry> entries;

  bool operator==(const FinalResult &) const = default;
};

class FinalResultProtocolCodec final {
public:
  [[nodiscard]] static std::optional<std::vector<std::byte>>
  encodeServerFrame(const FinalResult &result);
};

} // namespace lol::transport::tcp
