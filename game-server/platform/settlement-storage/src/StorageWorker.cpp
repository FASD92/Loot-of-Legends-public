#include <lol/settlement_storage/StorageWorker.hpp>

#include <utility>

namespace lol::settlement_storage {
namespace {

settlement::SettlementStorageFailure
failureFor(AppendResultStatus status) noexcept {
  return status == AppendResultStatus::InvalidBatch
             ? settlement::SettlementStorageFailure::InvalidBatch
             : settlement::SettlementStorageFailure::IoFailure;
}

} // namespace

StorageWorker::StorageWorker(std::filesystem::path journalPath)
    : journal_(std::move(journalPath)), worker_([this] { run(); }) {}

StorageWorker::~StorageWorker() {
  {
    std::lock_guard lock{mutex_};
    stopping_ = true;
  }
  changed_.notify_one();
  if (worker_.joinable()) {
    worker_.join();
  }
}

settlement::SubmitAppendResult
StorageWorker::submit(settlement::DurableAppendRequest request,
                      CompletionSink completion) {
  if (!completion) {
    return settlement::SubmitAppendResult::StorageUnavailable;
  }
  {
    std::lock_guard lock{mutex_};
    if (stopping_) {
      return settlement::SubmitAppendResult::StorageUnavailable;
    }
    if (queue_.size() >= kQueueCapacity) {
      return settlement::SubmitAppendResult::QueueFull;
    }
    queue_.push_back(Job{.request = std::move(request),
                         .completion = std::move(completion)});
  }
  changed_.notify_one();
  return settlement::SubmitAppendResult::Accepted;
}

settlement::OutboxLoadResult StorageWorker::nextUnretired() {
  std::lock_guard lock{journalMutex_};
  return journal_.nextUnretired();
}

settlement::OutboxRetireResult
StorageWorker::retire(const settlement::SettlementBatchId &batchId,
                      std::uint64_t commitSequence) {
  std::lock_guard lock{journalMutex_};
  return journal_.retire(batchId, commitSequence);
}

bool StorageWorker::compact() {
  std::lock_guard lock{journalMutex_};
  return journal_.compact();
}

void StorageWorker::run() {
  while (true) {
    std::optional<Job> job;
    {
      std::unique_lock lock{mutex_};
      changed_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        return;
      }
      job = std::move(queue_.front());
      queue_.pop_front();
    }

    AppendResult result;
    {
      std::lock_guard lock{journalMutex_};
      result = journal_.append(AppendBatch{
          .batchId = job->request.batchId.bytes(),
          .canonicalIntents = std::move(job->request.canonicalIntents),
      });
    }
    if (result.status == AppendResultStatus::DurablyQueued &&
        result.commitSequence.has_value()) {
      job->completion(settlement::DurableAppendCompleted{
          .batchId = job->request.batchId,
          .roomId = job->request.roomId,
          .battleId = job->request.battleId,
          .commitSequence = *result.commitSequence,
      });
    } else {
      job->completion(settlement::DurableAppendFailed{
          .batchId = job->request.batchId,
          .roomId = job->request.roomId,
          .battleId = job->request.battleId,
          .failure = failureFor(result.status),
      });
    }
  }
}

} // namespace lol::settlement_storage
