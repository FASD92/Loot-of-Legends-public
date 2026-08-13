package com.fasd92.lootoflegends.meta.identity.api;

import java.time.Instant;
import java.util.Objects;

public record IssuedMetaSession(String metaSession, Instant expiresAt) {
  public IssuedMetaSession {
    Objects.requireNonNull(metaSession, "metaSession");
    Objects.requireNonNull(expiresAt, "expiresAt");
  }
}
