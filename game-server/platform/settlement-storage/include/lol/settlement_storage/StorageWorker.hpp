#pragma once

#include <lol/settlement/SettlementPublication.hpp>
#include <lol/settlement_storage/SegmentJournal.hpp>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>

namespace lol::settlement_storage {

class StorageWorker final : public settlement::SettlementStoragePort,
                            public settlement::SettlementOutboxPort {
public:
  explicit StorageWorker(std::filesystem::path journalPath);
  ~StorageWorker() override;

  StorageWorker(const StorageWorker &) = delete;
  StorageWorker &operator=(const StorageWorker &) = delete;

  [[nodiscard]] settlement::SubmitAppendResult
  submit(settlement::DurableAppendRequest request,
         CompletionSink completion) override;
  [[nodiscard]] settlement::OutboxLoadResult nextUnretired() override;
  [[nodiscard]] settlement::OutboxRetireResult
  retire(const settlement::SettlementBatchId &batchId,
         std::uint64_t commitSequence) override;
  [[nodiscard]] bool compact() override;

private:
  static constexpr std::size_t kQueueCapacity = 128u;

  struct Job final {
    settlement::DurableAppendRequest request;
    CompletionSink completion;
  };

  void run();

  SegmentJournal journal_;
  std::mutex journalMutex_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<Job> queue_;
  bool stopping_{};
  std::thread worker_;
};

} // namespace lol::settlement_storage
