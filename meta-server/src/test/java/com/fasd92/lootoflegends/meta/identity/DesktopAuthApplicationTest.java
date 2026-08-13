package com.fasd92.lootoflegends.meta.identity;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasd92.lootoflegends.meta.identity.api.CompleteProviderCallbackCommand;
import com.fasd92.lootoflegends.meta.identity.api.DesktopAuthUseCase;
import com.fasd92.lootoflegends.meta.identity.api.ExchangeDesktopAuthCommand;
import com.fasd92.lootoflegends.meta.identity.api.IssuedMetaSession;
import com.fasd92.lootoflegends.meta.identity.api.StartDesktopAuthCommand;
import com.fasd92.lootoflegends.meta.identity.api.StartDesktopAuthResult;
import com.fasd92.lootoflegends.meta.identity.application.DesktopAuthApplication;
import com.fasd92.lootoflegends.meta.identity.application.DesktopAuthStore;
import com.fasd92.lootoflegends.meta.identity.application.ExternalIdentityProvider;
import java.net.URI;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;
import java.security.SecureRandom;
import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneId;
import java.time.ZoneOffset;
import java.util.HashMap;
import java.util.Map;
import java.util.Optional;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

class DesktopAuthApplicationTest {
  private static final String STATE = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  private static final String OTHER_STATE = "ccccccccccccccccccccccccccccccccccccccccccc";
  private static final String VERIFIER = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  private static final URI LOOPBACK = URI.create("http://127.0.0.1:49152/callback");

  private MutableClock clock;
  private FakeStore store;
  private FakeProvider provider;
  private DesktopAuthApplication application;

  @BeforeEach
  void setUp() {
    clock = new MutableClock(Instant.parse("2026-08-10T00:00:00Z"));
    store = new FakeStore();
    provider = new FakeProvider();
    application = new DesktopAuthApplication(store, provider, clock, new SecureRandom());
  }

  @Test
  void successIssuesOneTimeHandoffAndMetaSessionOnly() {
    StartDesktopAuthResult started =
        application.start(new StartDesktopAuthCommand(LOOPBACK, STATE, challenge(VERIFIER)));

    assertEquals(clock.instant().plusSeconds(120), started.expiresAt());
    assertEquals(provider.providerState, query(started.authorizationUrl(), "state"));
    assertEquals(provider.providerChallenge, query(started.authorizationUrl(), "code_challenge"));

    URI callback =
        application.completeProviderCallback(
            new CompleteProviderCallbackCommand(provider.providerState, "synthetic-provider-code"));
    String handoffCode = query(callback, "code");
    assertEquals(STATE, query(callback, "state"));
    assertEquals("127.0.0.1", callback.getHost());
    assertEquals(49152, callback.getPort());
    assertEquals("/callback", callback.getPath());

    IssuedMetaSession session =
        application.exchange(new ExchangeDesktopAuthCommand(handoffCode, STATE, VERIFIER));
    assertEquals(43, session.metaSession().length());
    assertEquals(clock.instant().plusSeconds(3600), session.expiresAt());
    assertTrue(application.authenticate(session.metaSession()).isPresent());
    assertEquals(
        "player-",
        application.authenticate(session.metaSession()).orElseThrow().nickname().substring(0, 7));
    assertNotEquals(provider.providerCode, session.metaSession());
    assertFalse(store.containsRaw(handoffCode));
    assertFalse(store.containsRaw(session.metaSession()));

    DesktopAuthUseCase.Rejected replay =
        assertThrows(
            DesktopAuthUseCase.Rejected.class,
            () ->
                application.exchange(new ExchangeDesktopAuthCommand(handoffCode, STATE, VERIFIER)));
    assertEquals(DesktopAuthUseCase.RejectionReason.EXPIRED_OR_USED, replay.reason());
  }

  @Test
  void stateOrPkceMismatchConsumesHandoff() {
    String stateMismatchCode = completeAttempt();
    DesktopAuthUseCase.Rejected stateMismatch =
        assertThrows(
            DesktopAuthUseCase.Rejected.class,
            () ->
                application.exchange(
                    new ExchangeDesktopAuthCommand(stateMismatchCode, OTHER_STATE, VERIFIER)));
    assertEquals(DesktopAuthUseCase.RejectionReason.STATE_OR_PKCE_MISMATCH, stateMismatch.reason());
    assertEquals(
        DesktopAuthUseCase.RejectionReason.EXPIRED_OR_USED,
        assertThrows(
                DesktopAuthUseCase.Rejected.class,
                () ->
                    application.exchange(
                        new ExchangeDesktopAuthCommand(stateMismatchCode, STATE, VERIFIER)))
            .reason());

    String pkceMismatchCode = completeAttempt();
    DesktopAuthUseCase.Rejected pkceMismatch =
        assertThrows(
            DesktopAuthUseCase.Rejected.class,
            () ->
                application.exchange(
                    new ExchangeDesktopAuthCommand(pkceMismatchCode, STATE, OTHER_STATE)));
    assertEquals(DesktopAuthUseCase.RejectionReason.STATE_OR_PKCE_MISMATCH, pkceMismatch.reason());
  }

  @Test
  void attemptExpiresAndLoopbackOriginIsStrict() {
    application.start(new StartDesktopAuthCommand(LOOPBACK, STATE, challenge(VERIFIER)));
    clock.advance(Duration.ofSeconds(120));

    assertEquals(
        DesktopAuthUseCase.RejectionReason.EXPIRED_OR_USED,
        assertThrows(
                DesktopAuthUseCase.Rejected.class,
                () ->
                    application.completeProviderCallback(
                        new CompleteProviderCallbackCommand(
                            provider.providerState, "synthetic-provider-code")))
            .reason());
    assertThrows(
        IllegalArgumentException.class,
        () ->
            application.start(
                new StartDesktopAuthCommand(
                    URI.create("http://localhost:49152/callback"), STATE, challenge(VERIFIER))));
    assertThrows(
        IllegalArgumentException.class,
        () ->
            application.start(
                new StartDesktopAuthCommand(
                    URI.create("http://127.0.0.1:49151/callback"), STATE, challenge(VERIFIER))));
  }

  private String completeAttempt() {
    application.start(new StartDesktopAuthCommand(LOOPBACK, STATE, challenge(VERIFIER)));
    URI callback =
        application.completeProviderCallback(
            new CompleteProviderCallbackCommand(provider.providerState, "synthetic-provider-code"));
    return query(callback, "code");
  }

  private static String challenge(String verifier) {
    return DesktopAuthApplication.s256(verifier);
  }

  private static String query(URI uri, String name) {
    for (String pair : uri.getRawQuery().split("&")) {
      String[] parts = pair.split("=", 2);
      if (URLDecoder.decode(parts[0], StandardCharsets.UTF_8).equals(name)) {
        return URLDecoder.decode(parts[1], StandardCharsets.UTF_8);
      }
    }
    throw new AssertionError("missing query " + name);
  }

  private static final class FakeProvider implements ExternalIdentityProvider {
    private String providerState;
    private String providerChallenge;
    private String providerCode;

    @Override
    public URI authorizationUri(String state, String codeChallenge) {
      providerState = state;
      providerChallenge = codeChallenge;
      return URI.create(
          "https://accounts.google.com/o/oauth2/v2/auth?state="
              + state
              + "&code_challenge="
              + codeChallenge);
    }

    @Override
    public ExternalIdentity exchange(String authorizationCode, String codeVerifier) {
      providerCode = authorizationCode;
      assertEquals(43, codeVerifier.length());
      return new ExternalIdentity(
          "https://accounts.google.com", "synthetic-subject", "player@example.invalid");
    }
  }

  private static final class FakeStore implements DesktopAuthStore {
    private final Map<String, Attempt> attempts = new HashMap<>();
    private final Map<String, Handoff> handoffs = new HashMap<>();
    private final Map<String, MetaSession> sessions = new HashMap<>();

    @Override
    public boolean putAttemptIfAbsent(String lookupHash, Attempt attempt) {
      return attempts.putIfAbsent(lookupHash, attempt) == null;
    }

    @Override
    public Optional<Attempt> takeAttempt(String lookupHash) {
      return Optional.ofNullable(attempts.remove(lookupHash));
    }

    @Override
    public boolean putHandoffIfAbsent(String lookupHash, Handoff handoff) {
      return handoffs.putIfAbsent(lookupHash, handoff) == null;
    }

    @Override
    public Optional<Handoff> takeHandoff(String lookupHash) {
      return Optional.ofNullable(handoffs.remove(lookupHash));
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
      throw new AssertionError("desktop auth must not issue evidence admission");
    }

    boolean containsRaw(String value) {
      return attempts.containsKey(value)
          || handoffs.containsKey(value)
          || sessions.containsKey(value);
    }
  }

  private static final class MutableClock extends Clock {
    private Instant now;

    MutableClock(Instant now) {
      this.now = now;
    }

    void advance(Duration duration) {
      now = now.plus(duration);
    }

    @Override
    public ZoneId getZone() {
      return ZoneOffset.UTC;
    }

    @Override
    public Clock withZone(ZoneId zone) {
      return this;
    }

    @Override
    public Instant instant() {
      return now;
    }
  }
}
