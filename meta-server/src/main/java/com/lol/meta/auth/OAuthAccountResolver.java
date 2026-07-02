package com.lol.meta.auth;

public interface OAuthAccountResolver {

  PlayerAccount resolveOAuthAccount(String provider, String providerSubject);
}
