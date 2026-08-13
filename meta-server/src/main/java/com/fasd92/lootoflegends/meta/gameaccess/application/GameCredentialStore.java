package com.fasd92.lootoflegends.meta.gameaccess.application;

import com.fasd92.lootoflegends.meta.gameaccess.api.ClaimGameCredentialResult;
import com.fasd92.lootoflegends.meta.identity.api.AccountId;
import java.time.Instant;

public interface GameCredentialStore {
  boolean putIfAbsent(
      String hash,
      AccountId accountId,
      String nickname,
      String audience,
      Instant expiresAt,
      Instant purgeAt);

  ClaimGameCredentialResult claim(String hash, String audience, Instant now);

  final class Unavailable extends RuntimeException {
    public Unavailable(Throwable cause) {
      super(cause);
    }
  }
}
