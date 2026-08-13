package com.fasd92.lootoflegends.meta.platform.http;

import static org.mockito.ArgumentMatchers.any;
import static org.mockito.Mockito.reset;
import static org.mockito.Mockito.when;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.header;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.fasd92.lootoflegends.meta.app.MetaServerApplication;
import com.fasd92.lootoflegends.meta.assets.application.AssetStore;
import com.fasd92.lootoflegends.meta.identity.api.DesktopAuthUseCase;
import com.fasd92.lootoflegends.meta.identity.api.IssuedMetaSession;
import com.fasd92.lootoflegends.meta.identity.api.StartDesktopAuthResult;
import com.fasd92.lootoflegends.meta.settlement.application.SettlementInbox;
import java.net.URI;
import java.time.Instant;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.AutoConfigureMockMvc;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.http.MediaType;
import org.springframework.test.context.bean.override.mockito.MockitoBean;
import org.springframework.test.web.servlet.MockMvc;

@SpringBootTest(
    classes = MetaServerApplication.class,
    properties = {
      "loot.internal.service-token=fixture-service-value",
      "loot.settlement.apply-enabled=false",
      "spring.autoconfigure.exclude=org.springframework.boot.autoconfigure.jdbc.DataSourceAutoConfiguration,org.springframework.boot.autoconfigure.flyway.FlywayAutoConfiguration"
    })
@AutoConfigureMockMvc
class DesktopAuthHttpTest {
  private static final String STATE = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  private static final String CHALLENGE = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
  private static final String HANDOFF = "ccccccccccccccccccccccccccccccccccccccccccc";
  private static final String SESSION = "ddddddddddddddddddddddddddddddddddddddddddd";

  @Autowired private MockMvc mvc;

  @MockitoBean private DesktopAuthUseCase useCase;

  @MockitoBean private SettlementInbox settlementInbox;

  @MockitoBean private AssetStore assetStore;

  @Test
  void unauthenticatedDesktopStartAndExchangeFollowFrozenContract() throws Exception {
    when(useCase.start(any()))
        .thenReturn(
            new StartDesktopAuthResult(
                URI.create("https://accounts.google.com/o/oauth2/v2/auth?state=provider-state"),
                Instant.parse("2026-08-10T00:02:00Z")));
    when(useCase.exchange(any()))
        .thenReturn(new IssuedMetaSession(SESSION, Instant.parse("2026-08-10T01:00:00Z")));

    mvc.perform(
            post("/api/v1/desktop-auth/attempts")
                .contentType(MediaType.APPLICATION_JSON)
                .content(
                    """
                    {"loopbackRedirectUri":"http://127.0.0.1:49152/callback","state":"%s","codeChallenge":"%s"}
                    """
                        .formatted(STATE, CHALLENGE)))
        .andExpect(status().isCreated())
        .andExpect(
            jsonPath("$.authorizationUrl")
                .value("https://accounts.google.com/o/oauth2/v2/auth?state=provider-state"))
        .andExpect(jsonPath("$.expiresAt").value("2026-08-10T00:02:00Z"))
        .andExpect(jsonPath("$.accessToken").doesNotExist())
        .andExpect(jsonPath("$.idToken").doesNotExist());

    mvc.perform(
            post("/api/v1/desktop-auth/exchanges")
                .contentType(MediaType.APPLICATION_JSON)
                .content(
                    """
                    {"handoffCode":"%s","state":"%s","codeVerifier":"%s"}
                    """
                        .formatted(HANDOFF, STATE, CHALLENGE)))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.metaSession").value(SESSION))
        .andExpect(jsonPath("$.expiresAt").value("2026-08-10T01:00:00Z"))
        .andExpect(jsonPath("$.accessToken").doesNotExist())
        .andExpect(jsonPath("$.idToken").doesNotExist());
  }

  @Test
  void providerCallbackRedirectsOnlyToUseCaseValidatedLoopback() throws Exception {
    when(useCase.completeProviderCallback(any()))
        .thenReturn(
            URI.create("http://127.0.0.1:49152/callback?code=" + HANDOFF + "&state=" + STATE));

    mvc.perform(
            get("/v1/identity/google/callback")
                .queryParam("code", "provider-code")
                .queryParam("state", CHALLENGE))
        .andExpect(status().isFound())
        .andExpect(
            header()
                .string(
                    "Location",
                    "http://127.0.0.1:49152/callback?code=" + HANDOFF + "&state=" + STATE));
  }

  @Test
  void mismatchAndExpiredOutcomesHaveStableCodes() throws Exception {
    when(useCase.exchange(any()))
        .thenThrow(
            new DesktopAuthUseCase.Rejected(
                DesktopAuthUseCase.RejectionReason.STATE_OR_PKCE_MISMATCH));

    mvc.perform(
            post("/api/v1/desktop-auth/exchanges")
                .contentType(MediaType.APPLICATION_JSON)
                .content(
                    """
                    {"handoffCode":"%s","state":"%s","codeVerifier":"%s"}
                    """
                        .formatted(HANDOFF, STATE, CHALLENGE)))
        .andExpect(status().isUnprocessableEntity())
        .andExpect(jsonPath("$.code").value("STATE_OR_PKCE_MISMATCH"));

    reset(useCase);
    when(useCase.exchange(any()))
        .thenThrow(
            new DesktopAuthUseCase.Rejected(DesktopAuthUseCase.RejectionReason.EXPIRED_OR_USED));

    mvc.perform(
            post("/api/v1/desktop-auth/exchanges")
                .contentType(MediaType.APPLICATION_JSON)
                .content(
                    """
                    {"handoffCode":"%s","state":"%s","codeVerifier":"%s"}
                    """
                        .formatted(HANDOFF, STATE, CHALLENGE)))
        .andExpect(status().isGone())
        .andExpect(jsonPath("$.code").value("EXPIRED"));
  }

  @Test
  void privateEvidenceAndMetricsRoutesAreAbsentByDefault() throws Exception {
    mvc.perform(
            post("/private/evidence/v1/meta-sessions")
                .contentType(MediaType.APPLICATION_JSON)
                .content("{\"runId\":\"run-a\",\"participantIndex\":1}"))
        .andExpect(status().isNotFound());
    mvc.perform(get("/private/metrics/v1")).andExpect(status().isNotFound());
    mvc.perform(get("/dev-auth")).andExpect(status().isNotFound());
  }
}
