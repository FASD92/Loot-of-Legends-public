package com.fasd92.lootoflegends.meta.identity.application;

import java.net.URI;

public interface ExternalIdentityProvider {
  URI authorizationUri(String state, String codeChallenge);

  ExternalIdentity exchange(String authorizationCode, String codeVerifier);

  record ExternalIdentity(String issuer, String subject, String email) {}

  final class Rejected extends RuntimeException {
    public Rejected(Throwable cause) {
      super(cause);
    }
  }

  final class Unavailable extends RuntimeException {
    public Unavailable(Throwable cause) {
      super(cause);
    }
  }
}
