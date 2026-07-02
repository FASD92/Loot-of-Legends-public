package com.lol.meta.session;

import static org.mockito.Mockito.verify;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.lol.meta.internal.InternalTokenInterceptor;
import com.lol.meta.internal.InternalTokenVerifier;
import com.lol.meta.internal.InternalWebMvcConfig;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.WebMvcTest;
import org.springframework.context.annotation.Import;
import org.springframework.http.MediaType;
import org.springframework.test.context.TestPropertySource;
import org.springframework.test.context.bean.override.mockito.MockitoBean;
import org.springframework.test.web.servlet.MockMvc;

@WebMvcTest(GameSessionTokenController.class)
@Import({InternalWebMvcConfig.class, InternalTokenInterceptor.class, InternalTokenVerifier.class})
@TestPropertySource(properties = "meta.internal.token=test-internal-token")
class GameSessionTokenControllerTests {

  private static final String INTERNAL_TOKEN = "test-internal-token";

  private final MockMvc mockMvc;

  @MockitoBean private GameSessionTokenService gameSessionTokenService;

  @Autowired
  GameSessionTokenControllerTests(MockMvc mockMvc) {
    this.mockMvc = mockMvc;
  }

  @Test
  void renewAcceptsAccountConnectionBodyWithoutToken() throws Exception {
    mockMvc
        .perform(
            post("/internal/release0/game-session-tokens/renew")
                .header(InternalTokenInterceptor.HEADER_NAME, INTERNAL_TOKEN)
                .contentType(MediaType.APPLICATION_JSON)
                .content("{\"accountId\":123,\"connectionId\":\"client-1\"}"))
        .andExpect(status().isNoContent());

    verify(gameSessionTokenService).renew(new GameSessionRenewRequest(123L, "client-1"));
  }
}
