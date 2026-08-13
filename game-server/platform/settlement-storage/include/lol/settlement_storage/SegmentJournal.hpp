#pragma once

#include <lol/settlement/SettlementPublisher.hpp>
#include <lol/settlement_storage/JournalRecovery.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace lol::settlement_storage {

struct AppendBatch {
  JournalBatchId batchId;
  std::vector<std::vector<std::uint8_t>> canonicalIntents;
};

enum class AppendStage {
  BeforeRecordWrite,
  AfterRecordWrite,
  BeforeDataSync,
  AfterDataSync,
};

class AppendFaultInjector {
public:
  virtual ~AppendFaultInjector() = default;
  virtual bool shouldFail(AppendStage stage, std::size_t recordIndex) = 0;
};

enum class AppendResultStatus {
  DurablyQueued,
  InvalidBatch,
  StorageUnavailable,
  InjectedFailure,
};

struct AppendResult {
  AppendResultStatus status{AppendResultStatus::StorageUnavailable};
  std::optional<std::uint64_t> commitSequence;
};

class SegmentJournal : public settlement::SettlementOutboxPort {
public:
  explicit SegmentJournal(std::filesystem::path path,
                          AppendFaultInjector *faultInjector = nullptr);

  std::optional<JournalRecoveryResult> recoverAndRepair();
  AppendResult append(const AppendBatch &batch);
  [[nodiscard]] settlement::OutboxLoadResult nextUnretired() override;
  [[nodiscard]] settlement::OutboxRetireResult
  retire(const settlement::SettlementBatchId &batchId,
         std::uint64_t commitSequence) override;
  [[nodiscard]] bool compact() override;
  bool healthy() const noexcept;

private:
  bool resolveActivePath();

  std::filesystem::path path_;
  std::filesystem::path activePath_;
  AppendFaultInjector *faultInjector_{};
  std::uint64_t nextSequence_{1u};
  std::uint64_t compactedThrough_{};
  std::uint64_t generation_{};
  bool recovered_{};
  bool healthy_{};
};

} // namespace lol::settlement_storage
