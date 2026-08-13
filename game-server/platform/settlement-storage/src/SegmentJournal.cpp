#include <lol/settlement_storage/SegmentJournal.hpp>

#include "JournalCodec.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lol::settlement_storage {
namespace {

constexpr std::uintmax_t kSegmentTargetBytes = 16u * 1024u * 1024u;

int openFlags(int flags) {
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  return flags;
}

bool isRegularFile(int descriptor) {
  struct stat status {};
  return ::fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode);
}

bool writeAll(int descriptor, std::span<const std::uint8_t> bytes) {
  std::size_t written{};
  while (written < bytes.size()) {
    const auto result =
        ::write(descriptor, bytes.data() + written, bytes.size() - written);
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return false;
    }
    written += static_cast<std::size_t>(result);
  }
  return true;
}

bool syncData(int descriptor) {
#ifdef __APPLE__
  return ::fsync(descriptor) == 0;
#else
  return ::fdatasync(descriptor) == 0;
#endif
}

bool syncDirectory(const std::filesystem::path &filePath) {
  const auto directory = filePath.parent_path().empty()
                             ? std::filesystem::path{"."}
                             : filePath.parent_path();
  auto flags = O_RDONLY;
#ifdef O_DIRECTORY
  flags |= O_DIRECTORY;
#endif
  const auto descriptor = ::open(directory.c_str(), openFlags(flags));
  if (descriptor < 0) {
    return false;
  }
  const auto synced = ::fsync(descriptor) == 0;
  const auto closed = ::close(descriptor) == 0;
  return synced && closed;
}

std::optional<std::vector<std::uint8_t>> readAll(int descriptor) {
  struct stat status {};
  if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 0 ||
      static_cast<std::uintmax_t>(status.st_size) >
          std::numeric_limits<std::size_t>::max()) {
    return std::nullopt;
  }

  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(status.st_size));
  std::size_t read{};
  while (read < bytes.size()) {
    const auto result = ::pread(descriptor, bytes.data() + read,
                                bytes.size() - read, static_cast<off_t>(read));
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result <= 0) {
      return std::nullopt;
    }
    read += static_cast<std::size_t>(result);
  }
  return bytes;
}

bool nonzero(const JournalBatchId &id) {
  return std::any_of(id.begin(), id.end(),
                     [](std::uint8_t value) { return value != 0u; });
}

AppendResult failed(AppendResultStatus status) {
  return {.status = status, .commitSequence = std::nullopt};
}

std::filesystem::path manifestPath(const std::filesystem::path &base) {
  return std::filesystem::path{base.string() + ".manifest"};
}

std::optional<std::uint64_t> parseUnsigned(std::string_view text) {
  if (text.empty() || std::ranges::any_of(text, [](char value) {
        return value < '0' || value > '9';
      })) {
    return std::nullopt;
  }
  try {
    std::size_t consumed{};
    const auto value = std::stoull(std::string{text}, &consumed);
    if (consumed != text.size()) {
      return std::nullopt;
    }
    return value;
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

std::optional<std::string> prefixed(std::string_view line,
                                    std::string_view prefix) {
  if (!line.starts_with(prefix)) {
    return std::nullopt;
  }
  return std::string{line.substr(prefix.size())};
}

bool appendCompactedBatch(std::vector<std::uint8_t> &bytes,
                          const RecoveredBatch &batch) {
  const auto intentCount =
      static_cast<std::uint64_t>(batch.canonicalIntents.size());
  if (batch.commitSequence <= intentCount) {
    return false;
  }
  std::vector<std::uint8_t> commitPayload;
  commitPayload.reserve(18u + batch.canonicalIntents.size() * 40u);
  commitPayload.insert(commitPayload.end(), batch.batchId.begin(),
                       batch.batchId.end());
  detail::appendU16(commitPayload,
                    static_cast<std::uint16_t>(batch.canonicalIntents.size()));
  auto intentSequence = batch.commitSequence - intentCount;
  for (const auto &intent : batch.canonicalIntents) {
    const auto record = detail::encodeRecord(detail::JournalRecordType::Intent,
                                             intentSequence, intent);
    if (record.empty()) {
      return false;
    }
    bytes.insert(bytes.end(), record.begin(), record.end());
    detail::appendU64(commitPayload, intentSequence);
    const auto hash = detail::sha256(intent);
    commitPayload.insert(commitPayload.end(), hash.begin(), hash.end());
    ++intentSequence;
  }
  const auto commit =
      detail::encodeRecord(detail::JournalRecordType::BatchCommit,
                           batch.commitSequence, commitPayload);
  if (commit.empty()) {
    return false;
  }
  bytes.insert(bytes.end(), commit.begin(), commit.end());
  return true;
}

struct TemporaryFile final {
  std::filesystem::path path;
  int descriptor{-1};
};

std::optional<TemporaryFile>
createTemporary(const std::filesystem::path &patternPath) {
  std::string pattern = patternPath.string() + ".XXXXXX";
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const auto descriptor = ::mkstemp(writable.data());
  if (descriptor < 0 || !isRegularFile(descriptor)) {
    if (descriptor >= 0) {
      ::close(descriptor);
      ::unlink(writable.data());
    }
    return std::nullopt;
  }
  return TemporaryFile{.path = writable.data(), .descriptor = descriptor};
}

} // namespace

SegmentJournal::SegmentJournal(std::filesystem::path path,
                               AppendFaultInjector *faultInjector)
    : path_(std::move(path)), activePath_(path_),
      faultInjector_(faultInjector) {}

bool SegmentJournal::resolveActivePath() {
  const auto manifest = manifestPath(path_);
  const auto descriptor = ::open(manifest.c_str(), openFlags(O_RDONLY));
  if (descriptor < 0 && errno == ENOENT) {
    activePath_ = path_;
    generation_ = 0u;
    compactedThrough_ = 0u;
    nextSequence_ = 1u;
    return true;
  }
  if (descriptor < 0 || !isRegularFile(descriptor)) {
    if (descriptor >= 0) {
      ::close(descriptor);
    }
    return false;
  }
  const auto bytes = readAll(descriptor);
  const auto closed = ::close(descriptor) == 0;
  if (!bytes.has_value() || !closed) {
    return false;
  }
  const std::string text(bytes->begin(), bytes->end());
  std::istringstream stream{text};
  std::array<std::string, 5> lines;
  for (auto &line : lines) {
    if (!std::getline(stream, line)) {
      return false;
    }
  }
  std::string extra;
  if (std::getline(stream, extra) || lines[0] != "LOOT-OUTBOX-MANIFEST-V1") {
    return false;
  }
  const auto generationText = prefixed(lines[1], "generation=");
  const auto activeText = prefixed(lines[2], "active=");
  const auto nextText = prefixed(lines[3], "nextSequence=");
  const auto compactedText = prefixed(lines[4], "compactedThrough=");
  if (!generationText.has_value() || !activeText.has_value() ||
      !nextText.has_value() || !compactedText.has_value()) {
    return false;
  }
  const auto generation = parseUnsigned(*generationText);
  const auto next = parseUnsigned(*nextText);
  const auto compacted = parseUnsigned(*compactedText);
  const std::filesystem::path activeName{*activeText};
  if (!generation.has_value() || *generation == 0u || !next.has_value() ||
      *next == 0u || !compacted.has_value() || activeName.empty() ||
      activeName.has_parent_path() || activeName.filename() != activeName) {
    return false;
  }
  const auto resolved = path_.parent_path() / activeName;
  const auto activeDescriptor = ::open(resolved.c_str(), openFlags(O_RDONLY));
  if (activeDescriptor < 0 || !isRegularFile(activeDescriptor)) {
    if (activeDescriptor >= 0) {
      ::close(activeDescriptor);
    }
    return false;
  }
  if (::close(activeDescriptor) != 0) {
    return false;
  }
  activePath_ = resolved;
  generation_ = *generation;
  nextSequence_ = *next;
  compactedThrough_ = *compacted;
  return true;
}

std::optional<JournalRecoveryResult> SegmentJournal::recoverAndRepair() {
  if (!resolveActivePath()) {
    recovered_ = true;
    healthy_ = false;
    return std::nullopt;
  }
  const auto descriptor = ::open(
      activePath_.c_str(), openFlags(O_RDWR | O_CREAT), S_IRUSR | S_IWUSR);
  if (descriptor < 0 || !isRegularFile(descriptor) ||
      !syncDirectory(activePath_)) {
    if (descriptor >= 0) {
      ::close(descriptor);
    }
    recovered_ = true;
    healthy_ = false;
    return std::nullopt;
  }

  const auto bytes = readAll(descriptor);
  if (!bytes.has_value()) {
    ::close(descriptor);
    recovered_ = true;
    healthy_ = false;
    return std::nullopt;
  }
  auto recovery = recoverJournal(*bytes);

  if (recovery.quarantineOffset.has_value()) {
    const auto offset = *recovery.quarantineOffset;
    const auto quarantinePath = std::filesystem::path{
        activePath_.string() + ".quarantine." + std::to_string(bytes->size()) +
        "." + std::to_string(offset)};
    const auto quarantineDescriptor =
        ::open(quarantinePath.c_str(), openFlags(O_WRONLY | O_CREAT | O_EXCL),
               S_IRUSR | S_IWUSR);
    const auto tail = std::span<const std::uint8_t>{*bytes}.subspan(offset);
    const auto quarantineWritten =
        quarantineDescriptor >= 0 && isRegularFile(quarantineDescriptor) &&
        writeAll(quarantineDescriptor, tail) && syncData(quarantineDescriptor);
    const auto quarantineClosed =
        quarantineDescriptor >= 0 && ::close(quarantineDescriptor) == 0;
    if (!quarantineWritten || !quarantineClosed ||
        !syncDirectory(quarantinePath) ||
        ::ftruncate(descriptor, static_cast<off_t>(recovery.durableBytes)) !=
            0 ||
        !syncData(descriptor)) {
      ::close(descriptor);
      recovered_ = true;
      healthy_ = false;
      return std::nullopt;
    }
  }

  if (::close(descriptor) != 0) {
    recovered_ = true;
    healthy_ = false;
    return std::nullopt;
  }
  recovered_ = true;
  healthy_ = recovery.status != JournalRecoveryStatus::CorruptTailQuarantined;
  nextSequence_ = std::max(nextSequence_, recovery.lastSequence + 1u);
  return recovery;
}

AppendResult SegmentJournal::append(const AppendBatch &batch) {
  if ((!recovered_ && !recoverAndRepair().has_value()) || !healthy_) {
    return failed(AppendResultStatus::StorageUnavailable);
  }
  if (!nonzero(batch.batchId) || batch.canonicalIntents.size() < 2u ||
      batch.canonicalIntents.size() > 10u ||
      std::any_of(batch.canonicalIntents.begin(), batch.canonicalIntents.end(),
                  [](const auto &payload) { return payload.empty(); })) {
    return failed(AppendResultStatus::InvalidBatch);
  }

  std::vector<std::vector<std::uint8_t>> records;
  records.reserve(batch.canonicalIntents.size() + 1u);
  std::vector<std::uint8_t> commitPayload;
  commitPayload.reserve(18u + batch.canonicalIntents.size() * 40u);
  commitPayload.insert(commitPayload.end(), batch.batchId.begin(),
                       batch.batchId.end());
  detail::appendU16(commitPayload,
                    static_cast<std::uint16_t>(batch.canonicalIntents.size()));

  auto sequence = nextSequence_;
  for (const auto &intent : batch.canonicalIntents) {
    auto record = detail::encodeRecord(detail::JournalRecordType::Intent,
                                       sequence, intent);
    if (record.empty()) {
      return failed(AppendResultStatus::InvalidBatch);
    }
    records.push_back(std::move(record));
    detail::appendU64(commitPayload, sequence);
    const auto hash = detail::sha256(intent);
    commitPayload.insert(commitPayload.end(), hash.begin(), hash.end());
    ++sequence;
  }
  const auto commitSequence = sequence;
  auto commit = detail::encodeRecord(detail::JournalRecordType::BatchCommit,
                                     commitSequence, commitPayload);
  if (commit.empty()) {
    return failed(AppendResultStatus::InvalidBatch);
  }
  records.push_back(std::move(commit));

  const auto descriptor =
      ::open(activePath_.c_str(), openFlags(O_WRONLY | O_APPEND));
  if (descriptor < 0 || !isRegularFile(descriptor)) {
    if (descriptor >= 0) {
      ::close(descriptor);
    }
    healthy_ = false;
    return failed(AppendResultStatus::StorageUnavailable);
  }

  const auto injected = [&](AppendStage stage, std::size_t recordIndex) {
    return faultInjector_ != nullptr &&
           faultInjector_->shouldFail(stage, recordIndex);
  };
  for (std::size_t index = 0; index < records.size(); ++index) {
    if (injected(AppendStage::BeforeRecordWrite, index)) {
      ::close(descriptor);
      healthy_ = false;
      return failed(AppendResultStatus::InjectedFailure);
    }
    if (!writeAll(descriptor, records[index])) {
      ::close(descriptor);
      healthy_ = false;
      return failed(AppendResultStatus::StorageUnavailable);
    }
    if (injected(AppendStage::AfterRecordWrite, index)) {
      ::close(descriptor);
      healthy_ = false;
      return failed(AppendResultStatus::InjectedFailure);
    }
  }

  const auto syncIndex = records.size();
  if (injected(AppendStage::BeforeDataSync, syncIndex)) {
    ::close(descriptor);
    healthy_ = false;
    return failed(AppendResultStatus::InjectedFailure);
  }
  if (!syncData(descriptor)) {
    ::close(descriptor);
    healthy_ = false;
    return failed(AppendResultStatus::StorageUnavailable);
  }
  if (injected(AppendStage::AfterDataSync, syncIndex)) {
    ::close(descriptor);
    healthy_ = false;
    return failed(AppendResultStatus::InjectedFailure);
  }
  if (::close(descriptor) != 0) {
    healthy_ = false;
    return failed(AppendResultStatus::StorageUnavailable);
  }

  nextSequence_ = commitSequence + 1u;
  return {
      .status = AppendResultStatus::DurablyQueued,
      .commitSequence = commitSequence,
  };
}

settlement::OutboxLoadResult SegmentJournal::nextUnretired() {
  const auto recovered = recoverAndRepair();
  if (!recovered.has_value() || !healthy_) {
    return {.status = settlement::OutboxLoadStatus::Unavailable,
            .batch = std::nullopt};
  }
  const auto batch = std::ranges::find_if(
      recovered->batches,
      [](const RecoveredBatch &candidate) { return !candidate.retired; });
  if (batch == recovered->batches.end()) {
    return {.status = settlement::OutboxLoadStatus::Empty,
            .batch = std::nullopt};
  }
  std::vector<settlement::DurableSettlementIntent> intents;
  intents.reserve(batch->canonicalIntents.size());
  for (const auto &payload : batch->canonicalIntents) {
    intents.push_back(
        settlement::DurableSettlementIntent{.canonicalPayload = payload});
  }
  return {
      .status = settlement::OutboxLoadStatus::Loaded,
      .batch =
          settlement::DurableSettlementBatch{
              .batchId = settlement::SettlementBatchId{batch->batchId},
              .commitSequence = batch->commitSequence,
              .intents = std::move(intents),
          },
  };
}

settlement::OutboxRetireResult
SegmentJournal::retire(const settlement::SettlementBatchId &batchId,
                       std::uint64_t commitSequence) {
  const auto recovered = recoverAndRepair();
  if (!recovered.has_value() || !healthy_) {
    return settlement::OutboxRetireResult::Unavailable;
  }
  const auto batch = std::ranges::find_if(
      recovered->batches, [&](const RecoveredBatch &candidate) {
        return candidate.batchId == batchId.bytes();
      });
  if (batch == recovered->batches.end()) {
    return commitSequence != 0u && commitSequence <= compactedThrough_
               ? settlement::OutboxRetireResult::AlreadyRetired
               : settlement::OutboxRetireResult::NotFound;
  }
  if (commitSequence != batch->commitSequence) {
    return settlement::OutboxRetireResult::NotFound;
  }
  if (batch->retired) {
    return settlement::OutboxRetireResult::AlreadyRetired;
  }

  std::vector<std::uint8_t> payload;
  payload.reserve(24u);
  payload.insert(payload.end(), batch->batchId.begin(), batch->batchId.end());
  detail::appendU64(payload, batch->commitSequence);
  const auto record = detail::encodeRecord(detail::JournalRecordType::Retired,
                                           nextSequence_, payload);
  if (record.empty()) {
    return settlement::OutboxRetireResult::Unavailable;
  }
  const auto descriptor =
      ::open(activePath_.c_str(), openFlags(O_WRONLY | O_APPEND));
  if (descriptor < 0 || !isRegularFile(descriptor)) {
    if (descriptor >= 0) {
      ::close(descriptor);
    }
    healthy_ = false;
    return settlement::OutboxRetireResult::Unavailable;
  }
  const auto durable = writeAll(descriptor, record) && syncData(descriptor);
  const auto closed = ::close(descriptor) == 0;
  if (!durable || !closed) {
    healthy_ = false;
    return settlement::OutboxRetireResult::Unavailable;
  }
  ++nextSequence_;
  return settlement::OutboxRetireResult::Retired;
}

bool SegmentJournal::compact() {
  const auto recovered = recoverAndRepair();
  if (!recovered.has_value() || !healthy_) {
    return false;
  }

  const auto retiredCount = static_cast<std::size_t>(std::ranges::count_if(
      recovered->batches,
      [](const RecoveredBatch &batch) { return batch.retired; }));
  std::error_code sizeError;
  const auto activeBytes = std::filesystem::file_size(activePath_, sizeError);
  if (sizeError) {
    return false;
  }
  if (retiredCount == 0u || (retiredCount * 2u < recovered->batches.size() &&
                             activeBytes < kSegmentTargetBytes)) {
    return true;
  }

  std::vector<std::uint8_t> compacted;
  for (const auto &batch : recovered->batches) {
    if (!batch.retired && !appendCompactedBatch(compacted, batch)) {
      return false;
    }
  }
  const auto verified = recoverJournal(compacted);
  if (verified.status != JournalRecoveryStatus::Clean ||
      verified.batches.size() != recovered->batches.size() - retiredCount) {
    return false;
  }

  const auto newGeneration = generation_ + 1u;
  const auto segmentPrefix =
      path_.parent_path() / (path_.filename().string() + ".generation." +
                             std::to_string(newGeneration));
  auto segment = createTemporary(segmentPrefix);
  if (!segment.has_value()) {
    return false;
  }
  const auto segmentWritten =
      writeAll(segment->descriptor, compacted) && syncData(segment->descriptor);
  const auto segmentClosed = ::close(segment->descriptor) == 0;
  segment->descriptor = -1;
  if (!segmentWritten || !segmentClosed || !syncDirectory(segment->path)) {
    ::unlink(segment->path.c_str());
    return false;
  }

  const auto newCompactedThrough =
      std::max(compactedThrough_, recovered->lastSequence);
  const std::string manifest =
      "LOOT-OUTBOX-MANIFEST-V1\n" + std::string{"generation="} +
      std::to_string(newGeneration) + "\n" +
      "active=" + segment->path.filename().string() + "\n" +
      "nextSequence=" + std::to_string(nextSequence_) + "\n" +
      "compactedThrough=" + std::to_string(newCompactedThrough) + "\n";
  auto manifestTemporary = createTemporary(
      std::filesystem::path{manifestPath(path_).string() + ".tmp"});
  if (!manifestTemporary.has_value()) {
    ::unlink(segment->path.c_str());
    return false;
  }
  const auto manifestBytes = std::span{
      reinterpret_cast<const std::uint8_t *>(manifest.data()), manifest.size()};
  const auto manifestWritten =
      writeAll(manifestTemporary->descriptor, manifestBytes) &&
      syncData(manifestTemporary->descriptor);
  const auto manifestClosed = ::close(manifestTemporary->descriptor) == 0;
  manifestTemporary->descriptor = -1;
  if (!manifestWritten || !manifestClosed) {
    ::unlink(manifestTemporary->path.c_str());
    ::unlink(segment->path.c_str());
    return false;
  }
  if (::rename(manifestTemporary->path.c_str(), manifestPath(path_).c_str()) !=
      0) {
    ::unlink(manifestTemporary->path.c_str());
    ::unlink(segment->path.c_str());
    return false;
  }
  if (!syncDirectory(manifestPath(path_))) {
    healthy_ = false;
    return false;
  }

  const auto previous = activePath_;
  activePath_ = segment->path;
  generation_ = newGeneration;
  compactedThrough_ = newCompactedThrough;
  recovered_ = true;
  healthy_ = true;
  if (previous != activePath_) {
    std::error_code removalError;
    std::filesystem::remove(previous, removalError);
    if (removalError || !syncDirectory(activePath_)) {
      healthy_ = false;
      return false;
    }
  }
  return true;
}

bool SegmentJournal::healthy() const noexcept { return healthy_; }

} // namespace lol::settlement_storage
