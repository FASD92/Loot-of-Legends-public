package com.fasd92.lootoflegends.meta.identity.application;

import com.fasd92.lootoflegends.meta.identity.api.AuthenticatedPrincipal;
import java.net.URI;
import java.time.Instant;
import java.util.Optional;

public interface DesktopAuthStore {
  boolean putAttemptIfAbsent(String lookupHash, Attempt attempt);

  Optional<Attempt> takeAttempt(String lookupHash);

  boolean putHandoffIfAbsent(String lookupHash, Handoff handoff);

  Optional<Handoff> takeHandoff(String lookupHash);

  boolean putSessionIfAbsent(String lookupHash, MetaSession session);

  Optional<MetaSession> findSession(String lookupHash);

  boolean putEvidenceAdmissionIfAbsent(String identityHash, Instant expiresAt);

  record Attempt(
      URI loopbackRedirectUri,
      String clientState,
      String clientCodeChallenge,
      String providerCodeVerifier,
      Instant expiresAt) {}

  record Handoff(
      AuthenticatedPrincipal principal,
      String clientState,
      String clientCodeChallenge,
      Instant expiresAt) {}

  record MetaSession(AuthenticatedPrincipal principal, Instant expiresAt) {}

  final class Unavailable extends RuntimeException {
    public Unavailable(Throwable cause) {
      super(cause);
    }
  }
}
