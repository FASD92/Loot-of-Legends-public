#include <lol/settlement/SettlementCapacityGate.hpp>
#include <lol/settlement_storage/StorageProbe.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using lol::settlement::CapacityReservationCode;
using lol::settlement::OutboxBacklogSnapshot;
using lol::settlement::SettlementCapacityGate;
using lol::settlement::SettlementCapacityReservation;
using lol::settlement_storage::probeJournalDirectory;
using lol::settlement_storage::StorageProbeResult;

class TempDirectory {
public:
  TempDirectory() {
    const auto base =
        (std::filesystem::temp_directory_path() / "loot-capacity-XXXXXX")
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

OutboxBacklogSnapshot healthy(std::uint64_t records = 0u,
                              std::uint64_t bytes = 0u,
                              std::chrono::milliseconds oldest = 0ms) {
  return {
      .unretiredRecords = records,
      .unretiredBytes = bytes,
      .oldestPendingAge = oldest,
      .storageHealthy = true,
  };
}

bool blocksEveryHighWatermarkAndUnhealthyStorage() {
  constexpr std::array snapshots = {
      OutboxBacklogSnapshot{.unretiredRecords = 5'000u,
                            .unretiredBytes = 0u,
                            .oldestPendingAge = 0ms,
                            .storageHealthy = true},
      OutboxBacklogSnapshot{.unretiredRecords = 1u,
                            .unretiredBytes = 64u * 1024u * 1024u,
                            .oldestPendingAge = 1ms,
                            .storageHealthy = true},
      OutboxBacklogSnapshot{.unretiredRecords = 1u,
                            .unretiredBytes = 1u,
                            .oldestPendingAge = 15min,
                            .storageHealthy = true},
      OutboxBacklogSnapshot{.unretiredRecords = 0u,
                            .unretiredBytes = 0u,
                            .oldestPendingAge = 0ms,
                            .storageHealthy = false},
  };

  for (const auto &snapshot : snapshots) {
    SettlementCapacityGate gate;
    gate.updateBacklog(snapshot);
    const auto result = gate.tryReserve();
    if (result.reservation.has_value() ||
        (snapshot.storageHealthy &&
         result.code != CapacityReservationCode::HighWatermark) ||
        (!snapshot.storageHealthy &&
         result.code != CapacityReservationCode::StorageUnhealthy)) {
      return false;
    }
  }
  return true;
}

bool requiresEveryLowWatermarkBeforeRecovery() {
  SettlementCapacityGate gate;
  gate.updateBacklog(healthy(5'000u, 64u * 1024u * 1024u, 15min));
  gate.updateBacklog(healthy(4'001u, 48u * 1024u * 1024u, 9min));
  if (gate.tryReserve().code != CapacityReservationCode::HighWatermark) {
    return false;
  }
  gate.updateBacklog(healthy(4'000u, 48u * 1024u * 1024u, 10min));
  if (gate.tryReserve().code != CapacityReservationCode::HighWatermark) {
    return false;
  }
  gate.updateBacklog(healthy(4'000u, 48u * 1024u * 1024u, 10min - 1ms));
  return gate.tryReserve().code == CapacityReservationCode::Reserved;
}

bool reservationProtectsExistingBattleAndIsBounded() {
  SettlementCapacityGate gate;
  gate.updateBacklog(healthy());
  auto first = gate.tryReserve();
  if (first.code != CapacityReservationCode::Reserved ||
      !first.reservation.has_value()) {
    return false;
  }
  const auto reserved = gate.metrics();
  if (reserved.reservedBatches != 1u || reserved.reservedRecords != 11u ||
      reserved.reservedBytes == 0u) {
    return false;
  }

  gate.updateBacklog(healthy(5'000u, 64u * 1024u * 1024u, 15min));
  if (!first.reservation->valid() ||
      gate.tryReserve().code != CapacityReservationCode::HighWatermark ||
      gate.metrics().reservedBatches != 1u) {
    return false;
  }
  first.reservation.reset();
  if (gate.metrics().reservedBatches != 0u) {
    return false;
  }

  gate.updateBacklog(healthy());
  std::vector<SettlementCapacityReservation> reservations;
  reservations.reserve(128u);
  for (std::size_t index = 0; index < 128u; ++index) {
    auto result = gate.tryReserve();
    if (!result.reservation.has_value()) {
      return false;
    }
    reservations.push_back(std::move(*result.reservation));
  }
  return gate.tryReserve().code ==
             CapacityReservationCode::ReservationCapacityExhausted &&
         gate.metrics().reservedBatches == 128u;
}

bool reservationAccountsForRecordAndByteHeadroom() {
  SettlementCapacityGate recordGate;
  recordGate.updateBacklog(healthy(4'990u, 0u, 1ms));
  if (recordGate.tryReserve().code !=
      CapacityReservationCode::ReservationCapacityExhausted) {
    return false;
  }

  SettlementCapacityGate byteGate;
  byteGate.updateBacklog(healthy(1u, 64u * 1024u * 1024u - 1u, 1ms));
  return byteGate.tryReserve().code ==
         CapacityReservationCode::ReservationCapacityExhausted;
}

bool startupProbeChecksReadWriteSyncAndPathSafety() {
  TempDirectory root;
  const auto journal = root.path() / "journal";
  const auto insecure = root.path() / "insecure";
  const auto link = root.path() / "journal-link";
  if (!std::filesystem::create_directory(journal) ||
      !std::filesystem::create_directory(insecure) ||
      ::chmod(journal.c_str(), S_IRWXU) != 0 ||
      ::chmod(insecure.c_str(),
              S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH) != 0 ||
      ::symlink(journal.c_str(), link.c_str()) != 0) {
    return false;
  }
  return probeJournalDirectory(journal) == StorageProbeResult::Ready &&
         probeJournalDirectory(root.path() / "missing") ==
             StorageProbeResult::MissingDirectory &&
         probeJournalDirectory(link) == StorageProbeResult::UnsafePath &&
         probeJournalDirectory(insecure) ==
             StorageProbeResult::InsecurePermissions;
}

} // namespace

int main() {
  return blocksEveryHighWatermarkAndUnhealthyStorage() &&
                 requiresEveryLowWatermarkBeforeRecovery() &&
                 reservationProtectsExistingBattleAndIsBounded() &&
                 reservationAccountsForRecordAndByteHeadroom() &&
                 startupProbeChecksReadWriteSyncAndPathSafety()
             ? EXIT_SUCCESS
             : EXIT_FAILURE;
}
