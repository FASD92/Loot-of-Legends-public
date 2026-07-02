package com.lol.meta.auth;

import java.util.Collection;
import java.util.Map;
import org.springframework.security.core.GrantedAuthority;
import org.springframework.security.oauth2.core.oidc.OidcIdToken;
import org.springframework.security.oauth2.core.oidc.OidcUserInfo;
import org.springframework.security.oauth2.core.oidc.user.OidcUser;

final class Release0AuthenticatedOidcUser implements OidcUser, Release0AuthenticatedPrincipal {

  private final OidcUser delegate;
  private final long release0AccountId;

  Release0AuthenticatedOidcUser(OidcUser delegate, long release0AccountId) {
    this.delegate = delegate;
    this.release0AccountId = release0AccountId;
  }

  @Override
  public long release0AccountId() {
    return release0AccountId;
  }

  @Override
  public Map<String, Object> getClaims() {
    return delegate.getClaims();
  }

  @Override
  public OidcUserInfo getUserInfo() {
    return delegate.getUserInfo();
  }

  @Override
  public OidcIdToken getIdToken() {
    return delegate.getIdToken();
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
