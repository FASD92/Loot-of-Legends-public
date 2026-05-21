package com.lol.meta.settlement;

import org.springframework.http.HttpStatus;
import org.springframework.web.bind.annotation.ResponseStatus;

@ResponseStatus(HttpStatus.UNAUTHORIZED)
public final class SettlementSessionUnauthorizedException extends RuntimeException {

  public SettlementSessionUnauthorizedException() {
    super("Settlement session token is invalid");
  }
}
