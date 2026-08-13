package com.fasd92.lootoflegends.meta.identity.api;

import java.net.URI;
import java.util.Optional;

public interface DesktopAuthUseCase {
  StartDesktopAuthResult start(StartDesktopAuthCommand command);

  URI completeProviderCallback(CompleteProviderCallbackCommand command);

  IssuedMetaSession exchange(ExchangeDesktopAuthCommand command);

  Optional<AuthenticatedPrincipal> authenticate(String metaSession);

  enum RejectionReason {
    EXPIRED_OR_USED,
    STATE_OR_PKCE_MISMATCH,
    PROVIDER_REJECTED,
    DEPENDENCY_UNAVAILABLE
  }

  final class Rejected extends RuntimeException {
    private final RejectionReason reason;

    public Rejected(RejectionReason reason) {
      this(reason, null);
    }

    public Rejected(RejectionReason reason, Throwable cause) {
      super(reason.name(), cause);
      this.reason = reason;
    }

    public RejectionReason reason() {
      return reason;
    }
  }
}
