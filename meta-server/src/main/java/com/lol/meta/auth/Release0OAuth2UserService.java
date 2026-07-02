package com.lol.meta.auth;

import java.util.Map;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.security.oauth2.client.userinfo.DefaultOAuth2UserService;
import org.springframework.security.oauth2.client.userinfo.OAuth2UserRequest;
import org.springframework.security.oauth2.client.userinfo.OAuth2UserService;
import org.springframework.security.oauth2.core.user.DefaultOAuth2User;
import org.springframework.security.oauth2.core.user.OAuth2User;
import org.springframework.stereotype.Service;

@Service
public final class Release0OAuth2UserService
    implements OAuth2UserService<OAuth2UserRequest, OAuth2User> {

  private final OAuthAccountResolver oauthAccountResolver;
  private final OAuth2UserService<OAuth2UserRequest, OAuth2User> delegate;

  @Autowired
  public Release0OAuth2UserService(OAuthAccountResolver oauthAccountResolver) {
    this(oauthAccountResolver, new DefaultOAuth2UserService());
  }

  Release0OAuth2UserService(
      OAuthAccountResolver oauthAccountResolver,
      OAuth2UserService<OAuth2UserRequest, OAuth2User> delegate) {
    this.oauthAccountResolver = oauthAccountResolver;
    this.delegate = delegate;
  }

  @Override
  public OAuth2User loadUser(OAuth2UserRequest userRequest) {
    OAuth2User user = delegate.loadUser(userRequest);
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
    OAuth2User normalizedUser =
        new DefaultOAuth2User(user.getAuthorities(), user.getAttributes(), nameAttributeKey);
    return new Release0AuthenticatedOAuth2User(normalizedUser, account.accountId());
  }
}
