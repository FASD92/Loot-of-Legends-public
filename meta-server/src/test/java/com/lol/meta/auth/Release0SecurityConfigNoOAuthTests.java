package com.lol.meta.auth;

import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.lol.meta.internal.InternalTokenVerifier;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.WebMvcTest;
import org.springframework.context.annotation.Import;
import org.springframework.test.context.bean.override.mockito.MockitoBean;
import org.springframework.test.web.servlet.MockMvc;

@WebMvcTest(AuthController.class)
@Import({AuthService.class, Release0SecurityConfig.class})
class Release0SecurityConfigNoOAuthTests {

  @Autowired private MockMvc mockMvc;

  @MockitoBean private CurrentAuthAccountProvider currentAuthAccountProvider;
  @MockitoBean private InternalTokenVerifier internalTokenVerifier;
  @MockitoBean private PlayerAccountRepository playerAccountRepository;

  @Test
  void oauthAuthorizationEndpointIsDeniedWhenClientRegistrationDoesNotExist() throws Exception {
    mockMvc.perform(get("/oauth2/authorization/google")).andExpect(status().isForbidden());
  }
}
