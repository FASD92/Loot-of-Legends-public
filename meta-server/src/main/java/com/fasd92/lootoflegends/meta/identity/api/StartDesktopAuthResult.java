package com.fasd92.lootoflegends.meta.identity.api;

import java.net.URI;
import java.time.Instant;
import java.util.Objects;

public record StartDesktopAuthResult(URI authorizationUrl, Instant expiresAt) {
  public StartDesktopAuthResult {
    Objects.requireNonNull(authorizationUrl, "authorizationUrl");
    Objects.requireNonNull(expiresAt, "expiresAt");
  }
}
