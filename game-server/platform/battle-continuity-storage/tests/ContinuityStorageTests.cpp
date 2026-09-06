#include <lol/battle_continuity/Durability.hpp>
#include <lol/battle_continuity/RecordCodec.hpp>
#include <lol/battle_continuity_storage/ContinuityStorage.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using lol::battle_continuity::BattleIdentity;
using lol::battle_continuity::BattleStartParticipant;
using lol::battle_continuity::BattleStartPayload;
using lol::battle_continuity::Bytes;
using lol::battle_continuity::CheckpointPayload;
using lol::battle_continuity::CommandDecisionPayload;
using lol::battle_continuity::CommandId;
using lol::battle_continuity::DurableTickCommitted;
using lol::battle_continuity::DurableTickSubmitResult;
using lol::battle_continuity::DurableTickWriteFailed;
using lol::battle_continuity::DurableTickWriteFailure;
using lol::battle_continuity::DurableTickWriteOutcome;
using lol::battle_continuity::DurableTickWriteRequest;
using lol::battle_continuity::Record;
using lol::battle_continuity::RecordedTickBatch;
using lol::battle_continuity::RecordHeader;
using lol::battle_continuity::RecordType;
using lol::battle_continuity::TickCommitPayload;
using lol::battle_continuity_storage::ContinuityStorage;
using lol::battle_continuity_storage::StorageError;
using lol::shared::BattleInstanceId;
using lol::shared::RoomId;
using lol::shared::SessionId;

class TempDirectory final {
public:
  TempDirectory() {
    const auto pattern = (std::filesystem::temp_directory_path() /
                          "lol-battle-continuity-storage-XXXXXX")
                             .string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    const auto created = ::mkdtemp(writable.data());
    if (created == nullptr) {
      std::abort();
    }
    path_ = created;
  }

  ~TempDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  TempDirectory(const TempDirectory &) = delete;
  TempDirectory &operator=(const TempDirectory &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

std::filesystem::path createKey(const std::filesystem::path &root,
                                std::size_t length = 32U,
                                mode_t mode = S_IRUSR | S_IWUSR,
                                const char *name = "continuity.key") {
  const auto path = root / name;
  const auto descriptor =
      ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, mode);
  if (descriptor < 0) {
    std::abort();
  }
  std::vector<std::uint8_t> key(length, 0U);
  if (!key.empty()) {
    key.front() = 0xA5U;
  }
  if (::write(descriptor, key.data(), key.size()) !=
          static_cast<ssize_t>(key.size()) ||
      ::close(descriptor) != 0) {
    std::abort();
  }
  return path;
}

void writeAppend(const std::filesystem::path &path,
                 std::span<const std::uint8_t> bytes) {
  const auto descriptor = ::open(path.c_str(), O_WRONLY | O_APPEND);
  if (descriptor < 0 ||
      ::write(descriptor, bytes.data(), bytes.size()) !=
          static_cast<ssize_t>(bytes.size()) ||
      ::close(descriptor) != 0) {
    std::abort();
  }
}

void flipByte(const std::filesystem::path &path, off_t offset) {
  const auto descriptor = ::open(path.c_str(), O_RDWR);
  const auto whence = offset < 0 ? SEEK_END : SEEK_SET;
  const auto position = offset;
  if (descriptor < 0 || ::lseek(descriptor, position, whence) < 0) {
    std::abort();
  }
  std::uint8_t value = 0xEEU;
  if (::write(descriptor, &value, sizeof(value)) !=
          static_cast<ssize_t>(sizeof(value)) ||
      ::close(descriptor) != 0) {
    std::abort();
  }
}

Bytes encodeRecords(std::vector<Record> records) {
  Bytes bytes;
  for (const auto &record : records) {
    const auto encoded = lol::battle_continuity::encodeRecord(record);
    if (!encoded.ok()) {
      std::abort();
    }
    bytes.insert(bytes.end(), encoded.bytes.begin(), encoded.bytes.end());
  }
  return bytes;
}

BattleIdentity identity(std::uint32_t epoch, std::uint32_t ordinal,
                        std::uint64_t battle = 1U) {
  return BattleIdentity{
      .originRecoveryEpoch = epoch,
      .roomId = RoomId{(static_cast<std::uint64_t>(epoch) << 32U) | ordinal},
      .battleInstanceId = BattleInstanceId{battle},
  };
}

RecordHeader header(const BattleIdentity &battle, std::uint32_t writerEpoch,
                    RecordType type, std::uint64_t sequence,
                    std::uint64_t tick) {
  return RecordHeader{
      .recordType = type,
      .recordSequence = sequence,
      .originRecoveryEpoch = battle.originRecoveryEpoch,
      .writerRecoveryEpoch = writerEpoch,
      .roomId = battle.roomId,
      .battleInstanceId = battle.battleInstanceId,
      .logicalTick = tick,
      .battleElapsedNanos = tick * lol::battle_continuity::kTickNanos,
  };
}

RecordedTickBatch validBatch(const BattleIdentity &battle,
                             std::uint32_t writerEpoch,
                             std::uint64_t firstSequence, std::uint64_t tick,
                             bool initial) {
  const auto stateBytes = Bytes{0x10U, static_cast<std::uint8_t>(tick)};
  const auto stateHash =
      lol::battle_continuity::canonicalStateHash(stateBytes).value();
  std::vector<Record> records;
  if (initial) {
    records.push_back(
        Record{.header = header(battle, writerEpoch, RecordType::BattleStart,
                                firstSequence, 0U),
               .payload = BattleStartPayload{
                   .rulesetVersion = 1U,
                   .battleSeed = 17U,
                   .tickHertz = 20U,
                   .initialStateHash = stateHash,
                   .participants =
                       {
                           BattleStartParticipant{.participantSlot = 1U,
                                                  .sessionId = SessionId{11U}},
                           BattleStartParticipant{.participantSlot = 2U,
                                                  .sessionId = SessionId{12U}},
                       },
               }});
    records.push_back(
        Record{.header = header(battle, writerEpoch, RecordType::Checkpoint,
                                firstSequence + 1U, 0U),
               .payload = CheckpointPayload{
                   .checkpointSchemaVersion =
                       lol::battle_continuity::kCheckpointSchemaVersion,
                   .canonicalStateBytes = stateBytes,
                   .stateHash = stateHash}});
  } else {
    records.push_back(Record{
        .header = header(battle, writerEpoch, RecordType::CommandDecision,
                         firstSequence, tick),
        .payload = CommandDecisionPayload{
            .participantSlot = 1U,
            .commandKind = 8U,
            .commandId = CommandId{.high = 4U, .low = tick},
            .decisionCode = 0U,
            .commandPayload = Bytes{0x01U},
            .outcomePayload = {},
            .roomRecoveryStateBytes = {},
            .postDecisionStateHash = stateHash,
        }});
  }
  const auto lastData = firstSequence + records.size() - 1U;
  records.push_back(
      Record{.header = header(battle, writerEpoch, RecordType::TickCommit,
                              lastData + 1U, tick),
             .payload = TickCommitPayload{
                 .firstRecordSequence = firstSequence,
                 .lastDataRecordSequence = lastData,
                 .recordCount = static_cast<std::uint32_t>(records.size()),
                 .committedStateHash = stateHash,
             }});
  return RecordedTickBatch{
      .identity = battle,
      .writerRecoveryEpoch = writerEpoch,
      .firstRecordSequence = firstSequence,
      .lastRecordSequence = lastData + 1U,
      .logicalTick = tick,
      .terminal = false,
      .encodedRecords = encodeRecords(std::move(records)),
  };
}

struct CompletionLatch final {
  void set(DurableTickWriteOutcome completion) {
    std::lock_guard lock{mutex};
    value = std::move(completion);
    changed.notify_all();
  }

  [[nodiscard]] std::optional<DurableTickWriteOutcome> wait() {
    std::unique_lock lock{mutex};
    if (!changed.wait_for(lock, 2s, [this] { return value.has_value(); })) {
      return std::nullopt;
    }
    return value;
  }

  std::mutex mutex;
  std::condition_variable changed;
  std::optional<DurableTickWriteOutcome> value;
};

DurableTickSubmitResult submit(ContinuityStorage &storage,
                               RecordedTickBatch batch, CompletionLatch &latch,
                               std::optional<Bytes> plaintext = std::nullopt) {
  return storage.submit(
      DurableTickWriteRequest{.batch = std::move(batch),
                              .privateEnvelopePlaintext = std::move(plaintext)},
      [&latch](DurableTickWriteOutcome outcome) {
        latch.set(std::move(outcome));
      });
}

bool rejectsUnsafeRootAndKey() {
  TempDirectory directory;
  const auto key = createKey(directory.path());
  const auto valid = ContinuityStorage::open(directory.path(), key);
  if (!valid.ok()) {
    return false;
  }
  valid.storage->stop();

  const auto shortKey =
      createKey(directory.path(), 31U, S_IRUSR | S_IWUSR, "short.key");
  if (ContinuityStorage::open(directory.path(), shortKey).ok()) {
    return false;
  }
  const auto longKey =
      createKey(directory.path(), 33U, S_IRUSR | S_IWUSR, "long.key");
  if (ContinuityStorage::open(directory.path(), longKey).ok()) {
    return false;
  }

  const auto permissiveKey = directory.path() / "permissive.key";
  const auto descriptor = ::open(permissiveKey.c_str(), O_WRONLY | O_CREAT,
                                 S_IRUSR | S_IWUSR | S_IRGRP);
  if (descriptor < 0) {
    return false;
  }
  const std::array<std::uint8_t, 32> permissiveBytes{};
  if (::write(descriptor, permissiveBytes.data(), permissiveBytes.size()) !=
          static_cast<ssize_t>(permissiveBytes.size()) ||
      ::close(descriptor) != 0) {
    return false;
  }
  if (ContinuityStorage::open(directory.path(), permissiveKey).ok()) {
    return false;
  }

  const auto symlinkKey = directory.path() / "symlink.key";
  if (::symlink(key.c_str(), symlinkKey.c_str()) != 0 ||
      ContinuityStorage::open(directory.path(), symlinkKey).ok()) {
    return false;
  }

  const auto actualRoot = directory.path() / "actual-root";
  const auto symlinkRoot = directory.path() / "root-link";
  if (::mkdir(actualRoot.c_str(), S_IRWXU) != 0 ||
      ::symlink(actualRoot.c_str(), symlinkRoot.c_str()) != 0) {
    return false;
  }
  const auto rootKey = createKey(actualRoot);
  return !ContinuityStorage::open(symlinkRoot, rootKey).ok();
}

bool manifestChecksumFailureStopsStartup() {
  TempDirectory directory;
  const auto key = createKey(directory.path());
  auto opened = ContinuityStorage::open(directory.path(), key);
  if (!opened.ok()) {
    return false;
  }
  opened.storage->stop();
  const auto manifest = directory.path() / ".battle-continuity.manifest";
  flipByte(manifest, -1);
  const auto retry = ContinuityStorage::open(directory.path(), key);
  return !retry.ok() && retry.error == StorageError::ManifestCorrupt;
}

bool differentKeyFailsGloballyBeforeEpochAdvance() {
  TempDirectory directory;
  const auto key = createKey(directory.path());
  auto first = ContinuityStorage::open(directory.path(), key);
  if (!first.ok() || first.storage->writerRecoveryEpoch() != 1U) {
    return false;
  }

  const auto battle = identity(1U, 1U);
  CompletionLatch completion;
  if (submit(*first.storage, validBatch(battle, 1U, 1U, 0U, true), completion,
             Bytes{0x91U}) != DurableTickSubmitResult::Accepted ||
      !completion.wait().has_value()) {
    return false;
  }
  first.storage->stop();
  first.storage.reset();

  const auto differentKey =
      createKey(directory.path(), 32U, S_IRUSR | S_IWUSR, "different.key");
  flipByte(differentKey, 0);
  const auto rejected =
      ContinuityStorage::open(directory.path(), differentKey);
  if (rejected.ok() || rejected.error != StorageError::InvalidKey) {
    return false;
  }

  auto reopened = ContinuityStorage::open(directory.path(), key);
  if (!reopened.ok() || reopened.storage->writerRecoveryEpoch() != 2U) {
    return false;
  }
  const auto recovered = reopened.storage->scan();
  return recovered.ok() && recovered.healthy.size() == 1U &&
         recovered.healthy.front().identity == battle &&
         recovered.quarantined.empty();
}

bool leaseAndEpochPersist() {
  TempDirectory directory;
  const auto key = createKey(directory.path());
  auto first = ContinuityStorage::open(directory.path(), key);
  if (!first.ok() || first.storage->writerRecoveryEpoch() != 1U) {
    return false;
  }
  const auto conflict = ContinuityStorage::open(directory.path(), key);
  if (conflict.ok() || conflict.error != StorageError::LockUnavailable) {
    return false;
  }
  first.storage->stop();
  first.storage.reset();
  auto second = ContinuityStorage::open(directory.path(), key);
  return second.ok() && second.storage->writerRecoveryEpoch() == 2U;
}

bool sidecarRoundTripsAndTamperIsolated() {
  TempDirectory directory;
  const auto key = createKey(directory.path());
  auto opened = ContinuityStorage::open(directory.path(), key);
  if (!opened.ok()) {
    return false;
  }
  const auto battle = identity(1U, 1U);
  CompletionLatch completion;
  const auto plaintext = Bytes{0x42U, 0x43U, 0x44U};
  if (submit(*opened.storage, validBatch(battle, 1U, 1U, 0U, true), completion,
             plaintext) != DurableTickSubmitResult::Accepted) {
    return false;
  }
  const auto outcome = completion.wait();
  const auto *committed = outcome.has_value()
                              ? std::get_if<DurableTickCommitted>(&*outcome)
                              : nullptr;
  if (committed == nullptr || committed->lastRecordSequence != 3U) {
    return false;
  }
  const auto recovered = opened.storage->scan();
  if (!recovered.ok() || recovered.healthy.size() != 1U ||
      !recovered.healthy.front().privateEnvelope.has_value() ||
      *recovered.healthy.front().privateEnvelope != plaintext) {
    return false;
  }
  flipByte(opened.storage->privateEnvelopePath(battle), -1);
  const auto afterTamper = opened.storage->scan();
  return afterTamper.ok() && afterTamper.healthy.empty() &&
         afterTamper.quarantined.size() == 1U;
}

bool missingSidecarIsQuarantined() {
  TempDirectory directory;
  const auto key = createKey(directory.path());
  auto opened = ContinuityStorage::open(directory.path(), key);
  if (!opened.ok()) {
    return false;
  }
  const auto battle = identity(1U, 1U);
  CompletionLatch completion;
  if (submit(*opened.storage, validBatch(battle, 1U, 1U, 0U, true), completion,
             Bytes{0x51U}) != DurableTickSubmitResult::Accepted ||
      !completion.wait().has_value()) {
    return false;
  }
  const auto sidecar = opened.storage->privateEnvelopePath(battle);
  std::error_code removeError;
  if (!std::filesystem::remove(sidecar, removeError) || removeError) {
    return false;
  }
  const auto recovered = opened.storage->scan();
  return recovered.ok() && recovered.healthy.empty() &&
         recovered.quarantined.size() == 1U &&
         recovered.quarantined.front().reason == StorageError::SidecarCorrupt;
}

bool startupQuarantineTombstoneIsolation() {
  TempDirectory directory;
  const auto key = createKey(directory.path());
  auto opened = ContinuityStorage::open(directory.path(), key);
  if (!opened.ok()) {
    return false;
  }
  const auto firstBattle = identity(1U, 1U);
  const auto failedBattle = identity(1U, 2U);
  CompletionLatch firstCompletion;
  CompletionLatch failedCompletion;
  if (submit(*opened.storage, validBatch(firstBattle, 1U, 1U, 0U, true),
             firstCompletion,
             Bytes{0x81U}) != DurableTickSubmitResult::Accepted ||
      submit(*opened.storage, validBatch(failedBattle, 1U, 1U, 0U, true),
             failedCompletion,
             Bytes{0x82U}) != DurableTickSubmitResult::Accepted ||
      !firstCompletion.wait().has_value() ||
      !failedCompletion.wait().has_value()) {
    return false;
  }
  opened.storage->waitUntilIdle();
  const auto epoch = opened.storage->writerRecoveryEpoch();
  if (opened.storage->quarantineBattle(failedBattle, epoch) !=
          StorageError::None ||
      !std::filesystem::exists(opened.storage->journalPath(failedBattle)) ||
      !std::filesystem::exists(
          opened.storage->privateEnvelopePath(failedBattle))) {
    return false;
  }
  const auto quarantineMarker = opened.storage->journalPath(failedBattle);
  const auto markerPath = [&quarantineMarker] {
    auto path = quarantineMarker;
    path.replace_extension(".quarantine");
    return path;
  }();
  if (!std::filesystem::exists(markerPath)) {
    return false;
  }
  const auto isolated = opened.storage->scan();
  if (!isolated.ok() || isolated.healthy.size() != 1U ||
      isolated.healthy.front().identity != firstBattle ||
      !std::filesystem::exists(opened.storage->journalPath(firstBattle)) ||
      !std::filesystem::exists(
          opened.storage->privateEnvelopePath(firstBattle))) {
    return false;
  }
  flipByte(markerPath, -1);
  const auto corruptMarker = opened.storage->scan();
  if (!corruptMarker.ok() || corruptMarker.healthy.size() != 1U ||
      corruptMarker.healthy.front().identity != firstBattle ||
      corruptMarker.quarantined.size() != 1U ||
      corruptMarker.quarantined.front().identity != failedBattle ||
      corruptMarker.quarantined.front().reason !=
          StorageError::JournalCorrupt ||
      opened.storage->quarantineBattle(failedBattle, epoch) !=
          StorageError::JournalCorrupt) {
    return false;
  }
  opened.storage->stop();
  return opened.storage->quarantineBattle(firstBattle, epoch) ==
         StorageError::StorageStopped;
}

bool retireTombstonePreservesEvidenceAndCrashRecovery() {
  TempDirectory directory;
  const auto key = createKey(directory.path());
  auto opened = ContinuityStorage::open(directory.path(), key);
  if (!opened.ok()) {
    return false;
  }
  const auto normalBattle = identity(1U, 1U);
  const auto interruptedBattle = identity(1U, 2U);
  CompletionLatch normalCompletion;
  CompletionLatch interruptedCompletion;
  if (submit(*opened.storage, validBatch(normalBattle, 1U, 1U, 0U, true),
             normalCompletion,
             Bytes{0xA1U}) != DurableTickSubmitResult::Accepted ||
      submit(*opened.storage, validBatch(interruptedBattle, 1U, 1U, 0U, true),
             interruptedCompletion,
             Bytes{0xA2U}) != DurableTickSubmitResult::Accepted ||
      !normalCompletion.wait().has_value() ||
      !interruptedCompletion.wait().has_value()) {
    return false;
  }
  opened.storage->waitUntilIdle();
  const auto epoch = opened.storage->writerRecoveryEpoch();
  const auto normalJournal = opened.storage->journalPath(normalBattle);
  const auto normalSidecar = opened.storage->privateEnvelopePath(normalBattle);
  const auto normalMarker = [&normalJournal] {
    auto path = normalJournal;
    path.replace_extension(".retired");
    return path;
  }();
  if (opened.storage->retireBattle(normalBattle, epoch) != StorageError::None ||
      !std::filesystem::exists(normalJournal) ||
      !std::filesystem::exists(normalSidecar) ||
      !std::filesystem::exists(normalMarker)) {
    return false;
  }

  // Model a crash before the atomic marker rename.  The active journal and
  // sidecar remain recoverable because no valid marker was committed.
  const auto interruptedJournal =
      opened.storage->journalPath(interruptedBattle);
  const auto interruptedSidecar =
      opened.storage->privateEnvelopePath(interruptedBattle);
  const auto interruptedMarker = [&interruptedJournal] {
    auto path = interruptedJournal;
    path.replace_extension(".retired");
    return path;
  }();
  const auto interruptedMarkerTemp =
      std::filesystem::path{interruptedMarker.string() + ".tmp"};
  const std::array<std::uint8_t, 4> partialMarker{0x4CU, 0x42U, 0x54U, 0x4DU};
  const auto partialDescriptor =
      ::open(interruptedMarkerTemp.c_str(), O_WRONLY | O_CREAT | O_EXCL,
             S_IRUSR | S_IWUSR);
  if (partialDescriptor < 0 || ::close(partialDescriptor) != 0) {
    return false;
  }
  writeAppend(interruptedMarkerTemp, partialMarker);
  if (!std::filesystem::exists(interruptedMarkerTemp) ||
      !std::filesystem::exists(interruptedJournal) ||
      !std::filesystem::exists(interruptedSidecar)) {
    return false;
  }
  opened.storage->stop();

  auto restarted = ContinuityStorage::open(directory.path(), key);
  if (!restarted.ok()) {
    return false;
  }
  const auto recovered = restarted.storage->scan();
  if (!recovered.ok() || recovered.healthy.size() != 1U ||
      recovered.healthy.front().identity != interruptedBattle ||
      !recovered.quarantined.empty() ||
      !std::filesystem::exists(interruptedJournal) ||
      !std::filesystem::exists(interruptedSidecar)) {
    return false;
  }
  const auto restartedEpoch = restarted.storage->writerRecoveryEpoch();
  if (restarted.storage->retireBattle(interruptedBattle, restartedEpoch - 1U) !=
          StorageError::StaleWriterEpoch ||
      restarted.storage->retireBattle(interruptedBattle, restartedEpoch) !=
          StorageError::None ||
      !std::filesystem::exists(interruptedJournal) ||
      !std::filesystem::exists(interruptedSidecar) ||
      !std::filesystem::exists(interruptedMarker)) {
    return false;
  }
  const auto afterRetire = restarted.storage->scan();
  if (!afterRetire.ok() || !afterRetire.healthy.empty()) {
    return false;
  }
  flipByte(interruptedMarker, -1);
  const auto corruptMarker = restarted.storage->scan();
  if (!corruptMarker.ok() || !corruptMarker.healthy.empty() ||
      corruptMarker.quarantined.size() != 1U ||
      corruptMarker.quarantined.front().identity != interruptedBattle ||
      corruptMarker.quarantined.front().reason !=
          StorageError::JournalCorrupt ||
      !std::filesystem::exists(interruptedJournal) ||
      !std::filesystem::exists(interruptedSidecar)) {
    return false;
  }
  restarted.storage->stop();
  return restarted.storage->retireBattle(normalBattle, restartedEpoch) ==
         StorageError::StorageStopped;
}

bool staleEpochDoesNotWriteAndReportsCompletion() {
  TempDirectory directory;
  const auto key = createKey(directory.path());
  auto opened = ContinuityStorage::open(directory.path(), key);
  if (!opened.ok()) {
    return false;
  }
  CompletionLatch completion;
  const auto battle = identity(1U, 1U);
  auto batch = validBatch(battle, 999U, 1U, 0U, true);
  if (submit(*opened.storage, std::move(batch), completion) !=
      DurableTickSubmitResult::StaleWriterEpoch) {
    return false;
  }
  const auto result = completion.wait();
  const auto *failed = result.has_value()
                           ? std::get_if<DurableTickWriteFailed>(&*result)
                           : nullptr;
  return failed != nullptr && failed->identity == battle &&
         failed->writerRecoveryEpoch == 999U &&
         failed->lastRecordSequence == 3U &&
         failed->failure == DurableTickWriteFailure::StaleWriterEpoch &&
         !std::filesystem::exists(opened.storage->journalPath(battle));
}

bool writerDrainsQueueAndReturnsExactSequence() {
  TempDirectory directory;
  const auto key = createKey(directory.path());
  auto opened = ContinuityStorage::open(directory.path(), key, 4U);
  if (!opened.ok()) {
    return false;
  }
  const auto battle = identity(1U, 1U);
  CompletionLatch first;
  CompletionLatch second;
  if (submit(*opened.storage, validBatch(battle, 1U, 1U, 0U, true), first) !=
          DurableTickSubmitResult::Accepted ||
      submit(*opened.storage, validBatch(battle, 1U, 4U, 1U, false), second) !=
          DurableTickSubmitResult::Accepted) {
    return false;
  }
  opened.storage->stop();
  const auto firstResult = first.wait();
  const auto secondResult = second.wait();
  const auto *firstCommitted =
      firstResult.has_value() ? std::get_if<DurableTickCommitted>(&*firstResult)
                              : nullptr;
  const auto *secondCommitted =
      secondResult.has_value()
          ? std::get_if<DurableTickCommitted>(&*secondResult)
          : nullptr;
  if (firstCommitted == nullptr || secondCommitted == nullptr ||
      firstCommitted->lastRecordSequence != 3U ||
      secondCommitted->lastRecordSequence != 5U) {
    return false;
  }
  const auto file = opened.storage->journalPath(battle);
  const auto read = ::open(file.c_str(), O_RDONLY);
  if (read < 0) {
    return false;
  }
  struct stat status {};
  if (::fstat(read, &status) != 0 || status.st_size <= 0) {
    (void)::close(read);
    return false;
  }
  Bytes bytes(static_cast<std::size_t>(status.st_size));
  const auto readBytes = ::pread(read, bytes.data(), bytes.size(), 0);
  (void)::close(read);
  const auto decoded = lol::battle_continuity::decodeJournal(bytes);
  return readBytes == static_cast<ssize_t>(bytes.size()) && decoded.ok() &&
         decoded.committedBytes == bytes.size() && decoded.records.size() == 5U;
}

bool tornTailIsReportedSeparatelyAndHealthyPrefixSurvives() {
  TempDirectory directory;
  const auto key = createKey(directory.path());
  auto opened = ContinuityStorage::open(directory.path(), key);
  if (!opened.ok()) {
    return false;
  }
  const auto battle = identity(1U, 1U);
  CompletionLatch completion;
  if (submit(*opened.storage, validBatch(battle, 1U, 1U, 0U, true), completion,
             Bytes{0x52U}) != DurableTickSubmitResult::Accepted ||
      !completion.wait().has_value()) {
    return false;
  }
  opened.storage->waitUntilIdle();
  const auto journal = opened.storage->journalPath(battle);
  const std::array<std::uint8_t, 5> torn{0x4CU, 0x42U, 0x43U, 0x31U, 0x00U};
  writeAppend(journal, torn);
  const auto recovered = opened.storage->scan();
  if (!recovered.ok() || recovered.healthy.size() != 1U ||
      !recovered.quarantined.empty() || recovered.repairedTails.size() != 1U) {
    return false;
  }
  const auto read = ::open(journal.c_str(), O_RDONLY);
  struct stat status {};
  if (read < 0 || ::fstat(read, &status) != 0) {
    return false;
  }
  Bytes bytes(static_cast<std::size_t>(status.st_size));
  const auto readBytes = ::pread(read, bytes.data(), bytes.size(), 0);
  (void)::close(read);
  const auto decoded = lol::battle_continuity::decodeJournal(bytes);
  return readBytes == static_cast<ssize_t>(bytes.size()) && decoded.ok() &&
         decoded.committedBytes == bytes.size() &&
         std::filesystem::exists(recovered.repairedTails.front().artifact);
}

bool committedCorruptionIsolatedFromHealthyBattle() {
  TempDirectory directory;
  const auto key = createKey(directory.path());
  auto opened = ContinuityStorage::open(directory.path(), key, 4U);
  if (!opened.ok()) {
    return false;
  }
  const auto corruptBattle = identity(1U, 1U);
  const auto healthyBattle = identity(1U, 2U);
  CompletionLatch corruptCompletion;
  CompletionLatch healthyCompletion;
  if (submit(*opened.storage, validBatch(corruptBattle, 1U, 1U, 0U, true),
             corruptCompletion,
             Bytes{0x61U}) != DurableTickSubmitResult::Accepted ||
      submit(*opened.storage, validBatch(healthyBattle, 1U, 1U, 0U, true),
             healthyCompletion,
             Bytes{0x62U}) != DurableTickSubmitResult::Accepted) {
    return false;
  }
  opened.storage->waitUntilIdle();
  if (!corruptCompletion.wait().has_value() ||
      !healthyCompletion.wait().has_value()) {
    return false;
  }
  flipByte(opened.storage->journalPath(corruptBattle), 68);
  const auto recovered = opened.storage->scan();
  return recovered.ok() && recovered.healthy.size() == 1U &&
         recovered.healthy.front().identity == healthyBattle &&
         recovered.quarantined.size() == 1U;
}

bool committedTickCommitCorruptionIsQuarantined() {
  TempDirectory directory;
  const auto key = createKey(directory.path());
  auto opened = ContinuityStorage::open(directory.path(), key);
  if (!opened.ok()) {
    return false;
  }
  const auto battle = identity(1U, 1U);
  CompletionLatch initialCompletion;
  CompletionLatch secondCompletion;
  if (submit(*opened.storage, validBatch(battle, 1U, 1U, 0U, true),
             initialCompletion,
             Bytes{0x71U}) != DurableTickSubmitResult::Accepted ||
      submit(*opened.storage, validBatch(battle, 1U, 4U, 1U, false),
             secondCompletion) != DurableTickSubmitResult::Accepted) {
    return false;
  }
  opened.storage->waitUntilIdle();
  if (!initialCompletion.wait().has_value() ||
      !secondCompletion.wait().has_value()) {
    return false;
  }
  flipByte(opened.storage->journalPath(battle), -1);
  const auto recovered = opened.storage->scan();
  return recovered.ok() && recovered.healthy.empty() &&
         recovered.quarantined.size() == 1U;
}

bool mismatchedJournalFilenameIsQuarantinedWithoutCanonicalTombstone() {
  TempDirectory directory;
  const auto key = createKey(directory.path());
  auto opened = ContinuityStorage::open(directory.path(), key);
  if (!opened.ok()) {
    return false;
  }
  const auto canonical = identity(1U, 2U);
  const auto filenameIdentity = identity(1U, 1U);
  CompletionLatch completion;
  if (submit(*opened.storage, validBatch(canonical, 1U, 1U, 0U, true),
             completion,
             Bytes{0x73U}) != DurableTickSubmitResult::Accepted ||
      !completion.wait().has_value()) {
    return false;
  }
  const auto canonicalJournal = opened.storage->journalPath(canonical);
  const auto canonicalSidecar = opened.storage->privateEnvelopePath(canonical);
  const auto renamedJournal = opened.storage->journalPath(filenameIdentity);
  const auto renamedSidecar =
      opened.storage->privateEnvelopePath(filenameIdentity);
  std::error_code error;
  std::filesystem::rename(canonicalJournal, renamedJournal, error);
  if (error) {
    return false;
  }
  std::filesystem::rename(canonicalSidecar, renamedSidecar, error);
  if (error) {
    return false;
  }

  const auto recovered = opened.storage->scan();
  const auto canonicalQuarantine = [&canonicalJournal] {
    auto path = canonicalJournal;
    path.replace_extension(".quarantine");
    return path;
  }();
  const auto filenameQuarantine = [&renamedJournal] {
    auto path = renamedJournal;
    path.replace_extension(".quarantine");
    return path;
  }();
  return recovered.ok() && recovered.healthy.empty() &&
         recovered.quarantined.size() == 1U &&
         recovered.quarantined.front().identity == filenameIdentity &&
         std::filesystem::exists(recovered.quarantined.front().artifact) &&
         !std::filesystem::exists(canonicalQuarantine) &&
         !std::filesystem::exists(filenameQuarantine);
}

bool malformedJournalFilenameIsQuarantinedWithoutTombstone() {
  TempDirectory directory;
  const auto key = createKey(directory.path());
  auto opened = ContinuityStorage::open(directory.path(), key);
  if (!opened.ok()) {
    return false;
  }
  const auto canonical = identity(1U, 1U);
  CompletionLatch completion;
  if (submit(*opened.storage, validBatch(canonical, 1U, 1U, 0U, true),
             completion,
             Bytes{0x75U}) != DurableTickSubmitResult::Accepted ||
      !completion.wait().has_value()) {
    return false;
  }
  const auto canonicalJournal = opened.storage->journalPath(canonical);
  const auto malformedJournal = directory.path() / "not-a-battle.journal";
  std::error_code error;
  std::filesystem::rename(canonicalJournal, malformedJournal, error);
  if (error) {
    return false;
  }

  const auto recovered = opened.storage->scan();
  const auto canonicalQuarantine = [&canonicalJournal] {
    auto path = canonicalJournal;
    path.replace_extension(".quarantine");
    return path;
  }();
  return recovered.ok() && recovered.healthy.empty() &&
         recovered.quarantined.size() == 1U &&
         !recovered.quarantined.front().identity.has_value() &&
         std::filesystem::exists(recovered.quarantined.front().artifact) &&
         !std::filesystem::exists(canonicalQuarantine);
}

bool repairedTailWithMismatchedJournalFilenameIsQuarantined() {
  TempDirectory directory;
  const auto key = createKey(directory.path());
  auto opened = ContinuityStorage::open(directory.path(), key);
  if (!opened.ok()) {
    return false;
  }
  const auto canonical = identity(1U, 2U);
  const auto filenameIdentity = identity(1U, 1U);
  CompletionLatch completion;
  if (submit(*opened.storage, validBatch(canonical, 1U, 1U, 0U, true),
             completion,
             Bytes{0x74U}) != DurableTickSubmitResult::Accepted ||
      !completion.wait().has_value()) {
    return false;
  }
  const auto canonicalJournal = opened.storage->journalPath(canonical);
  const auto canonicalSidecar = opened.storage->privateEnvelopePath(canonical);
  const auto renamedJournal = opened.storage->journalPath(filenameIdentity);
  const auto renamedSidecar =
      opened.storage->privateEnvelopePath(filenameIdentity);
  std::error_code error;
  std::filesystem::rename(canonicalJournal, renamedJournal, error);
  if (error) {
    return false;
  }
  std::filesystem::rename(canonicalSidecar, renamedSidecar, error);
  if (error) {
    return false;
  }
  const std::array<std::uint8_t, 5> torn{0x4CU, 0x42U, 0x43U, 0x31U, 0x00U};
  writeAppend(renamedJournal, torn);

  const auto recovered = opened.storage->scan();
  const auto canonicalQuarantine = [&canonicalJournal] {
    auto path = canonicalJournal;
    path.replace_extension(".quarantine");
    return path;
  }();
  return recovered.ok() && recovered.healthy.empty() &&
         recovered.quarantined.size() == 1U &&
         recovered.quarantined.front().identity == filenameIdentity &&
         std::filesystem::exists(recovered.quarantined.front().artifact) &&
         !std::filesystem::exists(canonicalQuarantine);
}

} // namespace

int main() {
  return rejectsUnsafeRootAndKey() && manifestChecksumFailureStopsStartup() &&
                 differentKeyFailsGloballyBeforeEpochAdvance() &&
                 leaseAndEpochPersist() &&
                 sidecarRoundTripsAndTamperIsolated() &&
                 missingSidecarIsQuarantined() &&
                 startupQuarantineTombstoneIsolation() &&
                 retireTombstonePreservesEvidenceAndCrashRecovery() &&
                 staleEpochDoesNotWriteAndReportsCompletion() &&
                 writerDrainsQueueAndReturnsExactSequence() &&
                 tornTailIsReportedSeparatelyAndHealthyPrefixSurvives() &&
                 committedCorruptionIsolatedFromHealthyBattle() &&
                 committedTickCommitCorruptionIsQuarantined() &&
                 malformedJournalFilenameIsQuarantinedWithoutTombstone() &&
                 mismatchedJournalFilenameIsQuarantinedWithoutCanonicalTombstone() &&
                 repairedTailWithMismatchedJournalFilenameIsQuarantined()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
