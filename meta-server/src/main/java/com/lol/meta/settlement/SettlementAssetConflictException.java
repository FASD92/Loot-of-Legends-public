package com.lol.meta.settlement;

import org.springframework.http.HttpStatus;
import org.springframework.web.bind.annotation.ResponseStatus;

@ResponseStatus(HttpStatus.CONFLICT)
public final class SettlementAssetConflictException extends RuntimeException {

  public SettlementAssetConflictException(String settlementId, String reason) {
    super(
        "Cannot apply settlement assets for settlementId: "
            + settlementId
            + ", reason: "
            + reason);
  }

  public SettlementAssetConflictException(String settlementId, String reason, Throwable cause) {
    super(
        "Cannot apply settlement assets for settlementId: "
            + settlementId
            + ", reason: "
            + reason,
        cause);
  }
}
