package com.lol.meta.auth;

import static org.assertj.core.api.Assertions.assertThat;
import static org.hamcrest.Matchers.containsString;
import static org.hamcrest.Matchers.is;
import static org.hamcrest.Matchers.notNullValue;
import static org.mockito.Mockito.when;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.header;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.request;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.lol.meta.internal.InternalTokenVerifier;
import java.net.URI;
import java.util.List;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.WebMvcTest;
import org.springframework.context.annotation.Import;
import org.springframework.http.MediaType;
import org.springframework.mock.web.MockHttpSession;
import org.springframework.security.authentication.UsernamePasswordAuthenticationToken;
import org.springframework.security.core.authority.SimpleGrantedAuthority;
import org.springframework.security.core.context.SecurityContext;
import org.springframework.security.core.context.SecurityContextHolder;
import org.springframework.security.web.context.HttpSessionSecurityContextRepository;
import org.springframework.test.context.bean.override.mockito.MockitoBean;
import org.springframework.test.web.servlet.MockMvc;
import org.springframework.test.web.servlet.MvcResult;

@WebMvcTest(StandaloneOAuthHandoffController.class)
@Import({
  AuthService.class,
  Release0SecurityConfig.class,
  SecurityContextCurrentAuthAccountProvider.class,
  StandaloneOAuthHandoffService.class
})
class StandaloneOAuthHandoffControllerTests {

  private static final String LOOPBACK_CALLBACK =
      "http://127.0.0.1:53421/release0/oauth/callback";

  private final MockMvc mockMvc;
  private final StandaloneOAuthHandoffService handoffService;

  @MockitoBean private InternalTokenVerifier internalTokenVerifier;
  @MockitoBean private PlayerAccountRepository playerAccountRepository;

  @Autowired
  StandaloneOAuthHandoffControllerTests(
      MockMvc mockMvc, StandaloneOAuthHandoffService handoffService) {
    this.mockMvc = mockMvc;
    this.handoffService = handoffService;
  }

  @Test
  void startStoresLoopbackCallbackAndRedirectsToGoogleOAuth() throws Exception {
    mockMvc
        .perform(
            get("/api/release0/auth/standalone/start")
                .param("callback", LOOPBACK_CALLBACK)
                .param("state", "state-1"))
        .andExpect(status().is3xxRedirection())
        .andExpect(header().string("Location", containsString("/oauth2/authorization/google")))
        .andExpect(
            request()
                .sessionAttribute(
                    StandaloneOAuthHandoffController.CALLBACK_SESSION_ATTRIBUTE,
                    LOOPBACK_CALLBACK))
        .andExpect(
            request()
                .sessionAttribute(
                    StandaloneOAuthHandoffController.STATE_SESSION_ATTRIBUTE, "state-1"));
  }

  @Test
  void startRejectsNonLoopbackCallback() throws Exception {
    mockMvc
        .perform(
            get("/api/release0/auth/standalone/start")
                .param("callback", "https://example.com/callback")
                .param("state", "state-1"))
        .andExpect(status().isBadRequest());
  }

  @Test
  void startRejectsInvalidState() throws Exception {
    mockMvc
        .perform(
            get("/api/release0/auth/standalone/start")
                .param("callback", LOOPBACK_CALLBACK)
                .param("state", "bad state"))
        .andExpect(status().isBadRequest());

    mockMvc
        .perform(
            get("/api/release0/auth/standalone/start")
                .param("callback", LOOPBACK_CALLBACK)
                .param("state", "a".repeat(129)))
        .andExpect(status().isBadRequest());
  }

  @Test
  void completeRedirectsPendingLoopbackAfterOAuthLogin() throws Exception {
    MvcResult startResult =
        mockMvc
            .perform(
                get("/api/release0/auth/standalone/start")
                    .param("callback", LOOPBACK_CALLBACK)
                    .param("state", "state-1"))
            .andExpect(status().is3xxRedirection())
            .andReturn();
    MockHttpSession session = (MockHttpSession) startResult.getRequest().getSession(false);
    session.setAttribute(
        HttpSessionSecurityContextRepository.SPRING_SECURITY_CONTEXT_KEY,
        securityContextFor(123L));

    MvcResult completeResult =
        mockMvc
            .perform(get("/api/release0/auth/standalone/complete").session(session))
            .andExpect(status().is3xxRedirection())
            .andExpect(header().string("Location", containsString(LOOPBACK_CALLBACK)))
            .andExpect(header().string("Location", containsString("state=state-1")))
            .andReturn();

    URI redirect = URI.create(completeResult.getResponse().getHeader("Location"));
    String code = queryParam(redirect, "code");

    assertThat(handoffService.claimCode(code, "state-1").accountId()).isEqualTo(123L);
  }

  @Test
  void completeWithoutPendingCallbackPreservesBrowserSessionSmoke() throws Exception {
    mockMvc
        .perform(get("/api/release0/auth/standalone/complete"))
        .andExpect(status().is3xxRedirection())
        .andExpect(header().string("Location", "/api/release0/auth/session"));
  }

  @Test
  void exchangeCreatesAuthenticatedHttpSession() throws Exception {
    when(playerAccountRepository.findAccount(123L))
        .thenReturn(Optional.of(new PlayerAccount(123L, "Player123")));
    String code = handoffService.issueCode(123L, "state-1");

    mockMvc
        .perform(
            post("/api/release0/auth/standalone/exchange")
                .contentType(MediaType.APPLICATION_JSON)
                .content(
                    """
                    { "code": "%s", "state": "state-1" }
                    """
                        .formatted(code)))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.authenticated", is(true)))
        .andExpect(jsonPath("$.accountId", is(123)))
        .andExpect(jsonPath("$.nicknameRequired", is(false)))
        .andExpect(jsonPath("$.nickname", is("Player123")))
        .andExpect(
            request()
                .sessionAttribute(
                    HttpSessionSecurityContextRepository.SPRING_SECURITY_CONTEXT_KEY,
                    notNullValue()));
  }

  @Test
  void exchangeRotatesExistingHttpSessionId() throws Exception {
    when(playerAccountRepository.findAccount(123L))
        .thenReturn(Optional.of(new PlayerAccount(123L, "Player123")));
    String code = handoffService.issueCode(123L, "state-1");
    MockHttpSession session = new MockHttpSession();
    String originalSessionId = session.getId();

    MvcResult exchangeResult =
        mockMvc
            .perform(
                post("/api/release0/auth/standalone/exchange")
                    .session(session)
                    .contentType(MediaType.APPLICATION_JSON)
                    .content(
                        """
                        { "code": "%s", "state": "state-1" }
                        """
                            .formatted(code)))
            .andExpect(status().isOk())
            .andReturn();

    assertThat(exchangeResult.getRequest().getSession(false).getId())
        .isNotEqualTo(originalSessionId);
  }

  private static SecurityContext securityContextFor(long accountId) {
    SecurityContext context = SecurityContextHolder.createEmptyContext();
    TestRelease0Principal principal = new TestRelease0Principal(accountId);
    context.setAuthentication(
        new UsernamePasswordAuthenticationToken(
            principal, "n/a", List.of(new SimpleGrantedAuthority("ROLE_USER"))));
    return context;
  }

  private static String queryParam(URI uri, String name) {
    for (String pair : uri.getRawQuery().split("&")) {
      String[] parts = pair.split("=", 2);
      if (parts.length == 2 && parts[0].equals(name)) {
        return parts[1];
      }
    }
    throw new IllegalArgumentException("missing query param: " + name);
  }

  private record TestRelease0Principal(long release0AccountId)
      implements Release0AuthenticatedPrincipal {}
}
