package com.fasd92.lootoflegends.meta.settlement.api;

import com.fasd92.lootoflegends.meta.identity.api.AccountId;

public interface SettlementPendingQuery {
  long countPending(AccountId accountId);

  final class Unavailable extends RuntimeException {
    public Unavailable(Throwable cause) {
      super(cause);
    }
  }
}
