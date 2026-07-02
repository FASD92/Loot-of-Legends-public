package com.lol.meta.auth;

final class Release0StandalonePrincipal implements Release0AuthenticatedPrincipal {

  private final long release0AccountId;

  Release0StandalonePrincipal(long release0AccountId) {
    if (release0AccountId <= 0) {
      throw new IllegalArgumentException("release0AccountId must be positive");
    }
    this.release0AccountId = release0AccountId;
  }

  @Override
  public long release0AccountId() {
    return release0AccountId;
  }
}
