#include <lol/runtime/DeadlineScheduler.hpp>

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>

namespace {

using namespace std::chrono_literals;
using lol::runtime::ThreadDeadlineScheduler;
using lol::runtime::ThreadDeadlineSchedulerConfig;

bool boundedSchedulerRunsDueTaskAndStops() {
  ThreadDeadlineScheduler scheduler{
      ThreadDeadlineSchedulerConfig{.queueCapacity = 1}};
  std::mutex mutex;
  std::condition_variable changed;
  bool ran = false;
  if (!scheduler.scheduleAfter(20ms,
                               [&] {
                                 std::lock_guard lock{mutex};
                                 ran = true;
                                 changed.notify_all();
                               }) ||
      scheduler.scheduleAfter(20ms, [] {})) {
    return false;
  }
  {
    std::unique_lock lock{mutex};
    if (!changed.wait_for(lock, 2s, [&] { return ran; })) {
      return false;
    }
  }
  scheduler.stop();
  scheduler.stop();
  return !scheduler.scheduleAfter(0ms, [] {});
}

bool reservedLeaseRearmsOneTaskAndReturnsCapacityOnRelease() {
  ThreadDeadlineScheduler scheduler{
      ThreadDeadlineSchedulerConfig{.queueCapacity = 1}};
  std::mutex mutex;
  std::condition_variable changed;
  std::size_t runs = 0;

  auto lease = scheduler.tryReserve();
  if (!lease || scheduler.tryReserve() || scheduler.scheduleAfter(0ms, [] {}) ||
      !lease->armAfter(1h,
                       [&] {
                         std::lock_guard lock{mutex};
                         runs += 100;
                         changed.notify_all();
                       }) ||
      !lease->armAfter(0ms, [&] {
        std::lock_guard lock{mutex};
        ++runs;
        changed.notify_all();
      })) {
    return false;
  }
  {
    std::unique_lock lock{mutex};
    if (!changed.wait_for(lock, 2s, [&] { return runs == 1; })) {
      return false;
    }
  }

  lease->cancel();
  if (scheduler.scheduleAfter(0ms, [] {})) {
    return false;
  }
  lease.reset();
  if (!scheduler.scheduleAfter(0ms, [&] {
        std::lock_guard lock{mutex};
        ++runs;
        changed.notify_all();
      })) {
    return false;
  }
  {
    std::unique_lock lock{mutex};
    if (!changed.wait_for(lock, 2s, [&] { return runs == 2; })) {
      return false;
    }
  }
  scheduler.stop();
  return runs == 2;
}

} // namespace

int main() {
  return boundedSchedulerRunsDueTaskAndStops() &&
                 reservedLeaseRearmsOneTaskAndReturnsCapacityOnRelease()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
