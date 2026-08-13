#include <lol/battle/BattleResult.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace lol::battle {

namespace {

ResultBuildOutcome failure(ResultBuildStatus status) noexcept {
  return ResultBuildOutcome{status, std::nullopt};
}

ResultBuildOutcome built(BattleFinalResult result) {
  return ResultBuildOutcome{ResultBuildStatus::Built, std::move(result)};
}

bool ownedByCaptured(shared::SessionId sessionId,
                     const std::vector<CapturedParticipant> &captured) {
  return std::any_of(captured.begin(), captured.end(),
                     [sessionId](const CapturedParticipant &entry) {
                       return entry.sessionId == sessionId;
                     });
}

// Common resolved-ledger validation: exactly one Drop per captured
// participant, every DropId nonzero and unique, every Drop quantity exactly 1,
// no Drop Available at build time, Claimed has exactly one captured owner and
// Unclaimed none.
bool validResolvedDropLedger(const ResultBuildSource &source) {
  if (source.drops.size() != source.captured.size() || source.drops.empty()) {
    return false;
  }
  for (std::size_t index = 0; index < source.drops.size(); ++index) {
    const auto &drop = source.drops[index];
    if (drop.dropId.value == 0 || drop.quantity != 1 ||
        drop.state == LootDropState::Available) {
      return false;
    }
    if (drop.state == LootDropState::Claimed) {
      if (!drop.owner.has_value() ||
          !ownedByCaptured(*drop.owner, source.captured)) {
        return false;
      }
    } else if (drop.owner.has_value()) {
      // Unclaimed must have no owner.
      return false;
    }
    for (std::size_t other = index + 1; other < source.drops.size(); ++other) {
      if (drop.dropId == source.drops[other].dropId) {
        return false;
      }
    }
  }
  return true;
}

// Every holding must belong to a captured participant with a nonzero quantity.
bool validHoldings(const ResultBuildSource &source) {
  for (const auto &holding : source.holdings) {
    if (!ownedByCaptured(holding.sessionId, source.captured)) {
      return false;
    }
    if (holding.quantity == 0) {
      return false;
    }
  }
  return true;
}

// Every generated Drop item and every holding item must resolve in the
// supplied immutable catalog snapshot. A missing item is a catalog defect, not
// an invariant violation.
bool catalogComplete(const ResultBuildSource &source,
                     const RelicCatalog &catalog) {
  for (const auto &drop : source.drops) {
    if (!catalog.unitValueOf(drop.itemId).has_value()) {
      return false;
    }
  }
  for (const auto &holding : source.holdings) {
    if (!catalog.unitValueOf(holding.itemId).has_value()) {
      return false;
    }
  }
  return true;
}

// Per-participant final asset values from the frozen holdings with checked
// integer multiplication and addition. Returns Built or the failing status and
// fills totals in captured order.
ResultBuildStatus assetTotals(const ResultBuildSource &source,
                              const RelicCatalog &catalog,
                              std::vector<std::uint64_t> &totals) {
  totals.assign(source.captured.size(), 0);
  for (std::size_t index = 0; index < source.captured.size(); ++index) {
    const auto &participant = source.captured[index];
    std::uint64_t total = 0;
    for (const auto &holding : source.holdings) {
      if (holding.sessionId != participant.sessionId) {
        continue;
      }
      const auto unitValue = catalog.unitValueOf(holding.itemId);
      if (!unitValue.has_value()) {
        return ResultBuildStatus::CatalogIncomplete;
      }
      if (*unitValue != 0) {
        if (holding.quantity >
            std::numeric_limits<std::uint64_t>::max() / *unitValue) {
          return ResultBuildStatus::ArithmeticOverflow;
        }
        const auto product = holding.quantity * *unitValue;
        if (total > std::numeric_limits<std::uint64_t>::max() - product) {
          return ResultBuildStatus::ArithmeticOverflow;
        }
        total += product;
      }
    }
    totals[index] = total;
  }
  return ResultBuildStatus::Built;
}

// Economic conservation: for every (SessionId, ItemId), the aggregated
// quantity of Claimed Drops must exactly equal the aggregated holdings, in
// both directions. This runs after the checked arithmetic so a genuine
// overflow is never masked by a ledger mismatch.
bool holdingsConserved(const ResultBuildSource &source) {
  for (const auto &holding : source.holdings) {
    std::uint64_t claimedQuantity = 0;
    for (const auto &drop : source.drops) {
      if (drop.state == LootDropState::Claimed &&
          drop.owner == holding.sessionId && drop.itemId == holding.itemId) {
        claimedQuantity += drop.quantity;
      }
    }
    if (claimedQuantity != holding.quantity) {
      return false;
    }
  }
  for (const auto &drop : source.drops) {
    if (drop.state != LootDropState::Claimed) {
      continue;
    }
    const bool backed =
        std::any_of(source.holdings.begin(), source.holdings.end(),
                    [&drop](const LootHoldingProjection &holding) {
                      return holding.sessionId == drop.owner &&
                             holding.itemId == drop.itemId;
                    });
    if (!backed) {
      return false;
    }
  }
  return true;
}

} // namespace

ParticipantExitStatus
terminalExitStatus(ParticipantExitStatus status) noexcept {
  switch (status) {
  case ParticipantExitStatus::GameplayEligible:
    return ParticipantExitStatus::TerminalPresent;
  case ParticipantExitStatus::VoluntaryLeft:
  case ParticipantExitStatus::Disconnected:
    return ParticipantExitStatus::TerminalExited;
  case ParticipantExitStatus::TerminalPresent:
    return ParticipantExitStatus::TerminalPresent;
  case ParticipantExitStatus::TerminalExited:
    return ParticipantExitStatus::TerminalExited;
  }
  std::terminate();
}

ResultBuildOutcome buildFinalResult(const ResultBuildSource &source,
                                    const RelicCatalog &catalog) {
  if (source.outcome != BattleOutcome::MonsterDefeated &&
      source.outcome != BattleOutcome::CombatTimeout &&
      source.outcome != BattleOutcome::CancelledNoActiveParticipants) {
    return failure(ResultBuildStatus::InvariantBroken);
  }

  // Common source validation: nonzero RoomId/BattleInstanceId and a
  // RelicRuleset captured count (2..10) of nonzero, unique SessionIds.
  if (source.roomId.value() == 0 || source.battleId.value() == 0 ||
      source.captured.size() < RelicRuleset::minCapturedParticipants ||
      source.captured.size() > RelicRuleset::maxCapturedParticipants) {
    return failure(ResultBuildStatus::InvariantBroken);
  }
  for (std::size_t index = 0; index < source.captured.size(); ++index) {
    if (source.captured[index].sessionId.value() == 0) {
      return failure(ResultBuildStatus::InvariantBroken);
    }
    for (std::size_t other = index + 1; other < source.captured.size();
         ++other) {
      if (source.captured[index].sessionId ==
          source.captured[other].sessionId) {
        return failure(ResultBuildStatus::InvariantBroken);
      }
    }
  }

  if (source.outcome == BattleOutcome::CombatTimeout) {
    // CombatTimeout commits immediately without Drop generation: it requires
    // no drops and no holdings, includes every captured participant, and every
    // entry has value 0, null rank, and top false.
    if (!source.drops.empty() || !source.holdings.empty()) {
      return failure(ResultBuildStatus::InvariantBroken);
    }
    std::vector<BattleResultEntry> entries;
    entries.reserve(source.captured.size());
    for (const auto &participant : source.captured) {
      entries.push_back(BattleResultEntry{
          .sessionId = participant.sessionId,
          .nickname = participant.nickname,
          .exitStatus = terminalExitStatus(participant.exitStatus),
          .finalAssetValue = 0,
          .rank = std::nullopt,
          .isTop = false,
      });
    }
    std::sort(entries.begin(), entries.end(),
              [](const BattleResultEntry &lhs, const BattleResultEntry &rhs) {
                return lhs.sessionId < rhs.sessionId;
              });
    return built(BattleFinalResult{
        .roomId = source.roomId,
        .battleId = source.battleId,
        .outcome = BattleOutcome::CombatTimeout,
        .entries = std::move(entries),
    });
  }

  if (source.outcome == BattleOutcome::CancelledNoActiveParticipants) {
    // Every captured participant must already be exited: an input is accepted
    // only when it terminalizes to TerminalExited, and any GameplayEligible or
    // TerminalPresent participant is an inconsistent source.
    for (const auto &participant : source.captured) {
      if (terminalExitStatus(participant.exitStatus) !=
          ParticipantExitStatus::TerminalExited) {
        return failure(ResultBuildStatus::InvariantBroken);
      }
    }
    // Exactly two valid ledger shapes: no loot generated (drops and holdings
    // both empty) or the full resolved ledger (one terminal Drop per captured
    // participant with holdings conserved against the Claimed Drops). A
    // nonempty holdings source without drops, or a nonempty Drop ledger with
    // the wrong count/state/owner/economic backing, is rejected.
    const bool hasLedger = !source.drops.empty();
    if (!hasLedger && !source.holdings.empty()) {
      return failure(ResultBuildStatus::InvariantBroken);
    }
    if (hasLedger) {
      if (!validResolvedDropLedger(source) || !validHoldings(source)) {
        return failure(ResultBuildStatus::InvariantBroken);
      }
      if (!catalogComplete(source, catalog)) {
        return failure(ResultBuildStatus::CatalogIncomplete);
      }
    }
    std::vector<std::uint64_t> totals;
    const auto totalsStatus = assetTotals(source, catalog, totals);
    if (totalsStatus != ResultBuildStatus::Built) {
      return failure(totalsStatus);
    }
    if (hasLedger && !holdingsConserved(source)) {
      return failure(ResultBuildStatus::InvariantBroken);
    }
    std::vector<BattleResultEntry> entries;
    entries.reserve(source.captured.size());
    for (std::size_t index = 0; index < source.captured.size(); ++index) {
      const auto &participant = source.captured[index];
      entries.push_back(BattleResultEntry{
          .sessionId = participant.sessionId,
          .nickname = participant.nickname,
          .exitStatus = ParticipantExitStatus::TerminalExited,
          .finalAssetValue = totals[index],
          .rank = std::nullopt,
          .isTop = false,
      });
    }
    std::sort(entries.begin(), entries.end(),
              [](const BattleResultEntry &lhs, const BattleResultEntry &rhs) {
                return lhs.sessionId < rhs.sessionId;
              });
    return built(BattleFinalResult{
        .roomId = source.roomId,
        .battleId = source.battleId,
        .outcome = BattleOutcome::CancelledNoActiveParticipants,
        .entries = std::move(entries),
    });
  }

  // MonsterDefeated economic source validation: the source must be a resolved
  // loot ledger, not fabricated input. There is exactly one Drop per captured
  // participant; every DropId is nonzero and unique; every Drop quantity is
  // exactly 1; no Drop is Available at build time; Claimed has exactly one
  // owner and Unclaimed none; every Claimed owner is a captured SessionId.
  if (!validResolvedDropLedger(source) || !validHoldings(source)) {
    return failure(ResultBuildStatus::InvariantBroken);
  }
  if (!catalogComplete(source, catalog)) {
    return failure(ResultBuildStatus::CatalogIncomplete);
  }
  std::vector<std::uint64_t> totals;
  const auto totalsStatus = assetTotals(source, catalog, totals);
  if (totalsStatus != ResultBuildStatus::Built) {
    return failure(totalsStatus);
  }
  if (!holdingsConserved(source)) {
    return failure(ResultBuildStatus::InvariantBroken);
  }

  std::vector<BattleResultEntry> entries;
  entries.reserve(source.captured.size());
  for (std::size_t index = 0; index < source.captured.size(); ++index) {
    const auto &participant = source.captured[index];
    entries.push_back(BattleResultEntry{
        .sessionId = participant.sessionId,
        .nickname = participant.nickname,
        .exitStatus = terminalExitStatus(participant.exitStatus),
        .finalAssetValue = totals[index],
        .rank = std::nullopt,
        .isTop = false,
    });
  }

  // Competition ranking: descending finalAssetValue; equal values share the
  // same rank; the next different value gets its one-based sorted position
  // (1,1,3); ties order by numeric SessionId ascending; isTop is rank 1.
  std::sort(entries.begin(), entries.end(),
            [](const BattleResultEntry &lhs, const BattleResultEntry &rhs) {
              if (lhs.finalAssetValue != rhs.finalAssetValue) {
                return lhs.finalAssetValue > rhs.finalAssetValue;
              }
              return lhs.sessionId < rhs.sessionId;
            });
  std::uint32_t rank = 0;
  for (std::size_t index = 0; index < entries.size(); ++index) {
    if (index == 0 ||
        entries[index].finalAssetValue != entries[index - 1].finalAssetValue) {
      rank = static_cast<std::uint32_t>(index + 1);
    }
    entries[index].rank = rank;
    entries[index].isTop = rank == 1;
  }

  return built(BattleFinalResult{
      .roomId = source.roomId,
      .battleId = source.battleId,
      .outcome = BattleOutcome::MonsterDefeated,
      .entries = std::move(entries),
  });
}

} // namespace lol::battle
