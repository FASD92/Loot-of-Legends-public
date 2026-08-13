package com.fasd92.lootoflegends.meta.identity.api;

import java.net.URI;
import java.util.Objects;

public record StartDesktopAuthCommand(URI loopbackRedirectUri, String state, String codeChallenge) {
  public StartDesktopAuthCommand {
    Objects.requireNonNull(loopbackRedirectUri, "loopbackRedirectUri");
    Objects.requireNonNull(state, "state");
    Objects.requireNonNull(codeChallenge, "codeChallenge");
  }
}
