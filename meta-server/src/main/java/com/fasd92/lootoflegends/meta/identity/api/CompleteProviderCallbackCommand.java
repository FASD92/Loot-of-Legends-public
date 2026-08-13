package com.fasd92.lootoflegends.meta.identity.api;

import java.util.Objects;

public record CompleteProviderCallbackCommand(String providerState, String authorizationCode) {
  public CompleteProviderCallbackCommand {
    Objects.requireNonNull(providerState, "providerState");
    Objects.requireNonNull(authorizationCode, "authorizationCode");
  }
}
