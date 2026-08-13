#include "RudpCombatFlow.hpp"

#include <lol/shared/Identifiers.hpp>

#include <exception>
#include <type_traits>
#include <utility>

namespace lol::app {
namespace {

constexpr std::uint16_t kAttackTerminalResultMessageId = 28;
constexpr std::uint16_t kMonsterSpawnedMessageId = 29;
constexpr std::uint16_t kCombatTerminalEventMessageId = 30;
constexpr std::uint16_t kMonsterStateSnapshotMessageId = 31;
constexpr std::uint16_t kClaimLootTerminalResultMessageId = 33;
constexpr std::uint16_t kDropSpawnedMessageId = 34;
constexpr std::uint16_t kDropStateSnapshotMessageId = 35;

transport::rudp::RudpAttackTerminalResult
toRudpResult(const battle::AttackTerminalResult &result) {
  return {.commandId =
              transport::rudp::RudpCommandId{.high = result.commandId.high,
                                             .low = result.commandId.low},
          .battleInstanceId = result.battleId.value(),
          .resultCode = static_cast<transport::rudp::RudpAttackResultCode>(
              static_cast<std::uint16_t>(result.code)),
          .monsterId = result.monsterId,
          .remainingHitPoints = result.remainingHitPoints,
          .rulesetVersion = result.rulesetVersion,
          .combatOutcome = static_cast<transport::rudp::RudpCombatOutcome>(
              static_cast<std::uint8_t>(result.outcome))};
}

transport::rudp::RudpMonsterSpawned
toRudpSpawned(const game_flow::CombatMonsterSpawnedOutbound &spawned) {
  return {
      .eventId = transport::rudp::RudpEventId{.high = spawned.battleId.value(),
                                              .low = 1},
      .battleInstanceId = spawned.battleId.value(),
      .eventStreamKind = transport::rudp::RudpEventStreamKind::CombatLifecycle,
      .eventSequence = 1,
      .monsterId = battle::CombatRuleset::monsterId,
      .posXMillimeter = battle::CombatRuleset::spawnPosition.xMillimeter,
      .posYMillimeter = battle::CombatRuleset::spawnPosition.yMillimeter,
      .maximumHitPoints = battle::CombatRuleset::monsterHitPoints,
      .rulesetVersion = battle::CombatRuleset::version};
}

transport::rudp::RudpCombatTerminalEvent
toRudpTerminal(const battle::CombatTerminalRecord &terminal) {
  return {.eventId = transport::rudp::RudpEventId{.high = terminal.eventId.high,
                                                  .low = terminal.eventId.low},
          .battleInstanceId = terminal.battleId.value(),
          .eventStreamKind =
              transport::rudp::RudpEventStreamKind::CombatLifecycle,
          .eventSequence = terminal.eventSequence,
          .combatOutcome = static_cast<transport::rudp::RudpCombatOutcome>(
              static_cast<std::uint8_t>(terminal.outcome)),
          .monsterId = terminal.monsterId,
          .serverTick = terminal.serverTick,
          .rulesetVersion = terminal.rulesetVersion};
}

transport::rudp::RudpClaimLootTerminalResult
toRudpLootResult(const battle::ClaimLootTerminalResult &result) {
  const auto resultCode = [&result] {
    switch (result.code) {
    case battle::ClaimLootResultCode::Ok:
      return transport::rudp::RudpClaimLootResultCode::Ok;
    case battle::ClaimLootResultCode::NotEligible:
      return transport::rudp::RudpClaimLootResultCode::NotEligible;
    case battle::ClaimLootResultCode::StaleSession:
      return transport::rudp::RudpClaimLootResultCode::StaleSession;
    case battle::ClaimLootResultCode::StaleBattle:
      return transport::rudp::RudpClaimLootResultCode::StaleBattle;
    case battle::ClaimLootResultCode::InvalidDrop:
      return transport::rudp::RudpClaimLootResultCode::InvalidDrop;
    case battle::ClaimLootResultCode::UnknownDrop:
      return transport::rudp::RudpClaimLootResultCode::UnknownDrop;
    case battle::ClaimLootResultCode::OutOfRange:
      return transport::rudp::RudpClaimLootResultCode::OutOfRange;
    case battle::ClaimLootResultCode::AlreadyClaimed:
      return transport::rudp::RudpClaimLootResultCode::AlreadyClaimed;
    case battle::ClaimLootResultCode::Overloaded:
      return transport::rudp::RudpClaimLootResultCode::Overloaded;
    case battle::ClaimLootResultCode::CommandConflict:
      return transport::rudp::RudpClaimLootResultCode::CommandConflict;
    case battle::ClaimLootResultCode::CatalogRejected:
      return transport::rudp::RudpClaimLootResultCode::CatalogRejected;
    case battle::ClaimLootResultCode::ResolutionClosed:
      return transport::rudp::RudpClaimLootResultCode::ResolutionClosed;
    }
    std::terminate();
  }();
  return {
      .commandId = {.high = result.commandId.high, .low = result.commandId.low},
      .battleInstanceId = result.battleId.value(),
      .dropId = result.dropId.value,
      .resultCode = resultCode,
  };
}

transport::rudp::RudpDropSpawned
toRudpDropSpawned(shared::BattleInstanceId battleId,
                  const battle::LootDropProjection &drop) {
  return {
      .eventId = {.high = battleId.value(), .low = drop.dropId.value},
      .battleInstanceId = battleId.value(),
      .eventStreamKind = transport::rudp::RudpEventStreamKind::LootLifecycle,
      .eventSequence = static_cast<std::uint32_t>(drop.dropId.value),
      .dropId = drop.dropId.value,
      .itemId = drop.itemId.value,
      .quantity = drop.quantity,
      .posXMillimeter = drop.position.xMillimeter,
      .posYMillimeter = drop.position.yMillimeter,
      .rulesetVersion = battle::RelicRuleset::version,
  };
}

transport::rudp::RudpDropStateSnapshot
toRudpLootSnapshot(const battle::LootProjection &projection,
                   std::uint32_t sequence) {
  const auto resolutionState = [&projection] {
    switch (projection.resolution) {
    case battle::LootResolutionState::NotStarted:
      return transport::rudp::RudpLootResolutionState::NotStarted;
    case battle::LootResolutionState::Open:
      return transport::rudp::RudpLootResolutionState::Open;
    case battle::LootResolutionState::Resolved:
      return transport::rudp::RudpLootResolutionState::Resolved;
    }
    std::terminate();
  }();
  transport::rudp::RudpDropStateSnapshot snapshot{
      .battleInstanceId = projection.battleId.value(),
      .snapshotSequence = sequence,
      .resolutionState = resolutionState,
      .drops = {},
  };
  snapshot.drops.reserve(projection.drops.size());
  for (const auto &drop : projection.drops) {
    const auto state = [&drop] {
      switch (drop.state) {
      case battle::LootDropState::Available:
        return transport::rudp::RudpLootDropState::Available;
      case battle::LootDropState::Claimed:
        return transport::rudp::RudpLootDropState::Claimed;
      case battle::LootDropState::Unclaimed:
        return transport::rudp::RudpLootDropState::Unclaimed;
      }
      std::terminate();
    }();
    snapshot.drops.push_back(transport::rudp::RudpLootDropProjection{
        .dropId = drop.dropId.value,
        .itemId = drop.itemId.value,
        .quantity = drop.quantity,
        .posXMillimeter = drop.position.xMillimeter,
        .posYMillimeter = drop.position.yMillimeter,
        .state = state,
        .ownerSessionId = drop.owner.has_value() ? drop.owner->value() : 0,
    });
  }
  return snapshot;
}

} // namespace

RudpCombatFlow::RudpCombatFlow(transport::rudp::RudpBindingRegistry &bindings,
                               game_flow::RoomCommandGateway &gateway) noexcept
    : bindings_(bindings), gateway_(gateway) {}

RudpCombatSubmitResult
RudpCombatFlow::submitAttack(std::span<const std::byte> datagram,
                             const transport::rudp::RudpEndpoint &endpoint,
                             std::chrono::steady_clock::time_point receivedAt) {
  const auto decoded = transport::rudp::RudpCombatCodec::decode(datagram);
  if (decoded.error != transport::rudp::RudpCombatCodecError::None ||
      !decoded.header.has_value() || !decoded.message.has_value()) {
    return RudpCombatSubmitResult::Malformed;
  }
  const auto *attack =
      std::get_if<transport::rudp::RudpAttackIntent>(&*decoded.message);
  if (attack == nullptr) {
    return RudpCombatSubmitResult::UnexpectedMessage;
  }
  const auto received =
      bindings_.receive(*decoded.header, endpoint, receivedAt);
  if (received.status != transport::rudp::RudpPacketStatus::Current ||
      !received.disposition.has_value()) {
    return RudpCombatSubmitResult::PeerRejected;
  }
  static_cast<void>(discardAcknowledged(
      decoded.header->sessionId, decoded.header->sessionGeneration,
      decoded.header->transportEpoch, decoded.header->ack,
      decoded.header->ackBits));
  if (*received.disposition != transport::rudp::ReceiveDisposition::Newest &&
      *received.disposition != transport::rudp::ReceiveDisposition::Reordered) {
    return RudpCombatSubmitResult::StaleTransport;
  }
  const auto submitted = gateway_.submitAttack(
      battle::AttackCommand{
          .commandId = battle::CommandId{.high = attack->commandId.high,
                                         .low = attack->commandId.low},
          .sessionId = shared::SessionId{decoded.header->sessionId},
          .generation =
              shared::SessionGeneration{decoded.header->sessionGeneration},
          .battleId = shared::BattleInstanceId{attack->battleInstanceId},
          .targetHint = attack->targetHint,
      },
      receivedAt);
  if (submitted == game_flow::RoomSubmitResult::Accepted) {
    return RudpCombatSubmitResult::Accepted;
  }
  recordPeerFailure(RudpPeerFailure{
      .sessionId = shared::SessionId{decoded.header->sessionId},
      .generation =
          shared::SessionGeneration{decoded.header->sessionGeneration},
      .transportEpoch = decoded.header->transportEpoch,
      .sequence = decoded.header->sequence,
      .reason = RudpPeerFailureReason::RoomAdmissionRejected,
      .endpoint = endpoint,
  });
  return RudpCombatSubmitResult::RoomRejected;
}

RudpCombatSubmitResult RudpCombatFlow::submitClaimLoot(
    std::span<const std::byte> datagram,
    const transport::rudp::RudpEndpoint &endpoint,
    std::chrono::steady_clock::time_point receivedAt) {
  const auto decoded = transport::rudp::RudpLootCodec::decode(datagram);
  if (decoded.error != transport::rudp::RudpLootCodecError::None ||
      !decoded.header.has_value() || !decoded.message.has_value()) {
    return RudpCombatSubmitResult::Malformed;
  }
  const auto *claim =
      std::get_if<transport::rudp::RudpClaimLootIntent>(&*decoded.message);
  if (claim == nullptr) {
    return RudpCombatSubmitResult::UnexpectedMessage;
  }
  const auto received =
      bindings_.receive(*decoded.header, endpoint, receivedAt);
  if (received.status != transport::rudp::RudpPacketStatus::Current ||
      !received.disposition.has_value()) {
    return RudpCombatSubmitResult::PeerRejected;
  }
  static_cast<void>(discardAcknowledged(
      decoded.header->sessionId, decoded.header->sessionGeneration,
      decoded.header->transportEpoch, decoded.header->ack,
      decoded.header->ackBits));
  if (*received.disposition != transport::rudp::ReceiveDisposition::Newest &&
      *received.disposition != transport::rudp::ReceiveDisposition::Reordered) {
    return RudpCombatSubmitResult::StaleTransport;
  }
  const auto submitted = gateway_.submitClaimLoot(
      battle::ClaimLootCommand{
          .commandId = {.high = claim->commandId.high,
                        .low = claim->commandId.low},
          .sessionId = shared::SessionId{decoded.header->sessionId},
          .generation =
              shared::SessionGeneration{decoded.header->sessionGeneration},
          .battleId = shared::BattleInstanceId{claim->battleInstanceId},
          .dropId = battle::DropId{claim->dropId},
      },
      receivedAt);
  if (submitted == game_flow::RoomSubmitResult::Accepted) {
    return RudpCombatSubmitResult::Accepted;
  }
  recordPeerFailure(RudpPeerFailure{
      .sessionId = shared::SessionId{decoded.header->sessionId},
      .generation =
          shared::SessionGeneration{decoded.header->sessionGeneration},
      .transportEpoch = decoded.header->transportEpoch,
      .sequence = decoded.header->sequence,
      .reason = RudpPeerFailureReason::RoomAdmissionRejected,
      .endpoint = endpoint,
  });
  return RudpCombatSubmitResult::RoomRejected;
}

std::size_t RudpCombatFlow::discardAcknowledged(std::uint64_t sessionId,
                                                std::uint64_t sessionGeneration,
                                                std::uint32_t transportEpoch,
                                                std::uint32_t ack,
                                                std::uint32_t ackBits) {
  std::lock_guard lock{mutex_};
  const auto state = reliableStates_.find(
      ReliableKey{sessionId, sessionGeneration, transportEpoch});
  if (state == reliableStates_.end()) {
    return 0;
  }
  const auto removed = state->second.queue.discardAcknowledged(ack, ackBits);
  if (state->second.queue.empty()) {
    reliableStates_.erase(state);
  }
  return removed;
}

RudpCombatPollResult
RudpCombatFlow::pollReliable(std::chrono::steady_clock::time_point now) {
  std::lock_guard lock{mutex_};
  RudpCombatPollResult result;
  for (auto state = reliableStates_.begin(); state != reliableStates_.end();) {
    // Before polling, confirm the identity the reliable state was created for
    // is still the current binding. After session close/replacement, timeout,
    // TransportEpoch rebind, or invalidation the stored identity is stale and
    // must be dropped without transmit/expiry work.
    const auto bindingStatus =
        bindings_.status(state->first.sessionId, state->first.sessionGeneration,
                         state->first.transportEpoch, state->second.endpoint);
    if (bindingStatus != transport::rudp::RudpPacketStatus::Current) {
      state = reliableStates_.erase(state);
      continue;
    }
    auto polled = state->second.queue.poll(now);
    for (auto &transmission : polled.transmissions) {
      result.transmissions.push_back(EncodedRudpDatagram{
          .endpoint = state->second.endpoint,
          .datagram = std::move(transmission.datagram),
      });
    }
    for (const auto sequence : polled.expiredSequences) {
      recordPeerFailureLocked(RudpPeerFailure{
          .sessionId = shared::SessionId{state->first.sessionId},
          .generation =
              shared::SessionGeneration{state->first.sessionGeneration},
          .transportEpoch = state->first.transportEpoch,
          .sequence = sequence,
          .reason = RudpPeerFailureReason::ReliableExpired,
          .endpoint = state->second.endpoint,
      });
    }
    if (state->second.queue.empty()) {
      state = reliableStates_.erase(state);
    } else {
      ++state;
    }
  }
  result.failures.reserve(pendingFailures_.size());
  for (auto &[sessionId, failure] : pendingFailures_) {
    static_cast<void>(sessionId);
    result.failures.push_back(std::move(failure));
  }
  pendingFailures_.clear();
  return result;
}

std::vector<EncodedRudpDatagram> RudpCombatFlow::takeUnreliableSnapshots() {
  std::lock_guard lock{mutex_};
  return std::exchange(pendingSnapshots_, {});
}

std::size_t RudpCombatFlow::reliableStateCount() {
  std::lock_guard lock{mutex_};
  return reliableStates_.size();
}

std::size_t RudpCombatFlow::snapshotSequenceCount() {
  std::lock_guard lock{mutex_};
  return nextSnapshotSequences_.size();
}

void RudpCombatFlow::handleCombatOutbound(
    game_flow::CombatOutboundIntent intent) {
  std::visit(
      [this, &intent](const auto &message) {
        using Message = std::remove_cvref_t<decltype(message)>;
        if constexpr (std::is_same_v<Message,
                                     game_flow::CombatAttackResultOutbound>) {
          if (intent.actorSessionId.has_value() &&
              intent.actorGeneration.has_value()) {
            enqueueReliable(
                intent.actorSessionId->value(), intent.actorGeneration->value(),
                kAttackTerminalResultMessageId,
                RudpBattleMessage{transport::rudp::RudpCombatMessage{
                    toRudpResult(message.result)}});
          }
        } else if constexpr (std::is_same_v<
                                 Message,
                                 game_flow::CombatMonsterSpawnedOutbound>) {
          const auto spawned = toRudpSpawned(message);
          for (const auto &participant : message.participants) {
            enqueueReliable(
                participant.sessionId.value(), participant.generation.value(),
                kMonsterSpawnedMessageId,
                RudpBattleMessage{transport::rudp::RudpCombatMessage{spawned}});
          }
        } else if constexpr (std::is_same_v<
                                 Message,
                                 game_flow::CombatTerminalEventOutbound>) {
          const auto terminal = toRudpTerminal(message.terminal);
          for (const auto &participant : message.participants) {
            enqueueReliable(participant.sessionId.value(),
                            participant.generation.value(),
                            kCombatTerminalEventMessageId,
                            RudpBattleMessage{
                                transport::rudp::RudpCombatMessage{terminal}});
          }
        } else if constexpr (std::is_same_v<
                                 Message, game_flow::LootClaimResultOutbound>) {
          if (intent.actorSessionId.has_value() &&
              intent.actorGeneration.has_value()) {
            enqueueReliable(intent.actorSessionId->value(),
                            intent.actorGeneration->value(),
                            kClaimLootTerminalResultMessageId,
                            RudpBattleMessage{transport::rudp::RudpLootMessage{
                                toRudpLootResult(message.result)}});
          }
        } else if constexpr (std::is_same_v<
                                 Message,
                                 game_flow::LootDropsSpawnedOutbound>) {
          for (const auto &drop : message.projection.drops) {
            const auto spawned =
                toRudpDropSpawned(message.projection.battleId, drop);
            for (const auto &participant : message.participants) {
              enqueueReliable(
                  participant.sessionId.value(), participant.generation.value(),
                  kDropSpawnedMessageId,
                  RudpBattleMessage{transport::rudp::RudpLootMessage{spawned}});
            }
          }
        } else if constexpr (std::is_same_v<
                                 Message,
                                 game_flow::CombatBattleRetiredOutbound>) {
          // Room teardown already happened; only the per-battle snapshot
          // sequence identity is retired. No datagram is produced.
          retireSnapshotSequence(message.battleId.value());
        } else if constexpr (std::is_same_v<
                                 Message,
                                 game_flow::CombatMonsterStateOutbound>) {
          const auto datagrams =
              encodeMonsterSnapshot(message.projection, message.participants);
          if (datagrams.has_value()) {
            std::lock_guard lock{mutex_};
            pendingSnapshots_.insert(pendingSnapshots_.end(),
                                     datagrams->begin(), datagrams->end());
          }
        } else {
          const auto datagrams =
              encodeLootSnapshot(message.projection, message.participants);
          if (datagrams.has_value()) {
            std::lock_guard lock{mutex_};
            pendingSnapshots_.insert(pendingSnapshots_.end(),
                                     datagrams->begin(), datagrams->end());
          }
        }
      },
      intent.message);
}

std::uint32_t RudpCombatFlow::nextSnapshotSequence(std::uint64_t battleId) {
  std::lock_guard lock{mutex_};
  auto &sequence = nextSnapshotSequences_[battleId];
  if (sequence == 0) {
    sequence = 1;
  } else {
    ++sequence;
  }
  if (sequence == 0) {
    sequence = 1;
  }
  return sequence;
}

void RudpCombatFlow::retireSnapshotSequence(std::uint64_t battleId) {
  std::lock_guard lock{mutex_};
  nextSnapshotSequences_.erase(battleId);
}

void RudpCombatFlow::enqueueReliable(std::uint64_t sessionId,
                                     std::uint64_t expectedGeneration,
                                     std::uint16_t messageId,
                                     const RudpBattleMessage &message) {
  if (!bindings_.isBound(sessionId, expectedGeneration)) {
    return;
  }
  const auto route = bindings_.nextOutbound(sessionId);
  if (!route.has_value() || route->sessionGeneration != expectedGeneration) {
    return;
  }
  const transport::rudp::RudpHeader header{
      .flag = transport::rudp::RudpFlag::Reliable,
      .sessionId = route->sessionId,
      .sessionGeneration = route->sessionGeneration,
      .transportEpoch = route->transportEpoch,
      .sequence = route->sequence,
      .ack = route->ack.ack,
      .ackBits = route->ack.ackBits,
      .messageId = messageId,
  };
  auto encoded = std::visit(
      [&header](const auto &typed) {
        using Message = std::remove_cvref_t<decltype(typed)>;
        if constexpr (std::is_same_v<Message,
                                     transport::rudp::RudpCombatMessage>) {
          return transport::rudp::RudpCombatCodec::encode(header, typed);
        } else {
          return transport::rudp::RudpLootCodec::encode(header, typed);
        }
      },
      message);
  if (!encoded.has_value()) {
    return;
  }
  const ReliableKey key{route->sessionId, route->sessionGeneration,
                        route->transportEpoch};
  std::lock_guard lock{mutex_};
  retireReliableStatesLocked(route->sessionId, key);
  auto &state = reliableStates_[key];
  state.endpoint = route->endpoint;
  const auto admission =
      state.queue.enqueue(route->sequence, std::move(*encoded),
                          transport::rudp::ReliableLane::Application,
                          std::chrono::steady_clock::now());
  if (admission != transport::rudp::ReliableQueueAdmission::Accepted) {
    recordPeerFailureLocked(RudpPeerFailure{
        .sessionId = shared::SessionId{route->sessionId},
        .generation = shared::SessionGeneration{route->sessionGeneration},
        .transportEpoch = route->transportEpoch,
        .sequence = route->sequence,
        .reason = RudpPeerFailureReason::ReliableAdmissionRejected,
        .endpoint = route->endpoint,
    });
  }
}

void RudpCombatFlow::retireReliableStatesLocked(std::uint64_t sessionId,
                                                const ReliableKey &keep) {
  for (auto state = reliableStates_.begin(); state != reliableStates_.end();) {
    if (state->first.sessionId == sessionId && !(state->first == keep)) {
      state = reliableStates_.erase(state);
    } else {
      ++state;
    }
  }
}

void RudpCombatFlow::recordPeerFailure(RudpPeerFailure failure) {
  std::lock_guard lock{mutex_};
  recordPeerFailureLocked(std::move(failure));
}

void RudpCombatFlow::recordPeerFailureLocked(RudpPeerFailure failure) {
  if (bindings_.status(failure.sessionId.value(), failure.generation.value(),
                       failure.transportEpoch, failure.endpoint) !=
      transport::rudp::RudpPacketStatus::Current) {
    return;
  }
  const auto pending = pendingFailures_.find(failure.sessionId.value());
  if (pending != pendingFailures_.end() &&
      pending->second.generation == failure.generation &&
      pending->second.transportEpoch == failure.transportEpoch) {
    return;
  }
  pendingFailures_.insert_or_assign(failure.sessionId.value(),
                                    std::move(failure));
}

std::optional<std::vector<EncodedRudpDatagram>>
RudpCombatFlow::encodeMonsterSnapshot(
    const battle::CombatProjection &projection,
    std::span<const game_flow::BattleParticipantProjection> participants) {
  const auto sequence = nextSnapshotSequence(projection.battleId.value());
  if (projection.terminal.has_value()) {
    retireSnapshotSequence(projection.battleId.value());
  }
  std::vector<EncodedRudpDatagram> datagrams;
  datagrams.reserve(participants.size());
  for (const auto &participant : participants) {
    if (!bindings_.isBound(participant.sessionId.value(),
                           participant.generation.value())) {
      continue;
    }
    const auto route = bindings_.nextOutbound(participant.sessionId.value());
    if (!route.has_value() ||
        route->sessionGeneration != participant.generation.value()) {
      continue;
    }
    const transport::rudp::RudpMonsterStateSnapshot snapshot{
        .battleInstanceId = projection.battleId.value(),
        .snapshotSequence = sequence,
        .serverTick = projection.serverTick,
        .monsterId = projection.monsterId,
        .hitPoints = projection.hitPoints,
        .monsterState = static_cast<transport::rudp::RudpMonsterState>(
            static_cast<std::uint8_t>(projection.monsterState)),
    };
    const auto encoded = transport::rudp::RudpCombatCodec::encode(
        transport::rudp::RudpHeader{
            .flag = transport::rudp::RudpFlag::Unreliable,
            .sessionId = route->sessionId,
            .sessionGeneration = route->sessionGeneration,
            .transportEpoch = route->transportEpoch,
            .sequence = route->sequence,
            .ack = route->ack.ack,
            .ackBits = route->ack.ackBits,
            .messageId = kMonsterStateSnapshotMessageId,
        },
        transport::rudp::RudpCombatMessage{snapshot});
    if (!encoded.has_value()) {
      return std::nullopt;
    }
    datagrams.push_back(EncodedRudpDatagram{
        .endpoint = route->endpoint,
        .datagram = std::move(*encoded),
    });
  }
  return datagrams;
}

std::optional<std::vector<EncodedRudpDatagram>>
RudpCombatFlow::encodeLootSnapshot(
    const battle::LootProjection &projection,
    std::span<const game_flow::BattleParticipantProjection> participants) {
  const auto snapshot = toRudpLootSnapshot(
      projection, nextSnapshotSequence(projection.battleId.value()));
  std::vector<EncodedRudpDatagram> datagrams;
  datagrams.reserve(participants.size());
  for (const auto &participant : participants) {
    if (!bindings_.isBound(participant.sessionId.value(),
                           participant.generation.value())) {
      continue;
    }
    const auto route = bindings_.nextOutbound(participant.sessionId.value());
    if (!route.has_value() ||
        route->sessionGeneration != participant.generation.value()) {
      continue;
    }
    auto encoded = transport::rudp::RudpLootCodec::encode(
        transport::rudp::RudpHeader{
            .flag = transport::rudp::RudpFlag::Unreliable,
            .sessionId = route->sessionId,
            .sessionGeneration = route->sessionGeneration,
            .transportEpoch = route->transportEpoch,
            .sequence = route->sequence,
            .ack = route->ack.ack,
            .ackBits = route->ack.ackBits,
            .messageId = kDropStateSnapshotMessageId,
        },
        transport::rudp::RudpLootMessage{snapshot});
    if (!encoded.has_value()) {
      return std::nullopt;
    }
    datagrams.push_back(EncodedRudpDatagram{
        .endpoint = route->endpoint,
        .datagram = std::move(*encoded),
    });
  }
  return datagrams;
}

} // namespace lol::app
