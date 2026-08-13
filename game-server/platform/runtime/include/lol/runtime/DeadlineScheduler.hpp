#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lol::runtime {

class DeadlineLease {
public:
  using Task = std::function<void()>;

  virtual ~DeadlineLease() = default;

  [[nodiscard]] virtual bool armAfter(std::chrono::milliseconds delay,
                                      Task task) = 0;
  virtual void cancel() noexcept = 0;
};

class DeadlineScheduler {
public:
  using Task = DeadlineLease::Task;

  virtual ~DeadlineScheduler() = default;

  [[nodiscard]] virtual bool scheduleAfter(std::chrono::milliseconds delay,
                                           Task task) = 0;
  [[nodiscard]] virtual std::unique_ptr<DeadlineLease> tryReserve() = 0;
};

struct ThreadDeadlineSchedulerConfig final {
  std::size_t queueCapacity;
};

class ThreadDeadlineScheduler final : public DeadlineScheduler {
public:
  explicit ThreadDeadlineScheduler(ThreadDeadlineSchedulerConfig config);
  ~ThreadDeadlineScheduler() override;

  ThreadDeadlineScheduler(const ThreadDeadlineScheduler &) = delete;
  ThreadDeadlineScheduler &operator=(const ThreadDeadlineScheduler &) = delete;

  [[nodiscard]] bool scheduleAfter(std::chrono::milliseconds delay,
                                   Task task) override;
  [[nodiscard]] std::unique_ptr<DeadlineLease> tryReserve() override;
  void stop();

private:
  using Clock = std::chrono::steady_clock;

  class Lease;

  enum class InvocationState : std::uint8_t {
    Pending,
    Running,
    Canceled,
  };

  struct InvocationGate final {
    std::atomic<InvocationState> state{InvocationState::Pending};
  };

  struct ScheduledTask final {
    Clock::time_point deadline;
    std::uint64_t ordinal;
    std::uint64_t leaseId;
    std::shared_ptr<InvocationGate> gate;
    Task task;
  };

  struct LeaseRecord final {
    std::shared_ptr<InvocationGate> gate;
  };

  void run();
  [[nodiscard]] bool armLease(std::uint64_t leaseId,
                              std::chrono::milliseconds delay, Task task);
  void cancelLease(std::uint64_t leaseId) noexcept;
  void releaseLease(std::uint64_t leaseId) noexcept;
  void eraseLeaseTaskLocked(std::uint64_t leaseId);
  void insertTaskLocked(ScheduledTask task);

  const std::size_t queueCapacity_;
  std::mutex mutex_;
  std::mutex stopMutex_;
  std::condition_variable changed_;
  std::vector<ScheduledTask> tasks_;
  std::unordered_map<std::uint64_t, LeaseRecord> leases_;
  std::thread thread_;
  std::uint64_t nextOrdinal_{1};
  std::uint64_t nextLeaseId_{1};
  std::size_t unreservedTaskCount_{0};
  bool stopping_{false};
};

} // namespace lol::runtime
