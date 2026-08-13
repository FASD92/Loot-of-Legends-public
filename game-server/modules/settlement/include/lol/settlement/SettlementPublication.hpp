#pragma once

#include <lol/settlement/SettlementIntent.hpp>

#include <cstdint>
#include <functional>
#include <variant>
#include <vector>

namespace lol::settlement {

struct DurableAppendRequest final {
  SettlementBatchId batchId;
  shared::RoomId roomId;
  shared::BattleInstanceId battleId;
  std::vector<std::vector<std::uint8_t>> canonicalIntents;
};

struct DurableAppendCompleted final {
  SettlementBatchId batchId;
  shared::RoomId roomId;
  shared::BattleInstanceId battleId;
  std::uint64_t commitSequence;
};

enum class SettlementStorageFailure : std::uint8_t {
  QueueFull,
  InvalidBatch,
  IoFailure,
};

struct DurableAppendFailed final {
  SettlementBatchId batchId;
  shared::RoomId roomId;
  shared::BattleInstanceId battleId;
  SettlementStorageFailure failure;
};

using DurableAppendOutcome =
    std::variant<DurableAppendCompleted, DurableAppendFailed>;

enum class SubmitAppendResult : std::uint8_t {
  Accepted,
  QueueFull,
  StorageUnavailable,
};

// External asynchronous storage boundary. submit() is non-blocking and must
// not invoke the completion inline; the callback carries copied correlation
// values only and never a Room/Battle aggregate pointer.
class SettlementStoragePort {
public:
  using CompletionSink = std::function<void(DurableAppendOutcome)>;

  virtual ~SettlementStoragePort() = default;
  [[nodiscard]] virtual SubmitAppendResult
  submit(DurableAppendRequest request, CompletionSink completion) = 0;
};

} // namespace lol::settlement
