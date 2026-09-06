#pragma once

#include <lol/battle_continuity/Durability.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace lol::battle_continuity_storage {

inline constexpr std::size_t kDefaultWriterQueueCapacity = 64U;

enum class StorageError : std::uint8_t {
  None,
  InvalidRoot,
  RootNotDirectory,
  RootSymlink,
  RootOwnership,
  LockUnavailable,
  LockIo,
  InvalidKey,
  KeySymlink,
  KeyOwnership,
  KeyPermissions,
  KeyLength,
  ManifestMissing,
  ManifestCorrupt,
  ManifestIo,
  EpochExhausted,
  InvalidRequest,
  StaleWriterEpoch,
  QueueFull,
  StorageStopped,
  JournalIo,
  JournalRejected,
  JournalCorrupt,
  SidecarIo,
  SidecarCorrupt,
  CryptoUnavailable,
  DirectoryIo,
};

struct RecoveredBattle final {
  battle_continuity::BattleIdentity identity;
  battle_continuity::Bytes committedJournal;
  std::optional<battle_continuity::Bytes> privateEnvelope;
};

struct QuarantinedBattle final {
  std::filesystem::path artifact;
  std::optional<battle_continuity::BattleIdentity> identity;
  StorageError reason{StorageError::JournalCorrupt};
};

struct RepairedTail final {
  std::filesystem::path artifact;
  std::optional<battle_continuity::BattleIdentity> identity;
};

struct ScanResult final {
  std::vector<RecoveredBattle> healthy;
  std::vector<QuarantinedBattle> quarantined;
  std::vector<RepairedTail> repairedTails;
  std::optional<StorageError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

class ContinuityStorage;

struct OpenResult final {
  std::unique_ptr<ContinuityStorage> storage;
  StorageError error{StorageError::None};

  [[nodiscard]] bool ok() const noexcept { return storage != nullptr; }
};

class ContinuityStorage final : public battle_continuity::DurableTickWritePort {
public:
  [[nodiscard]] static OpenResult
  open(const std::filesystem::path &root, const std::filesystem::path &keyFile,
       std::size_t queueCapacity = kDefaultWriterQueueCapacity);

  ContinuityStorage(const ContinuityStorage &) = delete;
  ContinuityStorage &operator=(const ContinuityStorage &) = delete;
  ~ContinuityStorage();

  [[nodiscard]] std::uint32_t writerRecoveryEpoch() const noexcept;

  [[nodiscard]] battle_continuity::DurableTickSubmitResult
  submit(battle_continuity::DurableTickWriteRequest request,
         battle_continuity::DurableTickWritePort::CompletionSink completion)
      override;

  // Waits until all accepted requests have completed.  The single storage
  // worker owns journal descriptors and performs the only data sync.
  void waitUntilIdle();

  // Stop is idempotent.  Accepted requests are drained before the worker
  // exits; later requests return StorageStopped.
  void stop() noexcept;

  // Startup-only failure isolation for a battle whose semantic restore has
  // failed.  A durable identity-specific tombstone excludes the original
  // journal from future recovery while preserving journal and sidecar evidence.
  [[nodiscard]] StorageError
  quarantineBattle(const battle_continuity::BattleIdentity &identity,
                   std::uint32_t expectedWriterRecoveryEpoch) noexcept;

  // Runtime/startup terminalization.  A durable identity-specific tombstone
  // is written before returning, while journal and sidecar evidence remain in
  // place for audit and repeated-crash recovery.
  [[nodiscard]] StorageError
  retireBattle(const battle_continuity::BattleIdentity &identity,
               std::uint32_t expectedWriterRecoveryEpoch) noexcept;

  [[nodiscard]] ScanResult scan();

  // These paths are derived solely from the validated stable identity.
  [[nodiscard]] std::filesystem::path
  journalPath(const battle_continuity::BattleIdentity &identity) const;
  [[nodiscard]] std::filesystem::path
  privateEnvelopePath(const battle_continuity::BattleIdentity &identity) const;

private:
  struct Impl;

  explicit ContinuityStorage(std::unique_ptr<Impl> implementation) noexcept;

  std::unique_ptr<Impl> implementation_;
};

} // namespace lol::battle_continuity_storage
