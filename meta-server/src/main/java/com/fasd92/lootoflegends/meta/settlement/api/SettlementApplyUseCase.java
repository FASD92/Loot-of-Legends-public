package com.fasd92.lootoflegends.meta.settlement.api;

public interface SettlementApplyUseCase {
  ApplyResult applyNext();

  enum ApplyResult {
    IDLE,
    APPLIED
  }

  final class Retryable extends RuntimeException {
    public Retryable(Throwable cause) {
      super(cause);
    }
  }

  final class Invalid extends RuntimeException {
    public Invalid(Throwable cause) {
      super(cause);
    }
  }
}
