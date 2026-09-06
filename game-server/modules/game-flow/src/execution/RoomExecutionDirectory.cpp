#include "execution/RoomExecutionDirectory.hpp"

#include <utility>

namespace lol::game_flow::execution {

std::optional<RoomDirectoryEntry> RoomExecutionDirectory::create(
    runtime::WorkerPool &workers, runtime::DeadlineScheduler &deadlines,
    lobby_room::Room room, WorkBudget budget,
    RoomExecutionCell::OutcomeSink outcomeSink,
    const GameplayTransportReadinessPort *readiness,
    settlement::SettlementCapacityGate *capacityGate,
    settlement::SettlementStoragePort *storage,
    std::uint32_t writerRecoveryEpoch,
    battle_continuity::DurableTickWritePort *continuityStorage) {
  auto summary = room.summary();
  if (!summary.has_value()) {
    return std::nullopt;
  }

  std::lock_guard lock{mutex_};
  if (entries_.contains(summary->roomId)) {
    return std::nullopt;
  }
  RoomDirectoryEntry entry{
      .cell = RoomExecutionCell::create(workers, deadlines, std::move(room),
                                        budget, std::move(outcomeSink),
                                        readiness, capacityGate, storage,
                                        writerRecoveryEpoch, continuityStorage),
      .summary = std::move(*summary),
  };
  entries_.emplace(entry.summary.roomId, entry);
  hiddenSummaries_.erase(entry.summary.roomId);
  return entry;
}

std::optional<RoomDirectoryEntry> RoomExecutionDirectory::createRecovered(
    runtime::WorkerPool &workers, runtime::DeadlineScheduler &deadlines,
    RecoveredRoomExecutionState recovered, WorkBudget budget,
    RoomExecutionCell::OutcomeSink outcomeSink,
    const GameplayTransportReadinessPort *readiness,
    settlement::SettlementCapacityGate *capacityGate,
    settlement::SettlementStoragePort *storage,
    std::uint32_t writerRecoveryEpoch,
    battle_continuity::DurableTickWritePort *continuityStorage) {
  auto summary = recovered.room.summary();
  if (!summary.has_value()) {
    return std::nullopt;
  }

  std::lock_guard lock{mutex_};
  if (entries_.contains(summary->roomId)) {
    return std::nullopt;
  }
  auto cell = RoomExecutionCell::createRecovered(
      workers, deadlines, std::move(recovered), budget, std::move(outcomeSink),
      readiness, capacityGate, storage, writerRecoveryEpoch, continuityStorage);
  if (!cell.has_value()) {
    return std::nullopt;
  }
  RoomDirectoryEntry entry{
      .cell = std::move(*cell),
      .summary = std::move(*summary),
  };
  entries_.emplace(entry.summary.roomId, entry);
  hiddenSummaries_.erase(entry.summary.roomId);
  return entry;
}

std::optional<RoomDirectoryEntry>
RoomExecutionDirectory::lookup(shared::RoomId roomId) const {
  std::lock_guard lock{mutex_};
  const auto entry = entries_.find(roomId);
  if (entry == entries_.end()) {
    return std::nullopt;
  }
  return entry->second;
}

bool RoomExecutionDirectory::updateSummary(lobby_room::RoomSummary summary) {
  std::lock_guard lock{mutex_};
  const auto entry = entries_.find(summary.roomId);
  if (entry == entries_.end()) {
    return false;
  }
  entry->second.summary = std::move(summary);
  hiddenSummaries_.erase(entry->first);
  return true;
}

bool RoomExecutionDirectory::hideSummary(shared::RoomId roomId) {
  std::lock_guard lock{mutex_};
  if (!entries_.contains(roomId)) {
    return false;
  }
  return hiddenSummaries_.insert(roomId).second;
}

bool RoomExecutionDirectory::remove(shared::RoomId roomId) {
  std::lock_guard lock{mutex_};
  const auto entry = entries_.find(roomId);
  if (entry == entries_.end()) {
    return false;
  }
  // The Cell is logically retired before the entry is erased so that a strong
  // CellHandle surviving the removal cannot authorize further gameplay.
  entry->second.cell->retire();
  hiddenSummaries_.erase(roomId);
  entries_.erase(entry);
  return true;
}

std::vector<lobby_room::RoomSummary> RoomExecutionDirectory::summaries() const {
  std::lock_guard lock{mutex_};
  std::vector<lobby_room::RoomSummary> result;
  result.reserve(entries_.size());
  for (const auto &entry : entries_) {
    if (!hiddenSummaries_.contains(entry.first)) {
      result.push_back(entry.second.summary);
    }
  }
  return result;
}

std::size_t RoomExecutionDirectory::size() const {
  std::lock_guard lock{mutex_};
  return entries_.size();
}

bool RoomExecutionDirectory::activateRecovered() {
  std::vector<CellHandle> cells;
  {
    std::lock_guard lock{mutex_};
    cells.reserve(entries_.size());
    for (const auto &[roomId, entry] : entries_) {
      static_cast<void>(roomId);
      cells.push_back(entry.cell);
    }
  }
  for (const auto &cell : cells) {
    if (!cell->activateRecovered()) {
      return false;
    }
  }
  return true;
}

} // namespace lol::game_flow::execution
