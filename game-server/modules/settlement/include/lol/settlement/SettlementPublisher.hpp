#pragma once

#include <lol/settlement/SettlementIntent.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lol::settlement {

struct DurableSettlementIntent final {
  std::vector<std::uint8_t> canonicalPayload;
};

struct DurableSettlementBatch final {
  SettlementBatchId batchId;
  std::uint64_t commitSequence;
  std::vector<DurableSettlementIntent> intents;
};

enum class OutboxLoadStatus : std::uint8_t { Loaded, Empty, Unavailable };

struct OutboxLoadResult final {
  OutboxLoadStatus status{OutboxLoadStatus::Unavailable};
  std::optional<DurableSettlementBatch> batch;
};

enum class OutboxRetireResult : std::uint8_t {
  Retired,
  AlreadyRetired,
  NotFound,
  Unavailable,
};

class SettlementOutboxPort {
public:
  virtual ~SettlementOutboxPort() = default;

  [[nodiscard]] virtual OutboxLoadResult nextUnretired() = 0;
  [[nodiscard]] virtual OutboxRetireResult
  retire(const SettlementBatchId &batchId, std::uint64_t commitSequence) = 0;
  [[nodiscard]] virtual bool compact() = 0;
};

enum class MetaPublishOutcome : std::uint8_t {
  AcceptedPending,
  Applied,
  ResponseLost,
  Retryable,
  Conflict,
  Rejected,
};

enum class MetaStatusOutcome : std::uint8_t {
  AcceptedPending,
  Applied,
  NotFound,
  Retryable,
  Rejected,
};

class SettlementMetaPort {
public:
  virtual ~SettlementMetaPort() = default;

  [[nodiscard]] virtual MetaPublishOutcome
  publish(const DurableSettlementIntent &intent) = 0;
  [[nodiscard]] virtual MetaStatusOutcome
  status(const DurableSettlementIntent &intent) = 0;
};

enum class PublisherStepCode : std::uint8_t {
  Idle,
  Waiting,
  Progress,
  Retrying,
  Retired,
  Blocked,
};

struct PublisherStepResult final {
  PublisherStepCode code{PublisherStepCode::Idle};
  std::chrono::milliseconds retryAfter{};
};

// A single publisher driver. The caller owns scheduling and invokes step()
// from a non-Room worker. Only committed/unretired batches supplied by the
// outbox port are visible here.
class SettlementPublisher final {
public:
  SettlementPublisher(SettlementOutboxPort &outbox,
                      SettlementMetaPort &meta) noexcept;

  [[nodiscard]] PublisherStepResult
  step(std::chrono::steady_clock::time_point now);

private:
  enum class IntentPhase : std::uint8_t { Publish, ReconcileStatus };

  [[nodiscard]] PublisherStepResult
  scheduleRetry(std::chrono::steady_clock::time_point now);
  [[nodiscard]] PublisherStepResult advanceIntent() noexcept;
  [[nodiscard]] std::chrono::milliseconds retryDelay() const noexcept;

  SettlementOutboxPort &outbox_;
  SettlementMetaPort &meta_;
  std::optional<DurableSettlementBatch> batch_;
  std::size_t intentIndex_{};
  std::size_t retryAttempt_{};
  IntentPhase phase_{IntentPhase::Publish};
  std::chrono::steady_clock::time_point nextAttempt_{};
  bool waiting_{};
  bool blocked_{};
  bool compactionPending_{};
};

} // namespace lol::settlement
