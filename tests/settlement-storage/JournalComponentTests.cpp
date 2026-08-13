#include <lol/settlement_storage/SegmentJournal.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

#ifndef LOOT_OUTBOX_GOLDEN_DIR
#error "LOOT_OUTBOX_GOLDEN_DIR must name the outbox journal golden directory"
#endif

namespace {

using lol::settlement_storage::AppendFaultInjector;
using lol::settlement_storage::AppendResultStatus;
using lol::settlement_storage::AppendStage;
using lol::settlement_storage::JournalRecoveryStatus;
using lol::settlement_storage::SegmentJournal;

class TempDirectory {
public:
  TempDirectory() {
    const std::string base =
        (std::filesystem::temp_directory_path() / "loot-journal-XXXXXX")
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
               [](unsigned char value) { return !std::isspace(value); });

  std::vector<std::uint8_t> bytes;
  bytes.reserve(compact.size() / 2u);
  for (std::size_t offset = 0; offset < compact.size(); offset += 2u) {
    bytes.push_back(static_cast<std::uint8_t>(
        std::stoul(compact.substr(offset, 2u), nullptr, 16)));
  }
  return bytes;
}

std::vector<std::uint8_t> readFile(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>{stream},
          std::istreambuf_iterator<char>{}};
}

void writeFile(const std::filesystem::path &path,
               const std::vector<std::uint8_t> &bytes) {
  std::ofstream stream(path, std::ios::binary);
  stream.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

lol::settlement_storage::AppendBatch goldenBatch() {
  const auto valid = readHex("outbox-journal-valid.hex");
  constexpr std::size_t firstPayloadOffset = 56u;
  constexpr std::size_t firstPayloadLength = 114u;
  constexpr std::size_t secondRecordOffset = 174u;
  constexpr std::size_t secondPayloadOffset = secondRecordOffset + 56u;
  constexpr std::size_t secondPayloadLength = 114u;
  const lol::settlement_storage::JournalBatchId batchId = {
      0x62, 0x0b, 0x39, 0xc0, 0xdb, 0x27, 0x93, 0x37,
      0x4d, 0x8a, 0x5e, 0xf7, 0xe3, 0x44, 0x15, 0x36};
  return {
      .batchId = batchId,
      .canonicalIntents =
          {std::vector<std::uint8_t>(
               valid.begin() + static_cast<std::ptrdiff_t>(firstPayloadOffset),
               valid.begin() + static_cast<std::ptrdiff_t>(firstPayloadOffset +
                                                           firstPayloadLength)),
           std::vector<std::uint8_t>(
               valid.begin() + static_cast<std::ptrdiff_t>(secondPayloadOffset),
               valid.begin() + static_cast<std::ptrdiff_t>(
                                   secondPayloadOffset + secondPayloadLength))},
  };
}

class FaultAt final : public AppendFaultInjector {
public:
  FaultAt(AppendStage stage, std::size_t recordIndex)
      : stage_(stage), recordIndex_(recordIndex) {}

  bool shouldFail(AppendStage stage, std::size_t recordIndex) override {
    if (stage == AppendStage::BeforeDataSync) {
      ++beforeSyncCalls;
    } else if (stage == AppendStage::AfterDataSync) {
      ++afterSyncCalls;
    }
    return stage == stage_ && recordIndex == recordIndex_;
  }

  std::size_t beforeSyncCalls{};
  std::size_t afterSyncCalls{};

private:
  AppendStage stage_;
  std::size_t recordIndex_;
};

class CountingFaultInjector final : public AppendFaultInjector {
public:
  bool shouldFail(AppendStage stage, std::size_t) override {
    if (stage == AppendStage::BeforeDataSync) {
      ++beforeSyncCalls;
    } else if (stage == AppendStage::AfterDataSync) {
      ++afterSyncCalls;
    }
    return false;
  }

  std::size_t beforeSyncCalls{};
  std::size_t afterSyncCalls{};
};

bool writesGoldenBatchAndCompletesAfterOneSync() {
  TempDirectory directory;
  const auto journalPath = directory.path() / "segment-1.journal";
  CountingFaultInjector injector;
  SegmentJournal journal{journalPath, &injector};
  const auto recovered = journal.recoverAndRepair();
  const auto result = journal.append(goldenBatch());
  return recovered.has_value() &&
         recovered->status == JournalRecoveryStatus::Clean &&
         result.status == AppendResultStatus::DurablyQueued &&
         result.commitSequence == 3u && injector.beforeSyncCalls == 1u &&
         injector.afterSyncCalls == 1u &&
         readFile(journalPath) == readHex("outbox-journal-valid.hex");
}

bool neverCompletesAtInjectedRecordFailures() {
  constexpr std::array stages = {AppendStage::BeforeRecordWrite,
                                 AppendStage::AfterRecordWrite};
  for (const auto stage : stages) {
    for (std::size_t recordIndex = 0; recordIndex < 3u; ++recordIndex) {
      TempDirectory directory;
      const auto journalPath = directory.path() / "segment-1.journal";
      FaultAt injector{stage, recordIndex};
      SegmentJournal journal{journalPath, &injector};
      if (!journal.recoverAndRepair().has_value()) {
        return false;
      }
      const auto result = journal.append(goldenBatch());
      if (result.status != AppendResultStatus::InjectedFailure ||
          result.commitSequence.has_value()) {
        return false;
      }
    }
  }
  return true;
}

bool neverCompletesAtInjectedSyncFailures() {
  constexpr std::array stages = {AppendStage::BeforeDataSync,
                                 AppendStage::AfterDataSync};
  for (const auto stage : stages) {
    TempDirectory directory;
    const auto journalPath = directory.path() / "segment-1.journal";
    FaultAt injector{stage, 3u};
    SegmentJournal journal{journalPath, &injector};
    if (!journal.recoverAndRepair().has_value()) {
      return false;
    }
    const auto result = journal.append(goldenBatch());
    if (result.status != AppendResultStatus::InjectedFailure ||
        result.commitSequence.has_value()) {
      return false;
    }

    SegmentJournal restarted{journalPath};
    const auto recovered = restarted.recoverAndRepair();
    if (!recovered.has_value() || recovered->batches.size() != 1u ||
        recovered->batches.front().commitSequence != 3u) {
      return false;
    }
  }
  return true;
}

bool quarantinesAndTruncatesIncompleteTail() {
  TempDirectory directory;
  const auto journalPath = directory.path() / "segment-1.journal";
  const auto partial = readHex("outbox-journal-partial.hex");
  writeFile(journalPath, partial);

  SegmentJournal journal{journalPath};
  const auto recovered = journal.recoverAndRepair();
  const auto quarantine =
      directory.path() / "segment-1.journal.quarantine.489.0";
  return recovered.has_value() &&
         recovered->status ==
             JournalRecoveryStatus::IncompleteTailQuarantined &&
         std::filesystem::file_size(journalPath) == 0u &&
         readFile(quarantine) == partial;
}

bool quarantinesCorruptTailAndKeepsCommittedPrefix() {
  TempDirectory directory;
  const auto journalPath = directory.path() / "segment-1.journal";
  const auto corrupt = readHex("outbox-journal-corrupt.hex");
  writeFile(journalPath, corrupt);

  SegmentJournal journal{journalPath};
  const auto recovered = journal.recoverAndRepair();
  const auto quarantine =
      directory.path() / "segment-1.journal.quarantine.590.506";
  return recovered.has_value() &&
         recovered->status == JournalRecoveryStatus::CorruptTailQuarantined &&
         std::filesystem::file_size(journalPath) == 506u &&
         readFile(journalPath) == readHex("outbox-journal-valid.hex") &&
         readFile(quarantine) ==
             std::vector<std::uint8_t>(corrupt.begin() +
                                           static_cast<std::ptrdiff_t>(506u),
                                       corrupt.end());
}

} // namespace

int main() {
  return writesGoldenBatchAndCompletesAfterOneSync() &&
                 neverCompletesAtInjectedRecordFailures() &&
                 neverCompletesAtInjectedSyncFailures() &&
                 quarantinesAndTruncatesIncompleteTail() &&
                 quarantinesCorruptTailAndKeepsCommittedPrefix()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
