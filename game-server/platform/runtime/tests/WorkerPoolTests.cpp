#include <lol/runtime/WorkerPool.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace {

bool fail(const char *message) {
  std::printf("  reason: %s\n", message);
  return false;
}

// Deterministic barrier: a worker signals open() when it reaches a known
// point, and the main thread blocks in wait() until then. The wait is
// bounded so a broken test fails loudly instead of hanging; the handshake
// itself is the race proof, never the timeout.
class Gate {
public:
  void open() {
    {
      std::lock_guard lock{mutex_};
      open_ = true;
    }
    cv_.notify_all();
  }

  bool wait(std::chrono::milliseconds timeout) const {
    std::unique_lock lock{mutex_};
    return cv_.wait_for(lock, timeout, [this] { return open_; });
  }

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  bool open_{false};
};

// Generous bound for every handshake wait. It only converts a broken test
// into a loud failure; a timed-out wait is never counted as completion.
constexpr auto kHandshakeTimeout = std::chrono::seconds(10);

// A copyable callable carrying a lifetime-sentinel shared_ptr. The body opens
// `started` and then waits on `release`: the caller clears every caller-side
// strong reference only after `started`, so when the body exits the only
// remaining strong reference is the worker-local copy the pool must destroy
// before idle becomes observable.
class SentinelHolder {
public:
  SentinelHolder(std::shared_ptr<int> sentinel, Gate *started, Gate *release)
      : sentinel_(std::move(sentinel)), started_(started), release_(release) {}

  SentinelHolder(const SentinelHolder &) = default;
  SentinelHolder(SentinelHolder &&) = default;
  SentinelHolder &operator=(const SentinelHolder &) = default;
  SentinelHolder &operator=(SentinelHolder &&) = default;

  void operator()() {
    started_->open();
    release_->wait(kHandshakeTimeout);
  }

private:
  std::shared_ptr<int> sentinel_;
  Gate *started_;
  Gate *release_;
};

// Quiescence contract: after waitUntilIdle() returns true, every
// completed worker-local Task callable has been destroyed and its captures
// released. The sentinel weak_ptr must be expired immediately after return.
bool testQuiescenceReleasesCompletedCaptures() {
  Gate started;
  Gate release;

  lol::runtime::WorkerPool pool{{.threadCount = 1, .queueCapacity = 8}};

  // 1. A Task captures a lifetime-sentinel shared_ptr.
  auto strong = std::make_shared<int>(42);
  std::weak_ptr<int> weak = strong;
  SentinelHolder holder{strong, &started, &release};

  // 2. Submit the Task; a worker-local copy runs the body.
  lol::runtime::WorkerPool::Task task{std::move(holder)};
  if (!pool.submit(std::move(task))) {
    release.open();
    return fail("submit failed");
  }

  // 3. Once the body has started and is waiting on the gate, drop every
  // caller-side strong reference. The moved-from submit wrapper may still
  // hold the captures on some standard libraries, so clear it explicitly
  // instead of assuming it is empty; then reset the caller's original
  // sentinel strong pointer.
  if (!started.wait(kHandshakeTimeout)) {
    release.open();
    return fail("task body never started");
  }
  task = lol::runtime::WorkerPool::Task{};
  strong.reset();

  // 4. The body exits; the pool destroys its worker-local copy before
  // waitUntilIdle() returns true; 5. the sentinel weak_ptr is expired
  // immediately after return.
  release.open();
  const bool idle = pool.waitUntilIdle(std::chrono::milliseconds(10000));
  const bool expired = weak.expired();

  if (!idle) {
    return fail("waitUntilIdle timed out");
  }
  if (!expired) {
    return fail("sentinel capture still alive after idle");
  }
  return true;
}

// More than one worker, more than one queued task: every accepted task runs
// exactly once (no loss, no duplicate), and metrics stay exact.
bool testMultipleWorkersAndQueuedTasks() {
  constexpr std::uint64_t kTaskCount = 8;
  lol::runtime::WorkerPool pool{{.threadCount = 4, .queueCapacity = 64}};
  std::atomic<std::uint64_t> executions{0};

  for (std::uint64_t i = 0; i < kTaskCount; ++i) {
    if (!pool.submit([&executions] { ++executions; })) {
      return fail("submit failed");
    }
  }

  if (!pool.waitUntilIdle(std::chrono::milliseconds(5000))) {
    return fail("pool did not reach idle");
  }
  if (executions.load() != kTaskCount) {
    return fail("task lost or executed more than once");
  }

  const auto metrics = pool.metrics();
  if (metrics.pendingTasks != 0U || metrics.activeTasks != 0U ||
      metrics.acceptedTasks != kTaskCount ||
      metrics.completedTasks != kTaskCount || metrics.rejectedTasks != 0U) {
    return fail("metrics not exact after quiescence");
  }
  return true;
}

// Overflow rejection stays exact: submit one blocking task, wait until the
// worker has entered it, then fill the queue with exactly queueCapacity more
// tasks. The next submit must be rejected; after the gate opens, every
// accepted task drains exactly once. Every failure path opens the gate so
// WorkerPool destruction can never join a stranded worker.
bool testOverflowRejection() {
  constexpr std::size_t kQueueCapacity = 2;
  lol::runtime::WorkerPool pool{
      {.threadCount = 1, .queueCapacity = kQueueCapacity}};
  Gate started;
  Gate release;
  std::atomic<std::uint64_t> completed{0};

  auto task = [&] {
    started.open();
    if (release.wait(kHandshakeTimeout)) {
      ++completed;
    }
  };

  // 1. Submit one blocking task.
  if (!pool.submit(task)) {
    release.open();
    return fail("initial submit failed");
  }
  // 2. Wait until that task has entered.
  if (!started.wait(kHandshakeTimeout)) {
    release.open();
    return fail("first task never entered");
  }
  // 3. The worker is blocked inside the first task, so the queue has room for
  // exactly queueCapacity more accepted tasks.
  for (std::size_t i = 0; i < kQueueCapacity; ++i) {
    if (!pool.submit(task)) {
      release.open();
      return fail("queued submit failed");
    }
  }
  // 4. The queue is full and the only worker is busy: the next submit must be
  // rejected.
  if (pool.submit(task)) {
    release.open();
    return fail("overflow was accepted");
  }

  auto metrics = pool.metrics();
  if (metrics.acceptedTasks != 3U || metrics.rejectedTasks != 1U ||
      metrics.pendingTasks != kQueueCapacity || metrics.activeTasks != 1U) {
    release.open();
    return fail("metrics wrong at overflow point");
  }

  // 5. Open the gate; 6. every accepted task drains exactly once.
  release.open();
  if (!pool.waitUntilIdle(std::chrono::milliseconds(5000))) {
    return fail("pool did not reach idle after gate release");
  }
  if (completed.load() != 3U) {
    return fail("overflow drain lost work");
  }

  metrics = pool.metrics();
  if (metrics.acceptedTasks != 3U || metrics.rejectedTasks != 1U ||
      metrics.completedTasks != 3U || metrics.pendingTasks != 0U ||
      metrics.activeTasks != 0U) {
    return fail("metrics wrong after drain");
  }
  return true;
}

// stop() during active work drains every accepted task: the worker is
// mid-task when stop is called, and all queued tasks still run exactly once.
// A stopper thread signals immediately before calling stop(); the main
// thread observes that signal, releases the active task, then joins the
// stopper. No sleeps: the gate handshake is the only synchronization.
bool testStopDuringActiveWorkDrains() {
  lol::runtime::WorkerPool pool{{.threadCount = 1, .queueCapacity = 16}};
  Gate started;
  Gate release;
  Gate stopSignaled;
  std::atomic<std::uint64_t> completed{0};

  auto task = [&] {
    started.open();
    if (release.wait(kHandshakeTimeout)) {
      ++completed;
    }
  };

  for (std::size_t i = 0; i < 3U; ++i) {
    if (!pool.submit(task)) {
      release.open();
      return fail("submit failed");
    }
  }

  // The worker is actively running the first task and blocked on the gate;
  // two tasks are still queued.
  if (!started.wait(kHandshakeTimeout)) {
    release.open();
    return fail("first task never entered");
  }

  // The stopper signals immediately before calling stop(), so the main
  // thread knows stop() is about to join a worker that is mid-task.
  std::thread stopper{[&] {
    stopSignaled.open();
    pool.stop();
  }};
  if (!stopSignaled.wait(kHandshakeTimeout)) {
    release.open();
    stopper.join();
    return fail("stopper never signaled");
  }
  release.open();
  stopper.join();

  if (completed.load() != 3U) {
    return fail("stop did not drain every accepted task");
  }
  const auto metrics = pool.metrics();
  if (metrics.acceptedTasks != 3U || metrics.completedTasks != 3U ||
      metrics.pendingTasks != 0U || metrics.activeTasks != 0U ||
      metrics.rejectedTasks != 0U) {
    return fail("metrics wrong after stop");
  }
  return true;
}

struct TestCase {
  const char *name;
  bool (*run)();
};

} // namespace

int main() {
  const TestCase tests[] = {
      {"quiescence_releases_completed_captures",
       testQuiescenceReleasesCompletedCaptures},
      {"multiple_workers_and_queued_tasks", testMultipleWorkersAndQueuedTasks},
      {"overflow_rejection", testOverflowRejection},
      {"stop_during_active_work_drains", testStopDuringActiveWorkDrains},
  };

  bool allPassed = true;
  for (const auto &test : tests) {
    if (test.run()) {
      std::printf("PASS %s\n", test.name);
    } else {
      std::printf("FAIL %s\n", test.name);
      allPassed = false;
    }
  }
  return allPassed ? EXIT_SUCCESS : EXIT_FAILURE;
}
