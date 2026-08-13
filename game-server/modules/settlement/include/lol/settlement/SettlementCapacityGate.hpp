#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

namespace lol::settlement {
namespace detail {
struct SettlementCapacityState;
}

struct OutboxBacklogSnapshot {
  std::uint64_t unretiredRecords{};
  std::uint64_t unretiredBytes{};
  std::chrono::milliseconds oldestPendingAge{};
  bool storageHealthy{};
};

enum class CapacityReservationCode {
  Reserved,
  StorageUnhealthy,
  HighWatermark,
  ReservationCapacityExhausted,
};

class SettlementCapacityReservation {
public:
  SettlementCapacityReservation() = default;
  ~SettlementCapacityReservation();
  SettlementCapacityReservation(SettlementCapacityReservation &&other) noexcept;
  SettlementCapacityReservation &
  operator=(SettlementCapacityReservation &&other) noexcept;

  SettlementCapacityReservation(const SettlementCapacityReservation &) = delete;
  SettlementCapacityReservation &
  operator=(const SettlementCapacityReservation &) = delete;

  bool valid() const noexcept;
  void release() noexcept;

private:
  friend class SettlementCapacityGate;
  explicit SettlementCapacityReservation(
      std::shared_ptr<detail::SettlementCapacityState> state);

  std::shared_ptr<detail::SettlementCapacityState> state_;
  bool active_{};
};

struct CapacityReservationResult {
  CapacityReservationCode code{CapacityReservationCode::StorageUnhealthy};
  std::optional<SettlementCapacityReservation> reservation;
};

struct SettlementCapacityMetrics {
  OutboxBacklogSnapshot backlog;
  std::uint64_t reservedBatches{};
  std::uint64_t reservedRecords{};
  std::uint64_t reservedBytes{};
  bool highWatermarkBlocked{};
};

class SettlementCapacityGate {
public:
  SettlementCapacityGate();

  void updateBacklog(OutboxBacklogSnapshot snapshot);
  CapacityReservationResult tryReserve();
  SettlementCapacityMetrics metrics() const;

private:
  std::shared_ptr<detail::SettlementCapacityState> state_;
};

} // namespace lol::settlement
