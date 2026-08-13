package com.fasd92.lootoflegends.meta.gameaccess.api;

import com.fasd92.lootoflegends.meta.identity.api.AccountId;
import java.util.Objects;

public record IssueGameCredentialCommand(AccountId accountId, String nickname) {
  public IssueGameCredentialCommand {
    Objects.requireNonNull(accountId, "accountId");
    if (nickname == null || nickname.isBlank()) {
      throw new IllegalArgumentException("nickname must not be blank");
    }
  }
}
