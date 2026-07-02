package com.lol.meta.auth;

import java.util.Map;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.security.oauth2.client.oidc.userinfo.OidcUserRequest;
import org.springframework.security.oauth2.client.oidc.userinfo.OidcUserService;
import org.springframework.security.oauth2.core.oidc.OidcUserInfo;
import org.springframework.security.oauth2.core.oidc.user.DefaultOidcUser;
import org.springframework.security.oauth2.core.oidc.user.OidcUser;
import org.springframework.stereotype.Service;

@Service
public final class Release0OidcUserService extends OidcUserService {

  private final OAuthAccountResolver oauthAccountResolver;
  private final OidcUserService delegate;

  @Autowired
  public Release0OidcUserService(OAuthAccountResolver oauthAccountResolver) {
    this(oauthAccountResolver, new OidcUserService());
  }

  Release0OidcUserService(OAuthAccountResolver oauthAccountResolver, OidcUserService delegate) {
    this.oauthAccountResolver = oauthAccountResolver;
    this.delegate = delegate;
  }

  @Override
  public OidcUser loadUser(OidcUserRequest userRequest) {
    OidcUser user = delegate.loadUser(userRequest);
    String provider = userRequest.getClientRegistration().getRegistrationId();
    String providerSubject = Release0OAuthAccountAttributes.requireGoogleSubject(user);
    PlayerAccount account = oauthAccountResolver.resolveOAuthAccount(provider, providerSubject);
    String nameAttributeKey =
        Release0OAuthAccountAttributes.nameAttributeKey(
            userRequest
                .getClientRegistration()
                .getProviderDetails()
                .getUserInfoEndpoint()
                .getUserNameAttributeName());
    OidcUser normalizedUser =
        user.getUserInfo() == null
            ? new DefaultOidcUser(user.getAuthorities(), user.getIdToken(), nameAttributeKey)
            : new DefaultOidcUser(
                user.getAuthorities(), user.getIdToken(), user.getUserInfo(), nameAttributeKey);
    return new Release0AuthenticatedOidcUser(normalizedUser, account.accountId());
  }
}
