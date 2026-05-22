#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "Game/RoomEventDispatcher.hpp"
#include "Game/RoomEventMetrics.hpp"

namespace Game {
class WorkerPool {
public:
    using RoomHandler = std::function<void(uint32_t roomId)>;

    WorkerPool(
        size_t workerCount,
        RoomEventDispatcher& dispatcher,
        RoomHandler handler,
        RoomEventMetrics* metrics = nullptr);
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    ~WorkerPool();

    bool start();
    void notifyRoomScheduled();
    void shutdownAndJoin();

private:
    void workerLoop();

    const size_t workerCount_;
    RoomEventDispatcher& dispatcher_;
    RoomHandler handler_;
    RoomEventMetrics* metrics_{nullptr};
    std::mutex mutex_;
    std::condition_variable wakeCondition_;
    std::vector<std::thread> workers_;
    bool started_{false};
    bool shuttingDown_{false};
    size_t wakeTokens_{0};
};
}  // namespace Game
