#include <lol/settlement/SettlementPublication.hpp>
#include <lol/settlement_storage/JournalRecovery.hpp>
#include <lol/settlement_storage/SegmentJournal.hpp>
#include <lol/settlement_storage/StorageWorker.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using lol::settlement::DurableAppendCompleted;
using lol::settlement::DurableAppendFailed;
using lol::settlement::DurableAppendOutcome;
using lol::settlement::DurableAppendRequest;
using lol::settlement::SettlementBatchId;
using lol::settlement::SettlementStorageFailure;
using lol::settlement::SubmitAppendResult;
using lol::settlement_storage::JournalRecoveryStatus;
using lol::settlement_storage::SegmentJournal;
using lol::settlement_storage::StorageWorker;
using lol::shared::BattleInstanceId;
using lol::shared::RoomId;

class TempDirectory {
public:
  TempDirectory() {
    const std::string base =
        (std::filesystem::temp_directory_path() / "loot-storage-worker-XXXXXX")
            .string();
    std::vector<char> pattern(base.begin(), base.end());
    pattern.push_back('\0');
    path_ = ::mkdtemp(pattern.data());
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path &path() const noexcept { return path_; }

private:
  std::filesystem::path path_;
};

std::vector<std::uint8_t> readHex(const std::string &name) {
  std::ifstream stream(std::string{LOOT_OUTBOX_GOLDEN_DIR} + "/" + name);
  const std::string text{std::istreambuf_iterator<char>{stream},
                         std::istreambuf_iterator<char>{}};
  std::string compact;
  std::copy_if(text.begin(), text.end(), std::back_inserter(compact),
               [](unsigned char value) { return value > ' '; });
  std::vector<std::uint8_t> bytes;
  bytes.reserve(compact.size() / 2u);
  for (std::size_t offset = 0; offset < compact.size(); offset += 2u) {
    bytes.push_back(static_cast<std::uint8_t>(
        std::stoul(compact.substr(offset, 2u), nullptr, 16)));
  }
  return bytes;
}

DurableAppendRequest request() {
  const auto valid = readHex("outbox-journal-valid.hex");
  constexpr std::size_t firstPayloadOffset = 56u;
  constexpr std::size_t payloadLength = 114u;
  constexpr std::size_t secondPayloadOffset = 174u + 56u;
  SettlementBatchId::Bytes bytes = {0x62, 0x0b, 0x39, 0xc0, 0xdb, 0x27,
                                    0x93, 0x37, 0x4d, 0x8a, 0x5e, 0xf7,
                                    0xe3, 0x44, 0x15, 0x36};
  return {
      .batchId = SettlementBatchId{bytes},
      .roomId = RoomId{7},
      .battleId = BattleInstanceId{9},
      .canonicalIntents =
          {std::vector<std::uint8_t>(
               valid.begin() + static_cast<std::ptrdiff_t>(firstPayloadOffset),
               valid.begin() + static_cast<std::ptrdiff_t>(firstPayloadOffset +
                                                           payloadLength)),
           std::vector<std::uint8_t>(
               valid.begin() + static_cast<std::ptrdiff_t>(secondPayloadOffset),
               valid.begin() + static_cast<std::ptrdiff_t>(secondPayloadOffset +
                                                           payloadLength))},
  };
}

class Completion final {
public:
  void set(DurableAppendOutcome outcome) {
    std::lock_guard lock{mutex_};
    outcome_ = std::move(outcome);
    callbackThread_ = std::this_thread::get_id();
    changed_.notify_all();
  }

  std::optional<DurableAppendOutcome> wait() {
    std::unique_lock lock{mutex_};
    if (!changed_.wait_for(lock, 2s, [this] { return outcome_.has_value(); })) {
      return std::nullopt;
    }
    return outcome_;
  }

  std::thread::id callbackThread() const {
    std::lock_guard lock{mutex_};
    return callbackThread_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::optional<DurableAppendOutcome> outcome_;
  std::thread::id callbackThread_;
};

bool appendsOffCallerAndCompletesAfterDurability() {
  TempDirectory directory;
  const auto path = directory.path() / "segment-1.journal";
  const auto caller = std::this_thread::get_id();
  Completion completion;
  {
    StorageWorker worker{path};
    if (worker.submit(request(), [&completion](DurableAppendOutcome outcome) {
          completion.set(std::move(outcome));
        }) != SubmitAppendResult::Accepted) {
      return false;
    }
    const auto outcome = completion.wait();
    const auto *completed = outcome.has_value()
                                ? std::get_if<DurableAppendCompleted>(&*outcome)
                                : nullptr;
    if (completed == nullptr || completed->roomId != RoomId{7} ||
        completed->battleId != BattleInstanceId{9} ||
        completed->commitSequence != 3u ||
        completion.callbackThread() == caller) {
      return false;
    }
  }
  SegmentJournal restarted{path};
  const auto recovered = restarted.recoverAndRepair();
  return recovered.has_value() &&
         recovered->status == JournalRecoveryStatus::Clean &&
         recovered->batches.size() == 1u &&
         recovered->batches.front().commitSequence == 3u;
}

bool invalidBatchReturnsExplicitFailure() {
  TempDirectory directory;
  Completion completion;
  auto invalid = request();
  invalid.canonicalIntents.pop_back();
  StorageWorker worker{directory.path() / "segment-1.journal"};
  if (worker.submit(std::move(invalid),
                    [&completion](DurableAppendOutcome outcome) {
                      completion.set(std::move(outcome));
                    }) != SubmitAppendResult::Accepted) {
    return false;
  }
  const auto outcome = completion.wait();
  const auto *failed = outcome.has_value()
                           ? std::get_if<DurableAppendFailed>(&*outcome)
                           : nullptr;
  return failed != nullptr &&
         failed->failure == SettlementStorageFailure::InvalidBatch;
}

} // namespace

int main() {
  return appendsOffCallerAndCompletesAfterDurability() &&
                 invalidBatchReturnsExplicitFailure()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
