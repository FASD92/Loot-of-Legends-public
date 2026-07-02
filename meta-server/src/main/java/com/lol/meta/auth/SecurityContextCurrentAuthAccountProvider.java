package com.lol.meta.auth;

import java.util.Optional;
import org.springframework.security.core.Authentication;
import org.springframework.security.core.context.SecurityContextHolder;
import org.springframework.stereotype.Component;

@Component
public final class SecurityContextCurrentAuthAccountProvider implements CurrentAuthAccountProvider {

  @Override
  public Optional<Long> currentAccountId() {
    Authentication authentication = SecurityContextHolder.getContext().getAuthentication();
    if (authentication == null
        || !(authentication.getPrincipal() instanceof Release0AuthenticatedPrincipal principal)) {
      return Optional.empty();
    }
    long accountId = principal.release0AccountId();
    return accountId > 0 ? Optional.of(accountId) : Optional.empty();
  }
}
