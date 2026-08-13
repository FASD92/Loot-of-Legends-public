#pragma once

#include "execution/RoomExecutionCell.hpp"

#include <lol/lobby_room/RoomApi.hpp>
#include <lol/runtime/DeadlineScheduler.hpp>
#include <lol/runtime/WorkerPool.hpp>
#include <lol/shared/Identifiers.hpp>

#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

namespace lol::game_flow::execution {

using CellHandle = std::shared_ptr<RoomExecutionCell>;

struct RoomDirectoryEntry final {
  CellHandle cell;
  lobby_room::RoomSummary summary;
};

class RoomExecutionDirectory final {
public:
  [[nodiscard]] std::optional<RoomDirectoryEntry>
  create(runtime::WorkerPool &workers, runtime::DeadlineScheduler &deadlines,
         lobby_room::Room room, WorkBudget budget,
         RoomExecutionCell::OutcomeSink outcomeSink,
         const GameplayTransportReadinessPort *readiness = nullptr,
         settlement::SettlementCapacityGate *capacityGate = nullptr,
         settlement::SettlementStoragePort *storage = nullptr);
  [[nodiscard]] std::optional<RoomDirectoryEntry>
  lookup(shared::RoomId roomId) const;
  [[nodiscard]] bool updateSummary(lobby_room::RoomSummary summary);
  [[nodiscard]] bool hideSummary(shared::RoomId roomId);
  [[nodiscard]] bool remove(shared::RoomId roomId);
  [[nodiscard]] std::vector<lobby_room::RoomSummary> summaries() const;
  [[nodiscard]] std::size_t size() const;

private:
  mutable std::mutex mutex_;
  std::map<shared::RoomId, RoomDirectoryEntry> entries_;
  std::set<shared::RoomId> hiddenSummaries_;
};

} // namespace lol::game_flow::execution
