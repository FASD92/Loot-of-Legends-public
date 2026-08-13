#pragma once

#include <lol/shared/Identifiers.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace lol::battle {

enum class BattleLoadState : std::uint8_t {
  Created,
  LoadBarrierOpen,
  GameplayCommitted,
  LoadCancelled,
};

enum class LoadCandidateState : std::uint8_t {
  PendingLoad,
  Ready,
  Disconnected,
  TimedOut,
};

enum class ParticipantExitStatus : std::uint8_t {
  GameplayEligible,
  VoluntaryLeft,
  Disconnected,
  TerminalPresent,
  TerminalExited,
};

struct LoadCandidateProjection final {
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
  LoadCandidateState state;

  bool operator==(const LoadCandidateProjection &) const = default;
};

struct CapturedParticipant final {
  shared::AccountId accountId;
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
  std::string nickname;
  // Current exit status; always kept in sync with the participant gameplay
  // record. The first exit reason is sticky.
  ParticipantExitStatus exitStatus;

  bool operator==(const CapturedParticipant &) const = default;
};

struct BattleLoadProjection final {
  shared::RoomId roomId;
  shared::BattleInstanceId battleId;
  BattleLoadState state;
  std::vector<LoadCandidateProjection> candidates;
  std::vector<CapturedParticipant> capturedParticipants;
};

} // namespace lol::battle
