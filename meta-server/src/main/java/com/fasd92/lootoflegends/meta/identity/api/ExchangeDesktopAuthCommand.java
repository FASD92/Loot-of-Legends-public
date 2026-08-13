package com.fasd92.lootoflegends.meta.identity.api;

import java.util.Objects;

public record ExchangeDesktopAuthCommand(String handoffCode, String state, String codeVerifier) {
  public ExchangeDesktopAuthCommand {
    Objects.requireNonNull(handoffCode, "handoffCode");
    Objects.requireNonNull(state, "state");
    Objects.requireNonNull(codeVerifier, "codeVerifier");
  }
}
