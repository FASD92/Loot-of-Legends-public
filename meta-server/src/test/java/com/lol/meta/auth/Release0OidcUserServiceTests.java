package com.lol.meta.auth;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import java.time.Instant;
import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.Test;
import org.springframework.security.core.authority.SimpleGrantedAuthority;
import org.springframework.security.oauth2.client.oidc.userinfo.OidcUserRequest;
import org.springframework.security.oauth2.client.oidc.userinfo.OidcUserService;
import org.springframework.security.oauth2.client.registration.ClientRegistration;
import org.springframework.security.oauth2.core.AuthorizationGrantType;
import org.springframework.security.oauth2.core.OAuth2AccessToken;
import org.springframework.security.oauth2.core.OAuth2AuthenticationException;
import org.springframework.security.oauth2.core.oidc.OidcIdToken;
import org.springframework.security.oauth2.core.oidc.OidcUserInfo;
import org.springframework.security.oauth2.core.oidc.user.DefaultOidcUser;
import org.springframework.security.oauth2.core.oidc.user.OidcUser;

class Release0OidcUserServiceTests {

  @Test
  void attachesRelease0AccountIdResolvedFromGoogleOidcSubject() {
    FakeOAuthAccountResolver resolver = new FakeOAuthAccountResolver(new PlayerAccount(456L, null));
    OidcUserService delegate =
        new OidcUserService() {
          @Override
          public OidcUser loadUser(OidcUserRequest userRequest) {
            return new DefaultOidcUser(
                List.of(new SimpleGrantedAuthority("ROLE_USER")),
                idToken(Map.of("sub", "google-subject-1")),
                userInfo(Map.of("email", "player@example.com")),
                "sub");
          }
        };
    Release0OidcUserService userService = new Release0OidcUserService(resolver, delegate);

    OidcUser user = userService.loadUser(oidcUserRequest(Map.of("sub", "google-subject-1")));

    assertThat(resolver.provider).isEqualTo("google");
    assertThat(resolver.providerSubject).isEqualTo("google-subject-1");
    assertThat(user).isInstanceOf(Release0AuthenticatedPrincipal.class);
    assertThat(((Release0AuthenticatedPrincipal) user).release0AccountId()).isEqualTo(456L);
    Long accountId = user.getAttribute("release0AccountId");
    String email = user.getAttribute("email");
    assertThat(accountId).isNull();
    assertThat(email).isEqualTo("player@example.com");
    assertThat(user.getIdToken().getClaims()).doesNotContainKey("release0AccountId");
    assertThat(user.getUserInfo().getClaims()).doesNotContainKey("release0AccountId");
  }

  @Test
  void rejectsGoogleOidcUserWithoutSubject() {
    FakeOAuthAccountResolver resolver = new FakeOAuthAccountResolver(new PlayerAccount(456L, null));
    OidcUserService delegate =
        new OidcUserService() {
          @Override
          public OidcUser loadUser(OidcUserRequest userRequest) {
            return new DefaultOidcUser(
                List.of(new SimpleGrantedAuthority("ROLE_USER")),
                idToken(Map.of("email", "player@example.com")),
                "email");
          }
        };
    Release0OidcUserService userService = new Release0OidcUserService(resolver, delegate);

    assertThatThrownBy(
            () -> userService.loadUser(oidcUserRequest(Map.of("email", "player@example.com"))))
        .isInstanceOf(OAuth2AuthenticationException.class);
  }

  private static OidcUserRequest oidcUserRequest(Map<String, Object> claims) {
    return new OidcUserRequest(googleRegistration(), accessToken(), idToken(claims));
  }

  private static ClientRegistration googleRegistration() {
    return ClientRegistration.withRegistrationId("google")
        .clientId("client-id")
        .clientSecret("client-secret")
        .authorizationGrantType(AuthorizationGrantType.AUTHORIZATION_CODE)
        .redirectUri("{baseUrl}/login/oauth2/code/{registrationId}")
        .authorizationUri("https://accounts.google.com/o/oauth2/v2/auth")
        .tokenUri("https://oauth2.googleapis.com/token")
        .jwkSetUri("https://www.googleapis.com/oauth2/v3/certs")
        .issuerUri("https://accounts.google.com")
        .userInfoUri("https://openidconnect.googleapis.com/v1/userinfo")
        .userNameAttributeName("sub")
        .scope("openid", "profile", "email")
        .build();
  }

  private static OAuth2AccessToken accessToken() {
    return new OAuth2AccessToken(
        OAuth2AccessToken.TokenType.BEARER,
        "token",
        Instant.now(),
        Instant.now().plusSeconds(60));
  }

  private static OidcIdToken idToken(Map<String, Object> claims) {
    return new OidcIdToken("id-token", Instant.now(), Instant.now().plusSeconds(60), claims);
  }

  private static OidcUserInfo userInfo(Map<String, Object> claims) {
    return new OidcUserInfo(claims);
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
