#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace lol::runtime {

struct WorkerPoolConfig final {
  std::size_t threadCount;
  std::size_t queueCapacity;
};

struct WorkerPoolMetrics final {
  std::size_t pendingTasks;
  std::size_t activeTasks;
  std::uint64_t acceptedTasks;
  std::uint64_t rejectedTasks;
  std::uint64_t completedTasks;
};

class WorkerPool final {
public:
  using Task = std::function<void()>;

  explicit WorkerPool(WorkerPoolConfig config);
  ~WorkerPool();

  WorkerPool(const WorkerPool &) = delete;
  WorkerPool &operator=(const WorkerPool &) = delete;

  [[nodiscard]] bool submit(Task task);
  void stop();
  [[nodiscard]] bool waitUntilIdle(std::chrono::milliseconds timeout);
  [[nodiscard]] WorkerPoolMetrics metrics() const;

private:
  void workerLoop();

  const std::size_t queueCapacity_;
  mutable std::mutex mutex_;
  std::mutex stopMutex_;
  std::condition_variable workAvailable_;
  std::condition_variable idle_;
  std::deque<Task> tasks_;
  std::vector<std::thread> workers_;
  std::size_t activeTasks_{0};
  std::uint64_t acceptedTasks_{0};
  std::uint64_t rejectedTasks_{0};
  std::uint64_t completedTasks_{0};
  bool stopping_{false};
};

} // namespace lol::runtime
