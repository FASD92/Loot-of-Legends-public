#pragma once

#include <lol/battle/BattleLoadApi.hpp>
#include <lol/battle_continuity/FlightRecorder.hpp>
#include <lol/battle_continuity/RecoveryEnvelopeCodec.hpp>
#include <lol/lobby_room/RoomApi.hpp>
#include <lol/session/SessionRegistry.hpp>
#include <lol/settlement/SettlementIntent.hpp>
#include <lol/settlement/SettlementPublication.hpp>
#include <lol/shared/Identifiers.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lol::game_flow {

// The composition owner supplies the fresh generation allocated by
// SessionRegistry for each stable participant slot. The mapping is explicit so
// a recovery cannot accidentally apply a generation to another participant.
struct RecoveredSessionGeneration final {
  std::uint16_t participantSlot;
  shared::SessionGeneration generation;

  bool operator==(const RecoveredSessionGeneration &) const = default;
};

struct RecoveredBattleParticipant final {
  std::uint16_t participantSlot;
  shared::AccountId accountId;
  shared::SessionId sessionId;
  shared::SessionGeneration generation;
  std::string nickname;
  battle::ParticipantExitStatus exitStatus;

  bool operator==(const RecoveredBattleParticipant &) const = default;
};

enum class BattleContinuityRecoveryCode : std::uint8_t {
  JournalRejected,
  IdentityMismatch,
  ParticipantMappingMismatch,
  InvalidSessionGeneration,
  InvalidWriterRecoveryEpoch,
  CreatedState,
  LoadCancelledState,
  ResultGenerationFailed,
  TerminalReceiptMissing,
  TerminalReceiptMismatch,
  SettlementBatchInvalid,
  BattleReconstructionFailed,
  RecordingResumeFailed,
  RoomReconstructionFailed,
};

struct BattleContinuityRecoveryError final {
  BattleContinuityRecoveryCode code;
  std::size_t recordIndex{0U};
};

// A verified production Battle is always returned on success. A terminal
// battle with no currently present participants deliberately has no Room: its
// settlement batch remains available for headless exactly-once publication.
struct RecoveredBattleReconstruction final {
  battle::BattleInstance battle;
  battle_continuity::BattleRecording recording;
  std::optional<battle_continuity::RoomRecoveryState> roomRecoveryState;
  std::optional<lobby_room::Room> room;
  std::optional<settlement::SettlementIntentBatch> settlementBatch;
  std::vector<RecoveredBattleParticipant> activeParticipants;
};

struct BattleContinuityRecoveryResult final {
  std::optional<RecoveredBattleReconstruction> recovered;
  std::optional<BattleContinuityRecoveryError> error;

  [[nodiscard]] bool ok() const noexcept { return !error.has_value(); }
};

// Platform storage is adapted to this value before entering game-flow.  The
// game-flow module deliberately does not depend on a storage implementation.
struct StoredBattleRecovery final {
  battle_continuity::BattleIdentity identity;
  battle_continuity::Bytes committedJournal;
  std::optional<battle_continuity::Bytes> privateEnvelope;
};

enum class BattleRecoveryDisposition : std::uint8_t {
  Retire,
  Quarantine,
};

struct BattleRecoveryRequest final {
  StoredBattleRecovery stored;
  BattleRecoveryDisposition disposition;
  std::optional<BattleContinuityRecoveryCode> reason;
};

struct StartupRecoveredBattleInstall final {
  RecoveredBattleReconstruction reconstruction;
  std::vector<session::AuthenticateSessionResult> installedSessions;
  std::chrono::steady_clock::time_point reconnectExpiresAt;
  bool settlementAlreadyDurable{};
};

struct StartupRecoveryInput final {
  std::vector<StoredBattleRecovery> battles;
  std::uint32_t currentWriterRecoveryEpoch;
  std::chrono::steady_clock::time_point reconnectExpiresAt;
  session::SessionRegistry &sessions;
  settlement::SettlementStoragePort &settlementStorage;
  std::function<bool(BattleRecoveryRequest)> dispose;
  std::function<bool(const settlement::SettlementIntentBatch &)>
      settlementDurable;
  std::function<bool(StartupRecoveredBattleInstall)> install;
};

[[nodiscard]] bool recoverBattlesAtStartup(StartupRecoveryInput input);

// Pure game-flow reconstruction seam. It performs no file I/O, session
// allocation, route binding, scheduling, or outbound publication.
[[nodiscard]] BattleContinuityRecoveryResult
reconstructBattle(std::span<const std::uint8_t> committedJournal,
                  const battle_continuity::BattleRecoveryEnvelope &envelope,
                  std::span<const RecoveredSessionGeneration> freshGenerations,
                  std::uint32_t currentWriterRecoveryEpoch);

} // namespace lol::game_flow
