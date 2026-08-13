package com.fasd92.lootoflegends.meta.identity.api;

import java.util.Objects;

public record AuthenticatedPrincipal(AccountId accountId, String nickname) {
  public AuthenticatedPrincipal {
    Objects.requireNonNull(accountId, "accountId");
    if (nickname == null || nickname.isBlank()) {
      throw new IllegalArgumentException("nickname must not be blank");
    }
  }
}
