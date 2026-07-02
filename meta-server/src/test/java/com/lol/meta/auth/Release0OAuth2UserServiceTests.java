package com.lol.meta.auth;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import java.time.Instant;
import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.Test;
import org.springframework.security.core.authority.SimpleGrantedAuthority;
import org.springframework.security.oauth2.client.registration.ClientRegistration;
import org.springframework.security.oauth2.client.userinfo.OAuth2UserRequest;
import org.springframework.security.oauth2.client.userinfo.OAuth2UserService;
import org.springframework.security.oauth2.core.AuthorizationGrantType;
import org.springframework.security.oauth2.core.OAuth2AccessToken;
import org.springframework.security.oauth2.core.OAuth2AuthenticationException;
import org.springframework.security.oauth2.core.user.DefaultOAuth2User;
import org.springframework.security.oauth2.core.user.OAuth2User;

class Release0OAuth2UserServiceTests {

  @Test
  void attachesRelease0AccountIdResolvedFromGoogleSubject() {
    FakeOAuthAccountResolver resolver = new FakeOAuthAccountResolver(new PlayerAccount(456L, null));
    OAuth2UserService<OAuth2UserRequest, OAuth2User> delegate =
        request ->
            new DefaultOAuth2User(
                List.of(new SimpleGrantedAuthority("ROLE_USER")),
                Map.of("sub", "google-subject-1", "email", "player@example.com"),
                "sub");
    Release0OAuth2UserService userService = new Release0OAuth2UserService(resolver, delegate);

    OAuth2User user = userService.loadUser(oAuth2UserRequest());

    assertThat(resolver.provider).isEqualTo("google");
    assertThat(resolver.providerSubject).isEqualTo("google-subject-1");
    assertThat(user.getName()).isEqualTo("google-subject-1");
    assertThat(user).isInstanceOf(Release0AuthenticatedPrincipal.class);
    assertThat(((Release0AuthenticatedPrincipal) user).release0AccountId()).isEqualTo(456L);
    Long accountId = user.getAttribute("release0AccountId");
    String email = user.getAttribute("email");
    assertThat(accountId).isNull();
    assertThat(email).isEqualTo("player@example.com");
  }

  @Test
  void rejectsGoogleOAuthUserWithoutSubject() {
    FakeOAuthAccountResolver resolver = new FakeOAuthAccountResolver(new PlayerAccount(456L, null));
    OAuth2UserService<OAuth2UserRequest, OAuth2User> delegate =
        request ->
            new DefaultOAuth2User(
                List.of(new SimpleGrantedAuthority("ROLE_USER")),
                Map.of("email", "player@example.com"),
                "email");
    Release0OAuth2UserService userService = new Release0OAuth2UserService(resolver, delegate);

    assertThatThrownBy(() -> userService.loadUser(oAuth2UserRequest()))
        .isInstanceOf(OAuth2AuthenticationException.class);
  }

  private static OAuth2UserRequest oAuth2UserRequest() {
    ClientRegistration registration =
        ClientRegistration.withRegistrationId("google")
            .clientId("client-id")
            .clientSecret("client-secret")
            .authorizationGrantType(AuthorizationGrantType.AUTHORIZATION_CODE)
            .redirectUri("{baseUrl}/login/oauth2/code/{registrationId}")
            .authorizationUri("https://accounts.google.com/o/oauth2/v2/auth")
            .tokenUri("https://oauth2.googleapis.com/token")
            .userInfoUri("https://openidconnect.googleapis.com/v1/userinfo")
            .userNameAttributeName("sub")
            .scope("openid", "profile", "email")
            .build();
    OAuth2AccessToken accessToken =
        new OAuth2AccessToken(
            OAuth2AccessToken.TokenType.BEARER,
            "token",
            Instant.now(),
            Instant.now().plusSeconds(60));
    return new OAuth2UserRequest(registration, accessToken);
  }

  private static final class FakeOAuthAccountResolver implements OAuthAccountResolver {

    private final PlayerAccount account;
    private String provider;
    private String providerSubject;

    FakeOAuthAccountResolver(PlayerAccount account) {
      this.account = account;
    }

    @Override
    public PlayerAccount resolveOAuthAccount(String provider, String providerSubject) {
      this.provider = provider;
      this.providerSubject = providerSubject;
      return account;
    }
  }
}
