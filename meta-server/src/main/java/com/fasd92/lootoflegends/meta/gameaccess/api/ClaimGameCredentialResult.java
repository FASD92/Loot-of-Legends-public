package com.fasd92.lootoflegends.meta.gameaccess.api;

import com.fasd92.lootoflegends.meta.identity.api.AccountId;
import java.util.Objects;

public sealed interface ClaimGameCredentialResult {
  record Claimed(AccountId accountId, String nickname) implements ClaimGameCredentialResult {
    public Claimed {
      Objects.requireNonNull(accountId, "accountId");
      if (nickname == null || nickname.isBlank()) {
        throw new IllegalArgumentException("nickname must not be blank");
      }
    }
  }

  record Rejected(ClaimRejectionReason reason) implements ClaimGameCredentialResult {
    public Rejected {
      Objects.requireNonNull(reason, "reason");
    }
  }
}
