package com.lol.meta.settlement;

import org.springframework.http.HttpStatus;
import org.springframework.web.bind.annotation.ResponseStatus;

@ResponseStatus(HttpStatus.CONFLICT)
public final class SettlementConflictException extends RuntimeException {

  public SettlementConflictException(String settlementId) {
    super("Different settlement payload for settlementId: " + settlementId);
  }
}
