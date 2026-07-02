package com.lol.meta.auth;

import java.util.Collection;
import java.util.Map;
import org.springframework.security.core.GrantedAuthority;
import org.springframework.security.oauth2.core.user.OAuth2User;

final class Release0AuthenticatedOAuth2User
    implements OAuth2User, Release0AuthenticatedPrincipal {

  private final OAuth2User delegate;
  private final long release0AccountId;

  Release0AuthenticatedOAuth2User(OAuth2User delegate, long release0AccountId) {
    this.delegate = delegate;
    this.release0AccountId = release0AccountId;
  }

  @Override
  public long release0AccountId() {
    return release0AccountId;
  }

  @Override
  public Map<String, Object> getAttributes() {
    return delegate.getAttributes();
  }

  @Override
  public Collection<? extends GrantedAuthority> getAuthorities() {
    return delegate.getAuthorities();
  }

  @Override
  public String getName() {
    return delegate.getName();
  }
}
