package com.fasd92.lootoflegends.meta.gameaccess.api;

import java.time.Instant;
import java.util.Objects;

public record IssuedGameCredential(String credential, Instant expiresAt) {
  public IssuedGameCredential {
    Objects.requireNonNull(credential, "credential");
    Objects.requireNonNull(expiresAt, "expiresAt");
  }
}
