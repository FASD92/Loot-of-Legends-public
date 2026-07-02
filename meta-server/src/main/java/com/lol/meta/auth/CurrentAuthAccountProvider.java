package com.lol.meta.auth;

import java.util.Optional;

public interface CurrentAuthAccountProvider {

  Optional<Long> currentAccountId();
}
