package com.fasd92.lootoflegends.meta.identity.application;

import com.fasd92.lootoflegends.meta.identity.api.AccountId;
import com.fasd92.lootoflegends.meta.identity.api.AuthenticatedPrincipal;
import com.fasd92.lootoflegends.meta.identity.api.CompleteProviderCallbackCommand;
import com.fasd92.lootoflegends.meta.identity.api.DesktopAuthUseCase;
import com.fasd92.lootoflegends.meta.identity.api.EvidenceAdmissionUseCase;
import com.fasd92.lootoflegends.meta.identity.api.ExchangeDesktopAuthCommand;
import com.fasd92.lootoflegends.meta.identity.api.IssueEvidenceSessionCommand;
import com.fasd92.lootoflegends.meta.identity.api.IssuedMetaSession;
import com.fasd92.lootoflegends.meta.identity.api.StartDesktopAuthCommand;
import com.fasd92.lootoflegends.meta.identity.api.StartDesktopAuthResult;
import java.net.URI;
import java.net.URLEncoder;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.security.SecureRandom;
import java.time.Clock;
import java.time.Instant;
import java.util.Base64;
import java.util.HexFormat;
import java.util.Optional;
import java.util.UUID;
import java.util.regex.Pattern;
import org.springframework.stereotype.Service;

@Service
public final class DesktopAuthApplication implements DesktopAuthUseCase, EvidenceAdmissionUseCase {
  private static final int RANDOM_BYTES = 32;
  private static final int HANDOFF_TTL_SECONDS = 120;
  private static final int SESSION_TTL_SECONDS = 3600;
  private static final int MAX_TOKEN_ATTEMPTS = 4;
  private static final Pattern CLIENT_SECRET = Pattern.compile("[A-Za-z0-9._~-]{43,128}");
  private static final Pattern BASE64_URL = Pattern.compile("[A-Za-z0-9_-]{43}");
  private static final Pattern EVIDENCE_RUN_ID = Pattern.compile("[A-Za-z0-9][A-Za-z0-9._-]{0,63}");
  private static final String EVIDENCE_ISSUER = "urn:loot-of-legends:evidence";

  private final DesktopAuthStore store;
  private final ExternalIdentityProvider provider;
  private final Clock clock;
  private final SecureRandom random;

  public DesktopAuthApplication(
      DesktopAuthStore store, ExternalIdentityProvider provider, Clock clock, SecureRandom random) {
    this.store = store;
    this.provider = provider;
    this.clock = clock;
    this.random = random;
  }

  @Override
  public StartDesktopAuthResult start(StartDesktopAuthCommand command) {
    validateLoopback(command.loopbackRedirectUri());
    validateClientSecret(command.state(), "state");
    validateBase64Url(command.codeChallenge(), "codeChallenge");

    Instant expiresAt = clock.instant().plusSeconds(HANDOFF_TTL_SECONDS);
    for (int attemptNumber = 0; attemptNumber < MAX_TOKEN_ATTEMPTS; attemptNumber++) {
      String providerState = randomToken();
      String providerCodeVerifier = randomToken();
      DesktopAuthStore.Attempt attempt =
          new DesktopAuthStore.Attempt(
              command.loopbackRedirectUri(),
              command.state(),
              command.codeChallenge(),
              providerCodeVerifier,
              expiresAt);
      try {
        if (store.putAttemptIfAbsent(lookupHash(providerState), attempt)) {
          return new StartDesktopAuthResult(
              provider.authorizationUri(providerState, s256(providerCodeVerifier)), expiresAt);
        }
      } catch (DesktopAuthStore.Unavailable | ExternalIdentityProvider.Unavailable exception) {
        throw new DesktopAuthUseCase.Rejected(
            DesktopAuthUseCase.RejectionReason.DEPENDENCY_UNAVAILABLE, exception);
      }
    }
    throw new DesktopAuthUseCase.Rejected(
        DesktopAuthUseCase.RejectionReason.DEPENDENCY_UNAVAILABLE);
  }

  @Override
  public URI completeProviderCallback(CompleteProviderCallbackCommand command) {
    validateBase64Url(command.providerState(), "providerState");
    if (command.authorizationCode().isBlank()) {
      throw new IllegalArgumentException("authorizationCode must not be blank");
    }

    DesktopAuthStore.Attempt attempt;
    try {
      attempt =
          store
              .takeAttempt(lookupHash(command.providerState()))
              .filter(candidate -> clock.instant().isBefore(candidate.expiresAt()))
              .orElseThrow(
                  () ->
                      new DesktopAuthUseCase.Rejected(
                          DesktopAuthUseCase.RejectionReason.EXPIRED_OR_USED));
    } catch (DesktopAuthStore.Unavailable exception) {
      throw new DesktopAuthUseCase.Rejected(
          DesktopAuthUseCase.RejectionReason.DEPENDENCY_UNAVAILABLE, exception);
    }

    ExternalIdentityProvider.ExternalIdentity externalIdentity;
    try {
      externalIdentity =
          provider.exchange(command.authorizationCode(), attempt.providerCodeVerifier());
    } catch (ExternalIdentityProvider.Rejected exception) {
      throw new DesktopAuthUseCase.Rejected(
          DesktopAuthUseCase.RejectionReason.PROVIDER_REJECTED, exception);
    } catch (ExternalIdentityProvider.Unavailable exception) {
      throw new DesktopAuthUseCase.Rejected(
          DesktopAuthUseCase.RejectionReason.DEPENDENCY_UNAVAILABLE, exception);
    }

    AuthenticatedPrincipal principal = principalFor(externalIdentity);
    String handoffCode = putHandoff(principal, attempt);
    return URI.create(
        attempt.loopbackRedirectUri()
            + "?code="
            + encode(handoffCode)
            + "&state="
            + encode(attempt.clientState()));
  }

  @Override
  public IssuedMetaSession exchange(ExchangeDesktopAuthCommand command) {
    validateBase64Url(command.handoffCode(), "handoffCode");
    validateClientSecret(command.state(), "state");
    validateClientSecret(command.codeVerifier(), "codeVerifier");

    DesktopAuthStore.Handoff handoff;
    try {
      handoff =
          store
              .takeHandoff(lookupHash(command.handoffCode()))
              .filter(candidate -> clock.instant().isBefore(candidate.expiresAt()))
              .orElseThrow(
                  () ->
                      new DesktopAuthUseCase.Rejected(
                          DesktopAuthUseCase.RejectionReason.EXPIRED_OR_USED));
    } catch (DesktopAuthStore.Unavailable exception) {
      throw new DesktopAuthUseCase.Rejected(
          DesktopAuthUseCase.RejectionReason.DEPENDENCY_UNAVAILABLE, exception);
    }

    if (!secureEquals(handoff.clientState(), command.state())
        || !secureEquals(handoff.clientCodeChallenge(), s256(command.codeVerifier()))) {
      throw new DesktopAuthUseCase.Rejected(
          DesktopAuthUseCase.RejectionReason.STATE_OR_PKCE_MISMATCH);
    }

    return putSession(handoff.principal());
  }

  @Override
  public Optional<AuthenticatedPrincipal> authenticate(String metaSession) {
    if (metaSession == null || !BASE64_URL.matcher(metaSession).matches()) {
      return Optional.empty();
    }
    try {
      return store
          .findSession(lookupHash(metaSession))
          .filter(session -> clock.instant().isBefore(session.expiresAt()))
          .map(DesktopAuthStore.MetaSession::principal);
    } catch (DesktopAuthStore.Unavailable exception) {
      throw new DesktopAuthUseCase.Rejected(
          DesktopAuthUseCase.RejectionReason.DEPENDENCY_UNAVAILABLE, exception);
    }
  }

  @Override
  public IssuedMetaSession issueEvidenceSession(IssueEvidenceSessionCommand command) {
    Instant now = clock.instant();
    if (command == null
        || command.runId() == null
        || !EVIDENCE_RUN_ID.matcher(command.runId()).matches()
        || command.participantIndex() <= 0
        || command.expiresAt() == null
        || !now.isBefore(command.expiresAt())
        || command.expiresAt().isAfter(now.plusSeconds(24 * 60 * 60))) {
      throw new IllegalArgumentException("invalid evidence admission");
    }

    String subject = command.runId() + ":" + command.participantIndex();
    try {
      if (!store.putEvidenceAdmissionIfAbsent(
          lookupHash(EVIDENCE_ISSUER + "\u0000" + subject), command.expiresAt())) {
        throw new EvidenceAdmissionUseCase.Rejected(
            EvidenceAdmissionUseCase.RejectionReason.REPLAYED);
      }
    } catch (DesktopAuthStore.Unavailable exception) {
      throw new EvidenceAdmissionUseCase.Rejected(
          EvidenceAdmissionUseCase.RejectionReason.DEPENDENCY_UNAVAILABLE, exception);
    }

    UUID uuid = uuidFor(EVIDENCE_ISSUER, subject);
    return putEvidenceSession(
        new AuthenticatedPrincipal(
            new AccountId(uuid), "participant-%04d".formatted(command.participantIndex())),
        command.expiresAt());
  }

  public static String s256(String verifier) {
    return Base64.getUrlEncoder()
        .withoutPadding()
        .encodeToString(sha256(verifier.getBytes(StandardCharsets.US_ASCII)));
  }

  private String putHandoff(AuthenticatedPrincipal principal, DesktopAuthStore.Attempt attempt) {
    Instant expiresAt = clock.instant().plusSeconds(HANDOFF_TTL_SECONDS);
    for (int attemptNumber = 0; attemptNumber < MAX_TOKEN_ATTEMPTS; attemptNumber++) {
      String handoffCode = randomToken();
      DesktopAuthStore.Handoff handoff =
          new DesktopAuthStore.Handoff(
              principal, attempt.clientState(), attempt.clientCodeChallenge(), expiresAt);
      try {
        if (store.putHandoffIfAbsent(lookupHash(handoffCode), handoff)) {
          return handoffCode;
        }
      } catch (DesktopAuthStore.Unavailable exception) {
        throw new DesktopAuthUseCase.Rejected(
            DesktopAuthUseCase.RejectionReason.DEPENDENCY_UNAVAILABLE, exception);
      }
    }
    throw new DesktopAuthUseCase.Rejected(
        DesktopAuthUseCase.RejectionReason.DEPENDENCY_UNAVAILABLE);
  }

  private IssuedMetaSession putSession(AuthenticatedPrincipal principal) {
    return putSession(principal, clock.instant().plusSeconds(SESSION_TTL_SECONDS));
  }

  private IssuedMetaSession putEvidenceSession(
      AuthenticatedPrincipal principal, Instant expiresAt) {
    try {
      return putSession(principal, expiresAt);
    } catch (DesktopAuthUseCase.Rejected rejected) {
      throw new EvidenceAdmissionUseCase.Rejected(
          EvidenceAdmissionUseCase.RejectionReason.DEPENDENCY_UNAVAILABLE, rejected);
    }
  }

  private IssuedMetaSession putSession(AuthenticatedPrincipal principal, Instant expiresAt) {
    for (int attemptNumber = 0; attemptNumber < MAX_TOKEN_ATTEMPTS; attemptNumber++) {
      String metaSession = randomToken();
      try {
        if (store.putSessionIfAbsent(
            lookupHash(metaSession), new DesktopAuthStore.MetaSession(principal, expiresAt))) {
          return new IssuedMetaSession(metaSession, expiresAt);
        }
      } catch (DesktopAuthStore.Unavailable exception) {
        throw new DesktopAuthUseCase.Rejected(
            DesktopAuthUseCase.RejectionReason.DEPENDENCY_UNAVAILABLE, exception);
      }
    }
    throw new DesktopAuthUseCase.Rejected(
        DesktopAuthUseCase.RejectionReason.DEPENDENCY_UNAVAILABLE);
  }

  private String randomToken() {
    byte[] bytes = new byte[RANDOM_BYTES];
    random.nextBytes(bytes);
    return Base64.getUrlEncoder().withoutPadding().encodeToString(bytes);
  }

  private static AuthenticatedPrincipal principalFor(
      ExternalIdentityProvider.ExternalIdentity externalIdentity) {
    if (externalIdentity.issuer() == null
        || externalIdentity.issuer().isBlank()
        || externalIdentity.subject() == null
        || externalIdentity.subject().isBlank()) {
      throw new DesktopAuthUseCase.Rejected(DesktopAuthUseCase.RejectionReason.PROVIDER_REJECTED);
    }
    UUID uuid = uuidFor(externalIdentity.issuer(), externalIdentity.subject());
    String nickname = "player-" + uuid.toString().replace("-", "").substring(0, 8);
    return new AuthenticatedPrincipal(new AccountId(uuid), nickname);
  }

  private static UUID uuidFor(String issuer, String subject) {
    byte[] digest = sha256((issuer + "\u0000" + subject).getBytes(StandardCharsets.UTF_8));
    digest[6] = (byte) ((digest[6] & 0x0f) | 0x40);
    digest[8] = (byte) ((digest[8] & 0x3f) | 0x80);
    ByteBuffer buffer = ByteBuffer.wrap(digest);
    return new UUID(buffer.getLong(), buffer.getLong());
  }

  private static String lookupHash(String rawToken) {
    return HexFormat.of().formatHex(sha256(rawToken.getBytes(StandardCharsets.US_ASCII)));
  }

  private static byte[] sha256(byte[] input) {
    try {
      return MessageDigest.getInstance("SHA-256").digest(input);
    } catch (NoSuchAlgorithmException impossible) {
      throw new IllegalStateException(impossible);
    }
  }

  private static boolean secureEquals(String left, String right) {
    return MessageDigest.isEqual(
        left.getBytes(StandardCharsets.UTF_8), right.getBytes(StandardCharsets.UTF_8));
  }

  private static String encode(String value) {
    return URLEncoder.encode(value, StandardCharsets.UTF_8);
  }

  private static void validateLoopback(URI uri) {
    if (!"http".equals(uri.getScheme())
        || !"127.0.0.1".equals(uri.getHost())
        || uri.getPort() < 49152
        || uri.getPort() > 65535
        || !"/callback".equals(uri.getPath())
        || uri.getUserInfo() != null
        || uri.getQuery() != null
        || uri.getFragment() != null) {
      throw new IllegalArgumentException("invalid desktop auth loopback URI");
    }
  }

  private static void validateClientSecret(String value, String name) {
    if (!CLIENT_SECRET.matcher(value).matches()) {
      throw new IllegalArgumentException("invalid " + name);
    }
  }

  private static void validateBase64Url(String value, String name) {
    if (!BASE64_URL.matcher(value).matches()) {
      throw new IllegalArgumentException("invalid " + name);
    }
  }
}
