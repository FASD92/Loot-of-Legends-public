package com.fasd92.lootoflegends.meta.settlement.application;

import com.fasd92.lootoflegends.meta.identity.api.AccountId;
import com.fasd92.lootoflegends.meta.settlement.api.SettlementUseCase.Status;
import java.util.Optional;

public interface SettlementInbox {
  void insertIfAbsent(
      byte[] settlementId, AccountId accountId, byte[] payloadHash, byte[] canonicalPayload);

  Optional<StoredSettlement> findForUpdate(byte[] settlementId);

  Optional<StoredSettlement> find(byte[] settlementId);

  Optional<StoredSettlement> findNextPendingForUpdate();

  void markApplied(byte[] settlementId);

  long countPending(AccountId accountId);

  record StoredSettlement(
      byte[] settlementId, byte[] payloadHash, byte[] canonicalPayload, Status status) {
    public StoredSettlement {
      settlementId = settlementId.clone();
      payloadHash = payloadHash.clone();
      canonicalPayload = canonicalPayload.clone();
    }

    @Override
    public byte[] settlementId() {
      return settlementId.clone();
    }

    @Override
    public byte[] payloadHash() {
      return payloadHash.clone();
    }

    @Override
    public byte[] canonicalPayload() {
      return canonicalPayload.clone();
    }
  }

  final class Unavailable extends RuntimeException {
    public Unavailable(Throwable cause) {
      super(cause);
    }
  }
}
