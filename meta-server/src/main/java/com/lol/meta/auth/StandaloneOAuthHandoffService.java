package com.lol.meta.auth;

import java.net.URI;
import java.security.SecureRandom;
import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.util.Base64;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.ConcurrentHashMap;
import java.util.regex.Pattern;
import org.springframework.stereotype.Service;

@Service
public final class StandaloneOAuthHandoffService {

  public static final Duration DEFAULT_TTL = Duration.ofSeconds(60);
  private static final int CODE_BYTES = 32;
  private static final int MAX_STATE_LENGTH = 128;
  private static final String CALLBACK_PATH = "/release0/oauth/callback";
  private static final Pattern STATE_PATTERN = Pattern.compile("[A-Za-z0-9._~-]{1,128}");

  private final Clock clock;
  private final Duration ttl;
  private final SecureRandom secureRandom;
  private final Map<String, StandaloneOAuthHandoff> handoffs = new ConcurrentHashMap<>();

  public StandaloneOAuthHandoffService() {
    this(Clock.systemUTC(), DEFAULT_TTL, new SecureRandom());
  }

  private StandaloneOAuthHandoffService(Clock clock, Duration ttl, SecureRandom secureRandom) {
    this.clock = Objects.requireNonNull(clock, "clock");
    this.ttl = Objects.requireNonNull(ttl, "ttl");
    this.secureRandom = Objects.requireNonNull(secureRandom, "secureRandom");
  }

  public static StandaloneOAuthHandoffService forTest() {
    return forTest(Clock.systemUTC());
  }

  public static StandaloneOAuthHandoffService forTest(Clock clock) {
    return new StandaloneOAuthHandoffService(clock, DEFAULT_TTL, new SecureRandom());
  }

  public boolean isAllowedCallback(String callbackUri) {
    if (callbackUri == null || callbackUri.isBlank()) {
      return false;
    }

    URI uri;
    try {
      uri = URI.create(callbackUri);
    } catch (IllegalArgumentException exception) {
      return false;
    }

    String host = uri.getHost();
    return "http".equals(uri.getScheme())
        && ("127.0.0.1".equals(host) || "localhost".equals(host))
        && uri.getPort() > 0
        && CALLBACK_PATH.equals(uri.getPath())
        && uri.getRawQuery() == null
        && uri.getRawFragment() == null;
  }

  public boolean isAllowedState(String state) {
    return state != null
        && state.length() <= MAX_STATE_LENGTH
        && STATE_PATTERN.matcher(state).matches();
  }

  public String issueCode(long accountId, String state) {
    if (accountId <= 0) {
      throw new IllegalArgumentException("accountId must be positive");
    }
    if (!isAllowedState(state)) {
      throw new IllegalArgumentException("valid state is required");
    }

    pruneExpiredHandoffs();
    String code = newCode();
    handoffs.put(code, new StandaloneOAuthHandoff(accountId, state, clock.instant().plus(ttl)));
    return code;
  }

  public ClaimedStandaloneHandoff claimCode(String code, String state) {
    if (code == null || code.isBlank() || !isAllowedState(state)) {
      throw new InvalidHandoffException();
    }

    StandaloneOAuthHandoff handoff = handoffs.remove(code);
    if (handoff == null
        || !clock.instant().isBefore(handoff.expiresAt())
        || !state.equals(handoff.state())) {
      throw new InvalidHandoffException();
    }

    return new ClaimedStandaloneHandoff(handoff.accountId());
  }

  private void pruneExpiredHandoffs() {
    Instant now = clock.instant();
    handoffs.entrySet().removeIf(entry -> !now.isBefore(entry.getValue().expiresAt()));
  }

  private String newCode() {
    byte[] bytes = new byte[CODE_BYTES];
    secureRandom.nextBytes(bytes);
    return Base64.getUrlEncoder().withoutPadding().encodeToString(bytes);
  }

  public record ClaimedStandaloneHandoff(long accountId) {}

  public static final class InvalidHandoffException extends RuntimeException {}
}
