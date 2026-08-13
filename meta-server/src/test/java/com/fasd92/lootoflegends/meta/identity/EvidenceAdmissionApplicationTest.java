package com.fasd92.lootoflegends.meta.identity;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasd92.lootoflegends.meta.gameaccess.api.ClaimGameCredentialCommand;
import com.fasd92.lootoflegends.meta.gameaccess.api.ClaimGameCredentialResult;
import com.fasd92.lootoflegends.meta.gameaccess.api.IssueGameCredentialCommand;
import com.fasd92.lootoflegends.meta.gameaccess.application.GameCredentialApplication;
import com.fasd92.lootoflegends.meta.gameaccess.application.GameCredentialStore;
import com.fasd92.lootoflegends.meta.identity.api.EvidenceAdmissionUseCase;
import com.fasd92.lootoflegends.meta.identity.api.IssueEvidenceSessionCommand;
import com.fasd92.lootoflegends.meta.identity.application.DesktopAuthApplication;
import com.fasd92.lootoflegends.meta.identity.application.DesktopAuthStore;
import com.fasd92.lootoflegends.meta.identity.application.ExternalIdentityProvider;
import java.net.URI;
import java.security.SecureRandom;
import java.time.Clock;
import java.time.Instant;
import java.time.ZoneOffset;
import java.util.HashMap;
import java.util.Map;
import java.util.Optional;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

class EvidenceAdmissionApplicationTest {
  private static final Instant NOW = Instant.parse("2026-08-11T00:00:00Z");
  private static final Instant EXPIRES_AT = Instant.parse("2026-08-11T00:10:00Z");

  private FakeDesktopAuthStore sessions;
  private DesktopAuthApplication identity;

  @BeforeEach
  void setUp() {
    sessions = new FakeDesktopAuthStore();
    identity =
        new DesktopAuthApplication(
            sessions, new UnusedProvider(), Clock.fixed(NOW, ZoneOffset.UTC), new SecureRandom());
  }

  @Test
  void evidencePrincipalUsesTheNormalSessionCredentialClaimAndAuthenticateChain() {
    var issued =
        identity.issueEvidenceSession(new IssueEvidenceSessionCommand("run-a", 1, EXPIRES_AT));

    assertEquals(EXPIRES_AT, issued.expiresAt());
    var principal = identity.authenticate(issued.metaSession()).orElseThrow();
    assertEquals("participant-0001", principal.nickname());

    var credentialStore = new FakeGameCredentialStore();
    var gameAccess =
        new GameCredentialApplication(
            credentialStore, Clock.fixed(NOW, ZoneOffset.UTC), new SecureRandom());
    var credential =
        gameAccess.issue(
            new IssueGameCredentialCommand(principal.accountId(), principal.nickname()));
    var claimed =
        gameAccess.claim(
            new ClaimGameCredentialCommand(credential.credential(), "loot-game-server-v1"));

    var identityClaim = assertTrueClaimed(claimed);
    assertEquals(principal.accountId(), identityClaim.accountId());
    assertEquals(principal.nickname(), identityClaim.nickname());
  }

  @Test
  void sameRunParticipantReplayIsRejectedAndInputIsBounded() {
    var command = new IssueEvidenceSessionCommand("run-a", 20, EXPIRES_AT);
    identity.issueEvidenceSession(command);

    var replay =
        assertThrows(
            EvidenceAdmissionUseCase.Rejected.class, () -> identity.issueEvidenceSession(command));
    assertEquals(EvidenceAdmissionUseCase.RejectionReason.REPLAYED, replay.reason());

    for (var invalid :
        new IssueEvidenceSessionCommand[] {
          new IssueEvidenceSessionCommand("bad run", 1, EXPIRES_AT),
          new IssueEvidenceSessionCommand("run-a", 0, EXPIRES_AT),
          new IssueEvidenceSessionCommand("run-a", 1, NOW),
          new IssueEvidenceSessionCommand("x".repeat(65), 1, EXPIRES_AT)
        }) {
      assertThrows(IllegalArgumentException.class, () -> identity.issueEvidenceSession(invalid));
    }
  }

  private static ClaimGameCredentialResult.Claimed assertTrueClaimed(
      ClaimGameCredentialResult result) {
    assertTrue(result instanceof ClaimGameCredentialResult.Claimed);
    return (ClaimGameCredentialResult.Claimed) result;
  }

  private static final class UnusedProvider implements ExternalIdentityProvider {
    @Override
    public URI authorizationUri(String state, String codeChallenge) {
      throw new AssertionError("evidence admission must not invoke Google OIDC");
    }

    @Override
    public ExternalIdentity exchange(String authorizationCode, String codeVerifier) {
      throw new AssertionError("evidence admission must not invoke Google OIDC");
    }
  }

  private static final class FakeDesktopAuthStore implements DesktopAuthStore {
    private final Map<String, MetaSession> sessions = new HashMap<>();
    private final Map<String, Instant> admissions = new HashMap<>();

    @Override
    public boolean putAttemptIfAbsent(String lookupHash, Attempt attempt) {
      throw new AssertionError("desktop attempt is not part of evidence admission");
    }

    @Override
    public Optional<Attempt> takeAttempt(String lookupHash) {
      return Optional.empty();
    }

    @Override
    public boolean putHandoffIfAbsent(String lookupHash, Handoff handoff) {
      throw new AssertionError("desktop handoff is not part of evidence admission");
    }

    @Override
    public Optional<Handoff> takeHandoff(String lookupHash) {
      return Optional.empty();
    }

    @Override
    public boolean putSessionIfAbsent(String lookupHash, MetaSession session) {
      return sessions.putIfAbsent(lookupHash, session) == null;
    }

    @Override
    public Optional<MetaSession> findSession(String lookupHash) {
      return Optional.ofNullable(sessions.get(lookupHash));
    }

    @Override
    public boolean putEvidenceAdmissionIfAbsent(String identityHash, Instant expiresAt) {
      return admissions.putIfAbsent(identityHash, expiresAt) == null;
    }
  }

  private static final class FakeGameCredentialStore implements GameCredentialStore {
    private record Credential(
        com.fasd92.lootoflegends.meta.identity.api.AccountId accountId,
        String nickname,
        String audience) {}

    private final Map<String, Credential> credentials = new HashMap<>();

    @Override
    public boolean putIfAbsent(
        String hash,
        com.fasd92.lootoflegends.meta.identity.api.AccountId accountId,
        String nickname,
        String audience,
        Instant expiresAt,
        Instant purgeAt) {
      return credentials.putIfAbsent(hash, new Credential(accountId, nickname, audience)) == null;
    }

    @Override
    public ClaimGameCredentialResult claim(String hash, String audience, Instant now) {
      var credential = credentials.get(hash);
      if (credential == null || !credential.audience().equals(audience)) {
        return new ClaimGameCredentialResult.Rejected(
            com.fasd92.lootoflegends.meta.gameaccess.api.ClaimRejectionReason.INVALID);
      }
      return new ClaimGameCredentialResult.Claimed(credential.accountId(), credential.nickname());
    }
  }
}
