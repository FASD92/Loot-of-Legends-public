#pragma once

#include <lol/game_flow/RoomCommandGateway.hpp>
#include <lol/settlement/SettlementCapacityGate.hpp>

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

namespace lol::observability {

struct GameMetricSnapshot final {
  std::optional<double> roomQueueDelayMs;
  std::optional<double> roomProcessingDurationMs;
  std::optional<double> criticalTerminalLatencyMs;
  std::optional<double> activeSnapshotIntervalMs;
  std::optional<double> durableAppendLatencyMs;
  std::optional<double> outboxBacklogRecords;
  std::optional<double> outboxOldestPendingMs;
  std::optional<double> outboxDrainMs;
  std::optional<double> gameplayProgressTotal;
  std::optional<double> processCpuBusyRatio;
  std::optional<double> processRssBytes;
  std::optional<double> processFdCount;
  std::optional<double> serverInvariantTotal;
  std::optional<double> rudpAcceptedSamples{};
  std::optional<double> rudpRetransmittedSamples{};
  std::optional<double> rudpUnconfirmedSamples{};
  std::optional<double> rudpNonpositiveSamples{};
  std::optional<double> rudpExpiredSamples{};
  std::optional<double> rudpStaleSamples{};
  std::optional<double> rudpCoalescedSamples{};
  std::optional<double> rudpNoNewEntryAcks{};
  std::optional<double> rudpRetransmissions{};
  std::optional<double> rudpExpiries{};
  std::optional<double> rudpSendFailures{};
  std::vector<double> rudpSrttMs{};
  std::vector<double> rudpRttvarMs{};
  std::vector<double> rudpBaseRtoMs{};
  std::optional<double> rudpRecoveryEntered{};
  std::optional<double> rudpRecoveryEscalated{};
  std::optional<double> rudpRecoveryReset{};
  std::optional<double> rudpRecoveryStaleTimeouts{};
  std::optional<double> rudpRecoveryActive{};
  std::vector<double> rudpRecoveryFloorMs{};
  std::vector<double> rudpInitialRtoMs{};
  std::vector<double> rudpEffectiveRtoMs{};
};

class GameMetrics final {
public:
  explicit GameMetrics(std::size_t allocatedCpuCount);

  void recordSnapshot(std::chrono::steady_clock::time_point capturedAt);
  void recordDurableAppend(std::chrono::nanoseconds latency);
  [[nodiscard]] GameMetricSnapshot
  snapshot(const game_flow::RoomExecutionObservation &room,
           const settlement::SettlementCapacityMetrics &settlement);

private:
  std::size_t allocatedCpuCount_;
  std::mutex mutex_;
  std::optional<std::chrono::steady_clock::time_point> lastSnapshotAt_;
  std::optional<std::chrono::nanoseconds> latestSnapshotInterval_;
  std::optional<std::chrono::nanoseconds> latestDurableAppend_;
  std::optional<std::chrono::steady_clock::time_point> lastResourceSampleAt_;
  std::optional<std::chrono::nanoseconds> lastProcessCpuTime_;
  std::optional<std::chrono::steady_clock::time_point> outboxPendingSince_;
  std::optional<std::chrono::nanoseconds> latestOutboxDrain_;
};

} // namespace lol::observability
