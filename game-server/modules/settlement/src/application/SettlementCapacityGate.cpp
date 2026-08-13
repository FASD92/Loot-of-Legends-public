#include <lol/settlement/SettlementCapacityGate.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>

namespace lol::settlement {
namespace {

using namespace std::chrono_literals;

constexpr std::uint64_t kMaximumUnretiredRecords = 5'000u;
constexpr std::uint64_t kMaximumUnretiredBytes = 64u * 1024u * 1024u;
constexpr auto kMaximumOldestPendingAge = 15min;
constexpr std::uint64_t kRecoveryLowRecords = 4'000u;
constexpr std::uint64_t kRecoveryLowBytes = 48u * 1024u * 1024u;
constexpr auto kRecoveryLowOldestAge = 10min;
constexpr std::uint64_t kMaximumReservedBatches = 128u;
constexpr std::uint64_t kReservedRecordsPerBattle = 11u;

// V1 has two catalog item IDs. A canonical intent with both ItemDeltas is at
// most 130 bytes; the journal adds 60 bytes per record and a 478-byte commit.
constexpr std::uint64_t kMaximumCanonicalIntentBytes = 130u;
constexpr std::uint64_t kJournalRecordOverhead = 60u;
constexpr std::uint64_t kMaximumIntentsPerBattle = 10u;
constexpr std::uint64_t kBatchCommitBytes = 60u + 18u + 10u * 40u;
constexpr std::uint64_t kReservedBytesPerBattle =
    kMaximumIntentsPerBattle *
        (kJournalRecordOverhead + kMaximumCanonicalIntentBytes) +
    kBatchCommitBytes;

bool atHighWatermark(const OutboxBacklogSnapshot &snapshot) {
  return snapshot.unretiredRecords >= kMaximumUnretiredRecords ||
         snapshot.unretiredBytes >= kMaximumUnretiredBytes ||
         (snapshot.unretiredRecords != 0u &&
          snapshot.oldestPendingAge >= kMaximumOldestPendingAge);
}

bool belowEveryLowWatermark(const OutboxBacklogSnapshot &snapshot) {
  return snapshot.unretiredRecords <= kRecoveryLowRecords &&
         snapshot.unretiredBytes <= kRecoveryLowBytes &&
         (snapshot.unretiredRecords == 0u ||
          snapshot.oldestPendingAge < kRecoveryLowOldestAge);
}

bool wouldExceed(std::uint64_t current, std::uint64_t reserved,
                 std::uint64_t addition, std::uint64_t maximum) {
  return current > maximum || reserved > maximum - current ||
         addition > maximum - current - reserved;
}

} // namespace

namespace detail {

struct SettlementCapacityState {
  mutable std::mutex mutex;
  OutboxBacklogSnapshot backlog;
  std::uint64_t reservedBatches{};
  std::uint64_t reservedRecords{};
  std::uint64_t reservedBytes{};
  bool highWatermarkBlocked{true};
  bool initialized{};
};

} // namespace detail

SettlementCapacityReservation::SettlementCapacityReservation(
    std::shared_ptr<detail::SettlementCapacityState> state)
    : state_(std::move(state)), active_(true) {}

SettlementCapacityReservation::~SettlementCapacityReservation() { release(); }

SettlementCapacityReservation::SettlementCapacityReservation(
    SettlementCapacityReservation &&other) noexcept
    : state_(std::move(other.state_)),
      active_(std::exchange(other.active_, false)) {}

SettlementCapacityReservation &SettlementCapacityReservation::operator=(
    SettlementCapacityReservation &&other) noexcept {
  if (this != &other) {
    release();
    state_ = std::move(other.state_);
    active_ = std::exchange(other.active_, false);
  }
  return *this;
}

bool SettlementCapacityReservation::valid() const noexcept { return active_; }

void SettlementCapacityReservation::release() noexcept {
  if (!active_ || state_ == nullptr) {
    return;
  }
  std::lock_guard lock{state_->mutex};
  state_->reservedBatches -= 1u;
  state_->reservedRecords -= kReservedRecordsPerBattle;
  state_->reservedBytes -= kReservedBytesPerBattle;
  active_ = false;
}

SettlementCapacityGate::SettlementCapacityGate()
    : state_(std::make_shared<detail::SettlementCapacityState>()) {}

void SettlementCapacityGate::updateBacklog(OutboxBacklogSnapshot snapshot) {
  std::lock_guard lock{state_->mutex};
  state_->backlog = snapshot;
  if (!state_->initialized) {
    state_->initialized = true;
    state_->highWatermarkBlocked =
        !snapshot.storageHealthy || atHighWatermark(snapshot);
  } else if (!snapshot.storageHealthy) {
    state_->highWatermarkBlocked = true;
  } else if (state_->highWatermarkBlocked) {
    state_->highWatermarkBlocked = !belowEveryLowWatermark(snapshot);
  } else if (atHighWatermark(snapshot)) {
    state_->highWatermarkBlocked = true;
  }
}

CapacityReservationResult SettlementCapacityGate::tryReserve() {
  std::lock_guard lock{state_->mutex};
  if (!state_->backlog.storageHealthy) {
    return {.code = CapacityReservationCode::StorageUnhealthy,
            .reservation = std::nullopt};
  }
  if (state_->highWatermarkBlocked) {
    return {.code = CapacityReservationCode::HighWatermark,
            .reservation = std::nullopt};
  }
  if (state_->reservedBatches >= kMaximumReservedBatches ||
      wouldExceed(state_->backlog.unretiredRecords, state_->reservedRecords,
                  kReservedRecordsPerBattle, kMaximumUnretiredRecords) ||
      wouldExceed(state_->backlog.unretiredBytes, state_->reservedBytes,
                  kReservedBytesPerBattle, kMaximumUnretiredBytes)) {
    return {.code = CapacityReservationCode::ReservationCapacityExhausted,
            .reservation = std::nullopt};
  }

  state_->reservedBatches += 1u;
  state_->reservedRecords += kReservedRecordsPerBattle;
  state_->reservedBytes += kReservedBytesPerBattle;
  return {
      .code = CapacityReservationCode::Reserved,
      .reservation = SettlementCapacityReservation{state_},
  };
}

SettlementCapacityMetrics SettlementCapacityGate::metrics() const {
  std::lock_guard lock{state_->mutex};
  return {
      .backlog = state_->backlog,
      .reservedBatches = state_->reservedBatches,
      .reservedRecords = state_->reservedRecords,
      .reservedBytes = state_->reservedBytes,
      .highWatermarkBlocked = state_->highWatermarkBlocked,
  };
}

} // namespace lol::settlement
