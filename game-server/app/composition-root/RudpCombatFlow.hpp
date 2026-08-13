#pragma once

#include "AuthClaimCoordinator.hpp"
#include "RudpMovementFlow.hpp"

#include <lol/battle/CombatApi.hpp>
#include <lol/game_flow/RoomCommandGateway.hpp>
#include <lol/transport/rudp/ReliableQueue.hpp>
#include <lol/transport/rudp/RudpBindingRegistry.hpp>
#include <lol/transport/rudp/RudpCombatCodec.hpp>
#include <lol/transport/rudp/RudpLootCodec.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace lol::app {

enum class RudpCombatSubmitResult {
  Accepted,
  Malformed,
  UnexpectedMessage,
  PeerRejected,
  StaleTransport,
  RoomRejected,
};

struct RudpCombatPollResult final {
  std::vector<EncodedRudpDatagram> transmissions;
  std::vector<RudpPeerFailure> failures;
};

class RudpCombatFlow final {
public:
  RudpCombatFlow(transport::rudp::RudpBindingRegistry &bindings,
                 game_flow::RoomCommandGateway &gateway) noexcept;

  [[nodiscard]] RudpCombatSubmitResult
  submitAttack(std::span<const std::byte> datagram,
               const transport::rudp::RudpEndpoint &endpoint,
               std::chrono::steady_clock::time_point receivedAt);
  [[nodiscard]] RudpCombatSubmitResult
  submitClaimLoot(std::span<const std::byte> datagram,
                  const transport::rudp::RudpEndpoint &endpoint,
                  std::chrono::steady_clock::time_point receivedAt);

  [[nodiscard]] std::size_t discardAcknowledged(std::uint64_t sessionId,
                                                std::uint64_t sessionGeneration,
                                                std::uint32_t transportEpoch,
                                                std::uint32_t ack,
                                                std::uint32_t ackBits);

  [[nodiscard]] RudpCombatPollResult
  pollReliable(std::chrono::steady_clock::time_point now);

  [[nodiscard]] std::vector<EncodedRudpDatagram> takeUnreliableSnapshots();

  void handleCombatOutbound(game_flow::CombatOutboundIntent intent);

  // Composition-root-internal diagnostic accessors. Integration tests observe
  // the cardinality of the two lifecycle-bounded maps so that rebind, ACK,
  // expiry, terminal, teardown, and invalidation paths can prove boundedness.
  // These are not part of any module public API.
  [[nodiscard]] std::size_t reliableStateCount();
  [[nodiscard]] std::size_t snapshotSequenceCount();

private:
  using RudpBattleMessage = std::variant<transport::rudp::RudpCombatMessage,
                                         transport::rudp::RudpLootMessage>;

  struct ReliableKey final {
    std::uint64_t sessionId;
    std::uint64_t sessionGeneration;
    std::uint32_t transportEpoch;

    bool operator<(const ReliableKey &other) const {
      if (sessionId != other.sessionId) {
        return sessionId < other.sessionId;
      }
      if (sessionGeneration != other.sessionGeneration) {
        return sessionGeneration < other.sessionGeneration;
      }
      return transportEpoch < other.transportEpoch;
    }

    bool operator==(const ReliableKey &) const = default;
  };

  struct ReliableState final {
    transport::rudp::ReliableQueue queue;
    transport::rudp::RudpEndpoint endpoint;
  };

  [[nodiscard]] std::uint32_t nextSnapshotSequence(std::uint64_t battleId);
  void enqueueReliable(std::uint64_t sessionId,
                       std::uint64_t expectedGeneration,
                       std::uint16_t messageId,
                       const RudpBattleMessage &message);
  [[nodiscard]] std::optional<std::vector<EncodedRudpDatagram>>
  encodeMonsterSnapshot(
      const battle::CombatProjection &projection,
      std::span<const game_flow::BattleParticipantProjection> participants);
  [[nodiscard]] std::optional<std::vector<EncodedRudpDatagram>>
  encodeLootSnapshot(
      const battle::LootProjection &projection,
      std::span<const game_flow::BattleParticipantProjection> participants);

  // Retires every reliable state for a session that no longer matches the
  // current TransportEpoch/generation identity being (re)bound. Caller holds
  // mutex_.
  void retireReliableStatesLocked(std::uint64_t sessionId,
                                  const ReliableKey &keep);
  void recordPeerFailure(RudpPeerFailure failure);
  void recordPeerFailureLocked(RudpPeerFailure failure);

  // Retires the snapshot-sequence identity for a battle once its terminal
  // outcome has been encoded; no further snapshots are produced for it.
  void retireSnapshotSequence(std::uint64_t battleId);

  transport::rudp::RudpBindingRegistry &bindings_;
  game_flow::RoomCommandGateway &gateway_;
  std::mutex mutex_;
  std::map<ReliableKey, ReliableState> reliableStates_;
  // Session close is idempotent, so only the first current failure per
  // SessionId needs a main-loop handoff; a newer identity replaces stale work.
  std::map<std::uint64_t, RudpPeerFailure> pendingFailures_;
  std::map<std::uint64_t, std::uint32_t> nextSnapshotSequences_;
  std::vector<EncodedRudpDatagram> pendingSnapshots_;
};

} // namespace lol::app
