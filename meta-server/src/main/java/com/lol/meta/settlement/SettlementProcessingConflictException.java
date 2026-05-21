package com.lol.meta.settlement;

import org.springframework.http.HttpStatus;
import org.springframework.web.bind.annotation.ResponseStatus;

@ResponseStatus(HttpStatus.CONFLICT)
public final class SettlementProcessingConflictException extends RuntimeException {

  public SettlementProcessingConflictException(String settlementId) {
    super("Settlement is already processing for settlementId: " + settlementId);
  }
}
