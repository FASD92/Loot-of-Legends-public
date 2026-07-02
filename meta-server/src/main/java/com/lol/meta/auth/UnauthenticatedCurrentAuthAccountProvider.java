package com.lol.meta.auth;

import java.util.Optional;
import org.springframework.boot.autoconfigure.condition.ConditionalOnMissingBean;
import org.springframework.stereotype.Component;

@Component
@ConditionalOnMissingBean(CurrentAuthAccountProvider.class)
public final class UnauthenticatedCurrentAuthAccountProvider implements CurrentAuthAccountProvider {

  @Override
  public Optional<Long> currentAccountId() {
    return Optional.empty();
  }
}
