#include "Game/WorkerPool.hpp"

#include <chrono>

namespace Game {
namespace {
uint64_t elapsedMicros(
    std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point end) {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
}
}  // namespace

WorkerPool::WorkerPool(
    size_t workerCount,
    RoomEventDispatcher& dispatcher,
    RoomHandler handler,
    RoomEventMetrics* metrics)
    : workerCount_(workerCount),
      dispatcher_(dispatcher),
      handler_(handler),
      metrics_(metrics) {}

WorkerPool::~WorkerPool() {
    shutdownAndJoin();
}

bool WorkerPool::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (workerCount_ == 0 || started_ || shuttingDown_) {
        return false;
    }

    workers_.reserve(workerCount_);
    for (size_t i = 0; i < workerCount_; ++i) {
        workers_.emplace_back([this]() { workerLoop(); });
    }
    started_ = true;
    return true;
}

void WorkerPool::notifyRoomScheduled() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++wakeTokens_;
    }
    wakeCondition_.notify_one();
}

void WorkerPool::shutdownAndJoin() {
    bool shouldBeginDispatcherShutdown = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ && workers_.empty()) {
            return;
        }
        if (!shuttingDown_) {
            shuttingDown_ = true;
            shouldBeginDispatcherShutdown = true;
        }
    }

    if (shouldBeginDispatcherShutdown) {
        dispatcher_.beginShutdown();
    }
    wakeCondition_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    workers_.clear();
    started_ = false;
    wakeTokens_ = 0;
}

void WorkerPool::workerLoop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wakeCondition_.wait(
                lock,
                [this]() { return wakeTokens_ > 0 || shuttingDown_; });
            if (wakeTokens_ > 0) {
                --wakeTokens_;
            }
        }

        uint32_t roomId = 0;
        if (dispatcher_.tryPopActiveRoom(roomId)) {
            if (metrics_ != nullptr) {
                metrics_->workerStarted();
            }
            const std::chrono::steady_clock::time_point startedAt =
                std::chrono::steady_clock::now();
            handler_(roomId);
            const uint64_t handlerElapsedUs =
                elapsedMicros(startedAt, std::chrono::steady_clock::now());
            if (metrics_ != nullptr) {
                metrics_->recordEventProcessingTimeUs(handlerElapsedUs);
                metrics_->recordWorkerBusyTimeUs(handlerElapsedUs);
                metrics_->recordProcessed();
                metrics_->workerFinished();
            }
            const RoomEventDispatcherCompletionResult completion =
                dispatcher_.completeRoomProcessing(roomId);
            if (completion.rescheduledRoom) {
                notifyRoomScheduled();
            }
            continue;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (shuttingDown_) {
            break;
        }
    }
}
}  // namespace Game
