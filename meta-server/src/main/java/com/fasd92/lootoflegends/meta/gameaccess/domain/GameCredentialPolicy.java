package com.fasd92.lootoflegends.meta.gameaccess.domain;

import java.time.Duration;
import java.util.regex.Pattern;

public final class GameCredentialPolicy {
  public static final int ENTROPY_BYTES = 32;
  public static final Duration TTL = Duration.ofSeconds(30);
  public static final Duration OUTCOME_RETENTION = Duration.ofSeconds(30);
  public static final String AUDIENCE = "loot-game-server-v1";

  private static final Pattern ENCODED = Pattern.compile("[A-Za-z0-9_-]{43}");

  private GameCredentialPolicy() {}

  public static boolean isEncodedCredential(String value) {
    return value != null && ENCODED.matcher(value).matches();
  }
}
