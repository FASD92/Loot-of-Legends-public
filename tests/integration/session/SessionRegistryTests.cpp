#include <lol/session/SessionRegistry.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;

lol::shared::AccountId account(std::uint8_t suffix) {
  lol::shared::AccountId::Bytes bytes{};
  bytes.back() = suffix;
  return lol::shared::AccountId{bytes};
}

bool firstSessionHasNoReplacement() {
  lol::session::SessionRegistry registry;
  const auto authenticated = registry.authenticate(
      {lol::shared::RequestId{1}, {account(1), "player-one"}});

  return authenticated.sessionId.value() != 0 &&
         authenticated.generation.value() != 0 &&
         !authenticated.replaced.has_value() &&
         registry.activeSessionCount() == 1;
}

bool replacementProtectsTheNewSessionFromStaleDisconnect() {
  lol::session::SessionRegistry registry;
  const auto first = registry.authenticate(
      {lol::shared::RequestId{1}, {account(1), "player-one"}});
  const auto second = registry.authenticate(
      {lol::shared::RequestId{2}, {account(1), "player-one"}});

  return second.replaced.has_value() &&
         second.replaced->sessionId == first.sessionId &&
         second.replaced->generation == first.generation &&
         second.generation > first.generation &&
         !registry.disconnect(first.sessionId, first.generation) &&
         registry.activeSessionCount() == 1 &&
         registry.disconnect(second.sessionId, second.generation) &&
         !registry.disconnect(second.sessionId, second.generation) &&
         registry.activeSessionCount() == 0;
}

bool generationIsMonotonicAcrossSessions() {
  lol::session::SessionRegistry registry;
  const auto first = registry.authenticate(
      {lol::shared::RequestId{1}, {account(1), "player-one"}});
  const auto other = registry.authenticate(
      {lol::shared::RequestId{2}, {account(2), "player-two"}});
  const auto replacement = registry.authenticate(
      {lol::shared::RequestId{3}, {account(1), "player-one"}});

  return first.generation < other.generation &&
         other.generation < replacement.generation;
}

bool detachedSessionResumesWithTheSameIdentityExactlyOnce() {
  lol::session::SessionRegistry registry;
  const auto openedAt = std::chrono::steady_clock::time_point{};
  const auto authenticated = registry.authenticate(
      {lol::shared::RequestId{1}, {account(1), "player-one"}});

  if (!registry.detach(authenticated.sessionId, authenticated.generation,
                       openedAt + 30s)) {
    return false;
  }
  const auto resumed = registry.resume(lol::session::ResumeSessionCommand{
      .requestId = lol::shared::RequestId{2},
      .identity = {account(1), "player-one"},
      .sessionId = authenticated.sessionId,
      .generation = authenticated.generation,
      .now = openedAt + 29s,
  });
  const auto loser = registry.resume(lol::session::ResumeSessionCommand{
      .requestId = lol::shared::RequestId{3},
      .identity = {account(1), "player-one"},
      .sessionId = authenticated.sessionId,
      .generation = authenticated.generation,
      .now = openedAt + 29s,
  });

  return resumed.code == lol::session::ResumeSessionCode::Ok &&
         resumed.authenticated.has_value() &&
         resumed.authenticated->sessionId == authenticated.sessionId &&
         resumed.authenticated->generation == authenticated.generation &&
         !resumed.authenticated->replaced.has_value() &&
         loser.code == lol::session::ResumeSessionCode::NotDetached &&
         !loser.authenticated.has_value() && registry.activeSessionCount() == 1;
}

bool staleAccountSessionAndExpiredGraceCannotResume() {
  lol::session::SessionRegistry registry;
  const auto openedAt = std::chrono::steady_clock::time_point{};
  const auto authenticated = registry.authenticate(
      {lol::shared::RequestId{1}, {account(1), "player-one"}});
  if (!registry.detach(authenticated.sessionId, authenticated.generation,
                       openedAt + 30s)) {
    return false;
  }

  const auto wrongAccount = registry.resume(lol::session::ResumeSessionCommand{
      .requestId = lol::shared::RequestId{2},
      .identity = {account(2), "player-two"},
      .sessionId = authenticated.sessionId,
      .generation = authenticated.generation,
      .now = openedAt + 1s,
  });
  const auto staleGeneration =
      registry.resume(lol::session::ResumeSessionCommand{
          .requestId = lol::shared::RequestId{3},
          .identity = {account(1), "player-one"},
          .sessionId = authenticated.sessionId,
          .generation =
              lol::shared::SessionGeneration{authenticated.generation.value() +
                                             1},
          .now = openedAt + 1s,
      });
  const auto expired = registry.resume(lol::session::ResumeSessionCommand{
      .requestId = lol::shared::RequestId{4},
      .identity = {account(1), "player-one"},
      .sessionId = authenticated.sessionId,
      .generation = authenticated.generation,
      .now = openedAt + 30s,
  });

  return wrongAccount.code ==
             lol::session::ResumeSessionCode::IdentityMismatch &&
         staleGeneration.code ==
             lol::session::ResumeSessionCode::StaleSession &&
         expired.code == lol::session::ResumeSessionCode::Expired &&
         registry.disconnect(authenticated.sessionId,
                             authenticated.generation) &&
         !registry.disconnect(authenticated.sessionId,
                              authenticated.generation) &&
         registry.activeSessionCount() == 0;
}

bool recoveredSessionsPreserveIdsAndIssueFreshGenerations() {
  lol::session::SessionRegistry registry;
  const auto openedAt = std::chrono::steady_clock::time_point{};
  const auto recovered = registry.installRecoveredDetached(
      std::vector<lol::session::RecoveredDetachedSession>{
          {{account(1), "player-one"},
           lol::shared::SessionId{41},
           lol::shared::SessionGeneration{7}},
          {{account(2), "player-two"},
           lol::shared::SessionId{42},
           lol::shared::SessionGeneration{8}},
      },
      openedAt + 30s);
  if (!recovered.has_value() || recovered->size() != 2 ||
      recovered->at(0).sessionId != lol::shared::SessionId{41} ||
      recovered->at(1).sessionId != lol::shared::SessionId{42} ||
      recovered->at(0).generation <= lol::shared::SessionGeneration{8} ||
      recovered->at(1).generation <= recovered->at(0).generation ||
      registry.activeSessionCount() != 2) {
    return false;
  }

  const auto resumed = registry.resume(lol::session::ResumeSessionCommand{
      .requestId = lol::shared::RequestId{1},
      .identity = {account(1), "player-one"},
      .sessionId = lol::shared::SessionId{41},
      .generation = lol::shared::SessionGeneration{7},
      .now = openedAt + 1s,
  });
  return resumed.code == lol::session::ResumeSessionCode::Ok &&
         resumed.authenticated.has_value() &&
         resumed.authenticated->sessionId == lol::shared::SessionId{41} &&
         resumed.authenticated->generation == recovered->at(0).generation;
}

bool recoveredResumeConsumesOldAndCurrentProofExactlyOnce() {
  lol::session::SessionRegistry registry;
  const auto openedAt = std::chrono::steady_clock::time_point{};
  const auto recovered = registry.installRecoveredDetached(
      std::vector<lol::session::RecoveredDetachedSession>{
          {{account(1), "player-one"},
           lol::shared::SessionId{51},
           lol::shared::SessionGeneration{90}},
      },
      openedAt + 30s);
  if (!recovered.has_value() || recovered->size() != 1) {
    return false;
  }

  const auto resumed = registry.resume(lol::session::ResumeSessionCommand{
      .requestId = lol::shared::RequestId{1},
      .identity = {account(1), "player-one"},
      .sessionId = lol::shared::SessionId{51},
      .generation = lol::shared::SessionGeneration{90},
      .now = openedAt + 1s,
  });
  const auto staleOld = registry.resume(lol::session::ResumeSessionCommand{
      .requestId = lol::shared::RequestId{2},
      .identity = {account(1), "player-one"},
      .sessionId = lol::shared::SessionId{51},
      .generation = lol::shared::SessionGeneration{90},
      .now = openedAt + 1s,
  });
  const auto staleCurrent = registry.resume(lol::session::ResumeSessionCommand{
      .requestId = lol::shared::RequestId{3},
      .identity = {account(1), "player-one"},
      .sessionId = lol::shared::SessionId{51},
      .generation = recovered->at(0).generation,
      .now = openedAt + 1s,
  });
  return resumed.code == lol::session::ResumeSessionCode::Ok &&
         staleOld.code != lol::session::ResumeSessionCode::Ok &&
         staleCurrent.code != lol::session::ResumeSessionCode::Ok &&
         !staleOld.authenticated.has_value() &&
         !staleCurrent.authenticated.has_value();
}

bool recoveredInstallRejectsTheWholeBatchOnDuplicateOrConflict() {
  const auto openedAt = std::chrono::steady_clock::time_point{};
  {
    lol::session::SessionRegistry registry;
    const auto duplicateAccount = registry.installRecoveredDetached(
        std::vector<lol::session::RecoveredDetachedSession>{
            {{account(1), "player-one"},
             lol::shared::SessionId{61},
             lol::shared::SessionGeneration{1}},
            {{account(1), "player-one"},
             lol::shared::SessionId{62},
             lol::shared::SessionGeneration{2}},
        },
        openedAt + 30s);
    if (duplicateAccount.has_value() || registry.activeSessionCount() != 0) {
      return false;
    }
  }
  {
    lol::session::SessionRegistry registry;
    const auto duplicateSession = registry.installRecoveredDetached(
        std::vector<lol::session::RecoveredDetachedSession>{
            {{account(1), "player-one"},
             lol::shared::SessionId{63},
             lol::shared::SessionGeneration{1}},
            {{account(2), "player-two"},
             lol::shared::SessionId{63},
             lol::shared::SessionGeneration{2}},
        },
        openedAt + 30s);
    if (duplicateSession.has_value() || registry.activeSessionCount() != 0) {
      return false;
    }
  }
  {
    lol::session::SessionRegistry registry;
    const auto existing = registry.authenticate(
        {lol::shared::RequestId{1}, {account(1), "player-one"}});
    const auto conflict = registry.installRecoveredDetached(
        std::vector<lol::session::RecoveredDetachedSession>{
            {{account(1), "player-one"},
             lol::shared::SessionId{64},
             lol::shared::SessionGeneration{10}},
            {{account(2), "player-two"},
             lol::shared::SessionId{65},
             lol::shared::SessionGeneration{11}},
        },
        openedAt + 30s);
    return !conflict.has_value() && registry.activeSessionCount() == 1 &&
           registry.disconnect(existing.sessionId, existing.generation);
  }
}

bool recoveredInstallRejectsZeroAndCounterOverflowAtomically() {
  const auto openedAt = std::chrono::steady_clock::time_point{};
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  const auto rejected =
      [&openedAt](lol::session::RecoveredDetachedSession entry) {
        lol::session::SessionRegistry registry;
        const auto result = registry.installRecoveredDetached(
            std::vector<lol::session::RecoveredDetachedSession>{
                entry,
                {{account(2), "player-two"},
                 lol::shared::SessionId{72},
                 lol::shared::SessionGeneration{12}},
            },
            openedAt + 30s);
        return !result.has_value() && registry.activeSessionCount() == 0;
      };
  return rejected({{account(1), "player-one"},
                   lol::shared::SessionId{0},
                   lol::shared::SessionGeneration{1}}) &&
         rejected({{account(1), "player-one"},
                   lol::shared::SessionId{71},
                   lol::shared::SessionGeneration{0}}) &&
         rejected({{account(1), "player-one"},
                   lol::shared::SessionId{maximum},
                   lol::shared::SessionGeneration{1}}) &&
         rejected({{account(1), "player-one"},
                   lol::shared::SessionId{maximum - 1U},
                   lol::shared::SessionGeneration{1}}) &&
         rejected({{account(1), "player-one"},
                   lol::shared::SessionId{73},
                   lol::shared::SessionGeneration{maximum}}) &&
         rejected({{account(1), "player-one"},
                   lol::shared::SessionId{74},
                   lol::shared::SessionGeneration{maximum - 2U}});
}

bool recoveryEpochFloorFencesPreviousGeneration() {
  constexpr std::uint32_t recoveryEpoch = 7U;
  const auto floor = (static_cast<std::uint64_t>(recoveryEpoch) << 32U) | 1U;
  const auto openedAt = std::chrono::steady_clock::time_point{};
  lol::session::SessionRegistry registry{lol::shared::SessionGeneration{floor}};

  const auto authenticated = registry.authenticate(
      {lol::shared::RequestId{1}, {account(1), "player-one"}});
  const auto recovered = registry.installRecoveredDetached(
      std::vector<lol::session::RecoveredDetachedSession>{
          {{account(2), "player-two"},
           lol::shared::SessionId{41},
           lol::shared::SessionGeneration{3}}},
      openedAt + 30s);
  if (authenticated.generation != lol::shared::SessionGeneration{floor} ||
      !recovered.has_value() || recovered->size() != 1U) {
    return false;
  }

  const auto current = recovered->at(0).generation;
  const auto previous = lol::shared::SessionGeneration{3};
  if ((current.value() >> 32U) != recoveryEpoch || current <= previous ||
      registry.detach(lol::shared::SessionId{41}, previous, openedAt + 30s) ||
      registry.disconnect(lol::shared::SessionId{41}, previous)) {
    return false;
  }

  const auto resumed = registry.resume(lol::session::ResumeSessionCommand{
      .requestId = lol::shared::RequestId{2},
      .identity = {account(2), "player-two"},
      .sessionId = lol::shared::SessionId{41},
      .generation = previous,
      .now = openedAt + 1s,
  });
  const auto consumed = registry.resume(lol::session::ResumeSessionCommand{
      .requestId = lol::shared::RequestId{3},
      .identity = {account(2), "player-two"},
      .sessionId = lol::shared::SessionId{41},
      .generation = previous,
      .now = openedAt + 1s,
  });
  return resumed.code == lol::session::ResumeSessionCode::Ok &&
         consumed.code != lol::session::ResumeSessionCode::Ok &&
         !registry.detach(lol::shared::SessionId{41}, previous,
                          openedAt + 30s) &&
         !registry.disconnect(lol::shared::SessionId{41}, previous) &&
         registry.disconnect(lol::shared::SessionId{41}, current);
}

bool invalidGenerationFloorAndOverflowAreGuarded() {
  bool zeroRejected = false;
  try {
    lol::session::SessionRegistry invalid{lol::shared::SessionGeneration{0}};
    static_cast<void>(invalid);
  } catch (const std::invalid_argument &) {
    zeroRejected = true;
  }
  if (!zeroRejected) {
    return false;
  }

  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  lol::session::SessionRegistry registry{
      lol::shared::SessionGeneration{maximum - 1U}};
  static_cast<void>(registry.authenticate(
      {lol::shared::RequestId{1}, {account(1), "player-one"}}));
  try {
    static_cast<void>(registry.authenticate(
        {lol::shared::RequestId{2}, {account(2), "player-two"}}));
  } catch (const std::overflow_error &) {
    return registry.activeSessionCount() == 1U;
  }
  return false;
}

} // namespace

int main() {
  if (!firstSessionHasNoReplacement() ||
      !replacementProtectsTheNewSessionFromStaleDisconnect() ||
      !generationIsMonotonicAcrossSessions() ||
      !detachedSessionResumesWithTheSameIdentityExactlyOnce() ||
      !staleAccountSessionAndExpiredGraceCannotResume() ||
      !recoveredSessionsPreserveIdsAndIssueFreshGenerations() ||
      !recoveredResumeConsumesOldAndCurrentProofExactlyOnce() ||
      !recoveredInstallRejectsTheWholeBatchOnDuplicateOrConflict() ||
      !recoveredInstallRejectsZeroAndCounterOverflowAtomically() ||
      !recoveryEpochFloorFencesPreviousGeneration() ||
      !invalidGenerationFloorAndOverflowAreGuarded()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
