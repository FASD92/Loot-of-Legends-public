package com.lol.meta.auth;

import static org.assertj.core.api.Assertions.assertThat;

import java.util.List;
import java.util.Map;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;
import org.springframework.security.authentication.TestingAuthenticationToken;
import org.springframework.security.core.authority.SimpleGrantedAuthority;
import org.springframework.security.core.context.SecurityContextHolder;
import org.springframework.security.oauth2.client.authentication.OAuth2AuthenticationToken;
import org.springframework.security.oauth2.core.user.DefaultOAuth2User;
import org.springframework.security.oauth2.core.user.OAuth2User;

class SecurityContextCurrentAuthAccountProviderTests {

  @AfterEach
  void clearSecurityContext() {
    SecurityContextHolder.clearContext();
  }

  @Test
  void readsRelease0AccountIdFromRelease0Principal() {
    OAuth2User principal =
        new Release0AuthenticatedOAuth2User(
            new DefaultOAuth2User(
                List.of(new SimpleGrantedAuthority("ROLE_USER")),
                Map.of("sub", "google-subject-1"),
                "sub"),
            123L);
    SecurityContextHolder.getContext()
        .setAuthentication(
            new OAuth2AuthenticationToken(principal, principal.getAuthorities(), "google"));

    assertThat(new SecurityContextCurrentAuthAccountProvider().currentAccountId()).contains(123L);
  }

  @Test
  void ignoresPlainOAuthPrincipalEvenWhenAttributeLooksLikeRelease0AccountId() {
    OAuth2User principal =
        new DefaultOAuth2User(
            List.of(new SimpleGrantedAuthority("ROLE_USER")),
            Map.of("sub", "google-subject-1", "release0AccountId", 123L),
            "sub");
    SecurityContextHolder.getContext()
        .setAuthentication(
            new OAuth2AuthenticationToken(principal, principal.getAuthorities(), "google"));

    assertThat(new SecurityContextCurrentAuthAccountProvider().currentAccountId()).isEmpty();
  }

  @Test
  void returnsEmptyWhenOAuthPrincipalDoesNotCarryRelease0AccountId() {
    OAuth2User principal =
        new DefaultOAuth2User(
            List.of(new SimpleGrantedAuthority("ROLE_USER")),
            Map.of("sub", "google-subject-1"),
            "sub");
    SecurityContextHolder.getContext()
        .setAuthentication(
            new OAuth2AuthenticationToken(principal, principal.getAuthorities(), "google"));

    assertThat(new SecurityContextCurrentAuthAccountProvider().currentAccountId()).isEmpty();
  }

  @Test
  void returnsEmptyForNonOAuthAuthentication() {
    SecurityContextHolder.getContext()
        .setAuthentication(new TestingAuthenticationToken("user", "password"));

    assertThat(new SecurityContextCurrentAuthAccountProvider().currentAccountId()).isEmpty();
  }
}
