#include <lol/session/SessionRegistry.hpp>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

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

} // namespace

int main() {
  if (!firstSessionHasNoReplacement() ||
      !replacementProtectsTheNewSessionFromStaleDisconnect() ||
      !generationIsMonotonicAcrossSessions()) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
