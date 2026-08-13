#include <lol/runtime/WorkerPool.hpp>

#include <exception>
#include <stdexcept>
#include <utility>

namespace lol::runtime {

WorkerPool::WorkerPool(WorkerPoolConfig config)
    : queueCapacity_(config.queueCapacity) {
  if (config.threadCount == 0 || config.queueCapacity == 0) {
    throw std::invalid_argument(
        "WorkerPool thread count and queue capacity must be positive");
  }

  workers_.reserve(config.threadCount);
  try {
    for (std::size_t index = 0; index < config.threadCount; ++index) {
      workers_.emplace_back([this] { workerLoop(); });
    }
  } catch (...) {
    {
      std::lock_guard lock{mutex_};
      stopping_ = true;
    }
    workAvailable_.notify_all();
    for (auto &worker : workers_) {
      worker.join();
    }
    throw;
  }
}

WorkerPool::~WorkerPool() { stop(); }

bool WorkerPool::submit(Task task) {
  std::lock_guard lock{mutex_};
  if (!task || stopping_ || tasks_.size() >= queueCapacity_) {
    ++rejectedTasks_;
    return false;
  }
  tasks_.push_back(std::move(task));
  ++acceptedTasks_;
  workAvailable_.notify_one();
  return true;
}

void WorkerPool::stop() {
  std::lock_guard stopLock{stopMutex_};
  if (workers_.empty()) {
    return;
  }
  {
    std::lock_guard lock{mutex_};
    stopping_ = true;
  }
  workAvailable_.notify_all();
  for (auto &worker : workers_) {
    worker.join();
  }
  workers_.clear();
}

bool WorkerPool::waitUntilIdle(std::chrono::milliseconds timeout) {
  std::unique_lock lock{mutex_};
  return idle_.wait_for(lock, timeout,
                        [this] { return tasks_.empty() && activeTasks_ == 0; });
}

WorkerPoolMetrics WorkerPool::metrics() const {
  std::lock_guard lock{mutex_};
  return WorkerPoolMetrics{
      .pendingTasks = tasks_.size(),
      .activeTasks = activeTasks_,
      .acceptedTasks = acceptedTasks_,
      .rejectedTasks = rejectedTasks_,
      .completedTasks = completedTasks_,
  };
}

void WorkerPool::workerLoop() {
  while (true) {
    Task task;
    {
      std::unique_lock lock{mutex_};
      workAvailable_.wait(lock,
                          [this] { return stopping_ || !tasks_.empty(); });
      if (tasks_.empty()) {
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
      ++activeTasks_;
    }

    try {
      task();
    } catch (...) {
      std::terminate();
    }

    // Release the worker-local Task (and its captures) immediately after it
    // returns, outside mutex_, before idle is observable: waitUntilIdle()
    // returning true must mean every completed task has been destroyed.
    task = Task{};

    {
      std::lock_guard lock{mutex_};
      --activeTasks_;
      ++completedTasks_;
      if (tasks_.empty() && activeTasks_ == 0) {
        idle_.notify_all();
      }
    }
  }
}

} // namespace lol::runtime
