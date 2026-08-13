#pragma once

#include <lol/game_flow/RoomCommandGateway.hpp>
#include <lol/settlement/SettlementCapacityGate.hpp>

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>

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
