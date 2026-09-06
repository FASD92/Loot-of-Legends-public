#pragma once

#include <lol/battle_continuity/FlightRecorder.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <variant>

namespace lol::battle_continuity {

enum class DurableTickSubmitResult : std::uint8_t {
  Accepted,
  QueueFull,
  StaleWriterEpoch,
  InvalidRequest,
  Stopped,
};

enum class DurableTickWriteFailure : std::uint8_t {
  IoFailure,
  QueueFull,
  StaleWriterEpoch,
  InvalidBatch,
  Stopped,
};

struct DurableTickWriteRequest final {
  RecordedTickBatch batch;
  // Present only for the initial batch. The platform adapter encrypts these
  // canonical private bytes before making BattleStart durable.
  std::optional<Bytes> privateEnvelopePlaintext;
};

struct DurableTickCommitted final {
  BattleIdentity identity;
  std::uint32_t writerRecoveryEpoch;
  std::uint64_t lastRecordSequence;
};

struct DurableTickWriteFailed final {
  BattleIdentity identity;
  std::uint32_t writerRecoveryEpoch;
  std::uint64_t lastRecordSequence;
  DurableTickWriteFailure failure;
};

using DurableTickWriteOutcome =
    std::variant<DurableTickCommitted, DurableTickWriteFailed>;

// The semantic caller and the platform implementation both need this one
// asynchronous seam; no file descriptor or storage policy crosses it.
class DurableTickWritePort {
public:
  using CompletionSink = std::function<void(DurableTickWriteOutcome)>;

  virtual ~DurableTickWritePort() = default;
  [[nodiscard]] virtual DurableTickSubmitResult
  submit(DurableTickWriteRequest request, CompletionSink completion) = 0;
};

} // namespace lol::battle_continuity
