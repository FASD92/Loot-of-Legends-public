#include <lol/observability/GameMetrics.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>

#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

namespace lol::observability {
namespace {

double milliseconds(std::chrono::nanoseconds duration) {
  return std::chrono::duration<double, std::milli>{duration}.count();
}

std::optional<std::chrono::nanoseconds> processCpuTime() {
  timespec value{};
  if (::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &value) != 0 ||
      value.tv_sec < 0 || value.tv_nsec < 0) {
    return std::nullopt;
  }
  return std::chrono::seconds{value.tv_sec} +
         std::chrono::nanoseconds{value.tv_nsec};
}

std::optional<double> residentBytes() {
#if defined(__linux__)
  std::ifstream input{"/proc/self/statm"};
  std::uint64_t totalPages{};
  std::uint64_t residentPages{};
  const long pageBytes = ::sysconf(_SC_PAGESIZE);
  if (!(input >> totalPages >> residentPages) || pageBytes <= 0) {
    return std::nullopt;
  }
  static_cast<void>(totalPages);
  return static_cast<double>(residentPages) * static_cast<double>(pageBytes);
#elif defined(__APPLE__)
  rusage usage{};
  if (::getrusage(RUSAGE_SELF, &usage) != 0 || usage.ru_maxrss < 0) {
    return std::nullopt;
  }
  return static_cast<double>(usage.ru_maxrss);
#else
  return std::nullopt;
#endif
}

std::optional<double> openFileDescriptors() {
#if defined(__linux__)
  constexpr auto path = "/proc/self/fd";
#elif defined(__APPLE__)
  constexpr auto path = "/dev/fd";
#else
  return std::nullopt;
#endif
  std::error_code error;
  std::size_t count{};
  for (std::filesystem::directory_iterator entries{path, error}, end;
       !error && entries != end; entries.increment(error)) {
    ++count;
  }
  return error ? std::nullopt
               : std::optional<double>{static_cast<double>(count)};
}

} // namespace

GameMetrics::GameMetrics(std::size_t allocatedCpuCount)
    : allocatedCpuCount_(allocatedCpuCount) {
  if (allocatedCpuCount_ == 0u) {
    throw std::invalid_argument("allocated CPU count must be positive");
  }
}

void GameMetrics::recordSnapshot(
    std::chrono::steady_clock::time_point capturedAt) {
  std::lock_guard lock{mutex_};
  if (lastSnapshotAt_.has_value() && capturedAt >= *lastSnapshotAt_) {
    latestSnapshotInterval_ = capturedAt - *lastSnapshotAt_;
  }
  lastSnapshotAt_ = capturedAt;
}

void GameMetrics::recordDurableAppend(std::chrono::nanoseconds latency) {
  if (latency < std::chrono::nanoseconds::zero()) {
    return;
  }
  std::lock_guard lock{mutex_};
  latestDurableAppend_ = latency;
}

GameMetricSnapshot
GameMetrics::snapshot(const game_flow::RoomExecutionObservation &room,
                      const settlement::SettlementCapacityMetrics &settlement) {
  const auto now = std::chrono::steady_clock::now();
  const auto cpuTime = processCpuTime();
  std::lock_guard lock{mutex_};

  std::optional<double> cpuBusy;
  if (cpuTime.has_value() && lastProcessCpuTime_.has_value() &&
      lastResourceSampleAt_.has_value() && now > *lastResourceSampleAt_ &&
      *cpuTime >= *lastProcessCpuTime_) {
    const auto wall = now - *lastResourceSampleAt_;
    cpuBusy =
        std::chrono::duration<double>{*cpuTime - *lastProcessCpuTime_}.count() /
        std::chrono::duration<double>{wall}.count() /
        static_cast<double>(allocatedCpuCount_);
  }
  lastProcessCpuTime_ = cpuTime;
  lastResourceSampleAt_ = now;

  const auto pendingRecords = settlement.backlog.unretiredRecords;
  if (pendingRecords > 0u && !outboxPendingSince_.has_value()) {
    outboxPendingSince_ = now;
  } else if (pendingRecords == 0u && outboxPendingSince_.has_value()) {
    latestOutboxDrain_ = now - *outboxPendingSince_;
    outboxPendingSince_.reset();
  } else if (pendingRecords == 0u && !latestOutboxDrain_.has_value()) {
    latestOutboxDrain_ = std::chrono::nanoseconds::zero();
  }

  const bool roomSampled = room.gameplayProgressTotal > 0u;
  return GameMetricSnapshot{
      .roomQueueDelayMs =
          roomSampled ? std::optional{milliseconds(room.latestQueueDelay)}
                      : std::nullopt,
      .roomProcessingDurationMs =
          roomSampled
              ? std::optional{milliseconds(room.latestProcessingDuration)}
              : std::nullopt,
      .criticalTerminalLatencyMs =
          room.latestCriticalTerminalLatency.has_value()
              ? std::optional{milliseconds(*room.latestCriticalTerminalLatency)}
              : std::nullopt,
      .activeSnapshotIntervalMs =
          latestSnapshotInterval_.has_value()
              ? std::optional{milliseconds(*latestSnapshotInterval_)}
              : std::nullopt,
      .durableAppendLatencyMs =
          latestDurableAppend_.has_value()
              ? std::optional{milliseconds(*latestDurableAppend_)}
              : std::nullopt,
      .outboxBacklogRecords = static_cast<double>(pendingRecords),
      .outboxOldestPendingMs =
          static_cast<double>(settlement.backlog.oldestPendingAge.count()),
      .outboxDrainMs = latestOutboxDrain_.has_value()
                           ? std::optional{milliseconds(*latestOutboxDrain_)}
                           : std::nullopt,
      .gameplayProgressTotal = static_cast<double>(room.gameplayProgressTotal),
      .processCpuBusyRatio = cpuBusy,
      .processRssBytes = residentBytes(),
      .processFdCount = openFileDescriptors(),
      .serverInvariantTotal = static_cast<double>(room.serverInvariantTotal),
  };
}

} // namespace lol::observability
