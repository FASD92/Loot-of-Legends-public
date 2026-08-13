#include <lol/runtime/DeadlineScheduler.hpp>

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace lol::runtime {

class ThreadDeadlineScheduler::Lease final : public DeadlineLease {
public:
  Lease(ThreadDeadlineScheduler &owner, std::uint64_t id)
      : owner_(&owner), id_(id) {}

  ~Lease() override {
    if (owner_ != nullptr) {
      owner_->releaseLease(id_);
    }
  }

  [[nodiscard]] bool armAfter(std::chrono::milliseconds delay,
                              Task task) override {
    return owner_ != nullptr && owner_->armLease(id_, delay, std::move(task));
  }

  void cancel() noexcept override {
    if (owner_ != nullptr) {
      owner_->cancelLease(id_);
    }
  }

private:
  ThreadDeadlineScheduler *owner_;
  std::uint64_t id_;
};

ThreadDeadlineScheduler::ThreadDeadlineScheduler(
    ThreadDeadlineSchedulerConfig config)
    : queueCapacity_(config.queueCapacity) {
  if (config.queueCapacity == 0) {
    throw std::invalid_argument{
        "ThreadDeadlineScheduler queue capacity must be positive"};
  }
  thread_ = std::thread{[this] { run(); }};
}

ThreadDeadlineScheduler::~ThreadDeadlineScheduler() { stop(); }

bool ThreadDeadlineScheduler::scheduleAfter(std::chrono::milliseconds delay,
                                            Task task) {
  if (delay.count() < 0 || !task) {
    return false;
  }
  std::lock_guard lock{mutex_};
  if (stopping_ || unreservedTaskCount_ + leases_.size() >= queueCapacity_) {
    return false;
  }
  insertTaskLocked(ScheduledTask{
      .deadline = Clock::now() + delay,
      .ordinal = nextOrdinal_++,
      .leaseId = 0,
      .gate = nullptr,
      .task = std::move(task),
  });
  ++unreservedTaskCount_;
  changed_.notify_one();
  return true;
}

std::unique_ptr<DeadlineLease> ThreadDeadlineScheduler::tryReserve() {
  std::lock_guard lock{mutex_};
  if (stopping_ || unreservedTaskCount_ + leases_.size() >= queueCapacity_) {
    return nullptr;
  }
  const auto leaseId = nextLeaseId_++;
  leases_.emplace(leaseId, LeaseRecord{});
  return std::make_unique<Lease>(*this, leaseId);
}

bool ThreadDeadlineScheduler::armLease(std::uint64_t leaseId,
                                       std::chrono::milliseconds delay,
                                       Task task) {
  if (delay.count() < 0 || !task) {
    return false;
  }
  std::lock_guard lock{mutex_};
  const auto lease = leases_.find(leaseId);
  if (stopping_ || lease == leases_.end()) {
    return false;
  }
  if (lease->second.gate) {
    lease->second.gate->state.store(InvocationState::Canceled,
                                    std::memory_order_release);
  }
  eraseLeaseTaskLocked(leaseId);
  auto gate = std::make_shared<InvocationGate>();
  lease->second.gate = gate;
  insertTaskLocked(ScheduledTask{
      .deadline = Clock::now() + delay,
      .ordinal = nextOrdinal_++,
      .leaseId = leaseId,
      .gate = std::move(gate),
      .task = std::move(task),
  });
  changed_.notify_one();
  return true;
}

void ThreadDeadlineScheduler::cancelLease(std::uint64_t leaseId) noexcept {
  std::lock_guard lock{mutex_};
  const auto lease = leases_.find(leaseId);
  if (lease == leases_.end()) {
    return;
  }
  if (lease->second.gate) {
    lease->second.gate->state.store(InvocationState::Canceled,
                                    std::memory_order_release);
  }
  eraseLeaseTaskLocked(leaseId);
  lease->second.gate.reset();
  changed_.notify_one();
}

void ThreadDeadlineScheduler::releaseLease(std::uint64_t leaseId) noexcept {
  std::lock_guard lock{mutex_};
  const auto lease = leases_.find(leaseId);
  if (lease == leases_.end()) {
    return;
  }
  if (lease->second.gate) {
    lease->second.gate->state.store(InvocationState::Canceled,
                                    std::memory_order_release);
  }
  eraseLeaseTaskLocked(leaseId);
  leases_.erase(lease);
  changed_.notify_one();
}

void ThreadDeadlineScheduler::eraseLeaseTaskLocked(std::uint64_t leaseId) {
  std::erase_if(tasks_, [leaseId](const ScheduledTask &task) {
    if (task.leaseId != leaseId) {
      return false;
    }
    if (task.gate) {
      task.gate->state.store(InvocationState::Canceled,
                             std::memory_order_release);
    }
    return true;
  });
}

void ThreadDeadlineScheduler::insertTaskLocked(ScheduledTask task) {
  const auto position = std::upper_bound(
      tasks_.begin(), tasks_.end(), task,
      [](const ScheduledTask &left, const ScheduledTask &right) {
        return std::tie(left.deadline, left.ordinal) <
               std::tie(right.deadline, right.ordinal);
      });
  tasks_.insert(position, std::move(task));
}

void ThreadDeadlineScheduler::stop() {
  std::lock_guard stopLock{stopMutex_};
  if (!thread_.joinable()) {
    return;
  }
  {
    std::lock_guard lock{mutex_};
    stopping_ = true;
    for (auto &[leaseId, lease] : leases_) {
      static_cast<void>(leaseId);
      if (lease.gate) {
        lease.gate->state.store(InvocationState::Canceled,
                                std::memory_order_release);
        lease.gate.reset();
      }
    }
    tasks_.clear();
    unreservedTaskCount_ = 0;
  }
  changed_.notify_one();
  thread_.join();
}

void ThreadDeadlineScheduler::run() {
  while (true) {
    Task task;
    {
      std::unique_lock lock{mutex_};
      while (!stopping_ && tasks_.empty()) {
        changed_.wait(lock);
      }
      if (stopping_) {
        return;
      }

      const auto deadline = tasks_.front().deadline;
      changed_.wait_until(lock, deadline, [this, deadline] {
        return stopping_ || tasks_.empty() ||
               tasks_.front().deadline < deadline;
      });
      if (stopping_) {
        return;
      }
      if (tasks_.empty() || tasks_.front().deadline > Clock::now()) {
        continue;
      }
      auto scheduled = std::move(tasks_.front());
      tasks_.erase(tasks_.begin());
      if (scheduled.leaseId == 0) {
        --unreservedTaskCount_;
        task = std::move(scheduled.task);
      } else {
        const auto lease = leases_.find(scheduled.leaseId);
        if (lease != leases_.end() && lease->second.gate == scheduled.gate) {
          auto expected = InvocationState::Pending;
          if (scheduled.gate->state.compare_exchange_strong(
                  expected, InvocationState::Running,
                  std::memory_order_acq_rel)) {
            lease->second.gate.reset();
            task = std::move(scheduled.task);
          }
        }
      }
    }

    if (!task) {
      continue;
    }
    try {
      task();
    } catch (...) {
      std::terminate();
    }
  }
}

} // namespace lol::runtime
