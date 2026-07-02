package com.lol.meta.auth;

import static org.assertj.core.api.Assertions.assertThat;
import static org.hamcrest.Matchers.containsString;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.header;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.lol.meta.internal.InternalTokenVerifier;
import jakarta.servlet.Filter;
import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.beans.factory.annotation.Qualifier;
import org.springframework.boot.test.autoconfigure.web.servlet.WebMvcTest;
import org.springframework.boot.test.context.TestConfiguration;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Import;
import org.springframework.security.authentication.AuthenticationProvider;
import org.springframework.security.authentication.ProviderManager;
import org.springframework.security.core.authority.SimpleGrantedAuthority;
import org.springframework.security.oauth2.client.authentication.OAuth2LoginAuthenticationProvider;
import org.springframework.security.oauth2.client.oidc.authentication.OidcAuthorizationCodeAuthenticationProvider;
import org.springframework.security.oauth2.client.registration.ClientRegistration;
import org.springframework.security.oauth2.client.registration.ClientRegistrationRepository;
import org.springframework.security.oauth2.client.registration.InMemoryClientRegistrationRepository;
import org.springframework.security.oauth2.client.userinfo.OAuth2UserRequest;
import org.springframework.security.oauth2.client.userinfo.OAuth2UserService;
import org.springframework.security.oauth2.client.web.OAuth2LoginAuthenticationFilter;
import org.springframework.security.oauth2.core.AuthorizationGrantType;
import org.springframework.security.oauth2.core.user.DefaultOAuth2User;
import org.springframework.security.oauth2.core.user.OAuth2User;
import org.springframework.security.web.SecurityFilterChain;
import org.springframework.test.context.bean.override.mockito.MockitoBean;
import org.springframework.test.util.ReflectionTestUtils;
import org.springframework.test.web.servlet.MockMvc;
import org.springframework.test.web.servlet.setup.MockMvcBuilders;
import org.springframework.web.context.WebApplicationContext;
import org.springframework.web.filter.ForwardedHeaderFilter;

@WebMvcTest(AuthController.class)
@Import({
  AuthService.class,
  Release0SecurityConfig.class,
  Release0SecurityConfigOAuthTests.OAuthTestConfig.class
})
class Release0SecurityConfigOAuthTests {

  @Autowired private MockMvc mockMvc;
  @Autowired private WebApplicationContext webApplicationContext;
  @Autowired
  @Qualifier("springSecurityFilterChain")
  private Filter springSecurityFilterChain;
  @Autowired private SecurityFilterChain securityFilterChain;
  @Autowired private Release0OAuth2UserService release0OAuth2UserService;
  @Autowired private Release0OidcUserService release0OidcUserService;
  private MockMvc forwardedHeaderMockMvc;

  @MockitoBean private CurrentAuthAccountProvider currentAuthAccountProvider;
  @MockitoBean private InternalTokenVerifier internalTokenVerifier;
  @MockitoBean private PlayerAccountRepository playerAccountRepository;

  @BeforeEach
  void setUpForwardedHeaderMockMvc() {
    forwardedHeaderMockMvc =
        MockMvcBuilders.webAppContextSetup(webApplicationContext)
            .addFilters(new ForwardedHeaderFilter(), springSecurityFilterChain)
            .build();
  }

  @Test
  void googleAuthorizationEndpointStartsOAuthRedirectWhenClientRegistrationExists()
      throws Exception {
    mockMvc
        .perform(get("/oauth2/authorization/google"))
        .andExpect(status().is3xxRedirection())
        .andExpect(header().string("Location", containsString("accounts.google.com")));
  }

  @Test
  void googleAuthorizationEndpointUsesForwardedHttpsOriginForRedirectUri() throws Exception {
    forwardedHeaderMockMvc
        .perform(
            get("/oauth2/authorization/google")
                .header("X-Forwarded-Proto", "https")
                .header("X-Forwarded-Host", "release0.example.com"))
        .andExpect(status().is3xxRedirection())
        .andExpect(
            header()
                .string(
                    "Location",
                    containsString(
                        "redirect_uri=https://release0.example.com/login/oauth2/code/google")));
  }

  @Test
  void defaultLoginPageIsNotExposedAsNewPublicRoute() throws Exception {
    mockMvc.perform(get("/login")).andExpect(status().isForbidden());
  }

  @Test
  void unknownRouteRemainsDeniedWhenOauthIsEnabled() throws Exception {
    mockMvc.perform(get("/api/release0/unknown")).andExpect(status().isForbidden());
  }

  @Test
  void oauthLoginFilterUsesRelease0UserServices() {
    OAuth2LoginAuthenticationFilter filter =
        securityFilterChain.getFilters().stream()
            .filter(OAuth2LoginAuthenticationFilter.class::isInstance)
            .map(OAuth2LoginAuthenticationFilter.class::cast)
            .findFirst()
            .orElseThrow();
    Object authenticationManager = ReflectionTestUtils.getField(filter, "authenticationManager");
    ProviderManager providerManager = (ProviderManager) authenticationManager;

    OAuth2LoginAuthenticationProvider oauthProvider =
        findProvider(providerManager, OAuth2LoginAuthenticationProvider.class);
    OidcAuthorizationCodeAuthenticationProvider oidcProvider =
        findProvider(providerManager, OidcAuthorizationCodeAuthenticationProvider.class);

    Object configuredOauthService = ReflectionTestUtils.getField(oauthProvider, "userService");
    Object configuredOidcService = ReflectionTestUtils.getField(oidcProvider, "userService");
    assertThat(configuredOauthService).isSameAs(release0OAuth2UserService);
    assertThat(configuredOidcService).isSameAs(release0OidcUserService);
  }

  @Test
  void oauthLoginSuccessTargetsStandaloneCompletionFallback() {
    OAuth2LoginAuthenticationFilter filter =
        securityFilterChain.getFilters().stream()
            .filter(OAuth2LoginAuthenticationFilter.class::isInstance)
            .map(OAuth2LoginAuthenticationFilter.class::cast)
            .findFirst()
            .orElseThrow();
    Object successHandler = ReflectionTestUtils.getField(filter, "successHandler");

    assertThat(ReflectionTestUtils.getField(successHandler, "defaultTargetUrl"))
        .isEqualTo("/api/release0/auth/standalone/complete");
    assertThat(ReflectionTestUtils.getField(successHandler, "alwaysUseDefaultTargetUrl"))
        .isEqualTo(true);
  }

  private static <T extends AuthenticationProvider> T findProvider(
      ProviderManager providerManager, Class<T> providerType) {
    return providerManager.getProviders().stream()
        .filter(providerType::isInstance)
        .map(providerType::cast)
        .findFirst()
        .orElseThrow();
  }

  @TestConfiguration
  static class OAuthTestConfig {

    @Bean
    ClientRegistrationRepository clientRegistrationRepository() {
      return new InMemoryClientRegistrationRepository(googleRegistration());
    }

    @Bean
    Release0OAuth2UserService release0OAuth2UserService() {
      OAuthAccountResolver resolver = (provider, subject) -> new PlayerAccount(1L, null);
      OAuth2UserService<OAuth2UserRequest, OAuth2User> delegate =
          request ->
              new DefaultOAuth2User(
                  List.of(new SimpleGrantedAuthority("ROLE_USER")),
                  Map.of("sub", "google-subject-1"),
                  "sub");
      return new Release0OAuth2UserService(resolver, delegate);
    }

    @Bean
    Release0OidcUserService release0OidcUserService() {
      OAuthAccountResolver resolver = (provider, subject) -> new PlayerAccount(1L, null);
      return new Release0OidcUserService(resolver);
    }

    private static ClientRegistration googleRegistration() {
      return ClientRegistration.withRegistrationId("google")
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
    }
  }
}
