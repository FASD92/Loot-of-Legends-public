package com.fasd92.lootoflegends.meta.platform.http;

import static org.mockito.ArgumentMatchers.any;
import static org.mockito.Mockito.verifyNoInteractions;
import static org.mockito.Mockito.when;
import static org.springframework.security.test.web.servlet.request.SecurityMockMvcRequestPostProcessors.authentication;
import static org.springframework.security.test.web.servlet.request.SecurityMockMvcRequestPostProcessors.csrf;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.fasd92.lootoflegends.meta.app.MetaServerApplication;
import com.fasd92.lootoflegends.meta.assets.application.AssetStore;
import com.fasd92.lootoflegends.meta.gameaccess.api.ClaimGameCredentialResult;
import com.fasd92.lootoflegends.meta.gameaccess.api.GameCredentialUseCase;
import com.fasd92.lootoflegends.meta.gameaccess.api.IssuedGameCredential;
import com.fasd92.lootoflegends.meta.identity.api.AccountId;
import com.fasd92.lootoflegends.meta.identity.api.AuthenticatedPrincipal;
import com.fasd92.lootoflegends.meta.identity.api.DesktopAuthUseCase;
import com.fasd92.lootoflegends.meta.settlement.application.SettlementInbox;
import java.time.Instant;
import java.util.List;
import java.util.Optional;
import java.util.UUID;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.AutoConfigureMockMvc;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.http.MediaType;
import org.springframework.security.authentication.UsernamePasswordAuthenticationToken;
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
class GameCredentialHttpTest {
  private static final AccountId ACCOUNT_ID =
      new AccountId(UUID.fromString("00000000-0000-4000-8000-000000000001"));

  @Autowired private MockMvc mvc;

  @MockitoBean private GameCredentialUseCase useCase;

  @MockitoBean private DesktopAuthUseCase desktopAuth;

  @MockitoBean private SettlementInbox settlementInbox;

  @MockitoBean private AssetStore assetStore;

  @Test
  void internalClaimRejectsMissingServiceAuthentication() throws Exception {
    mvc.perform(
            post("/internal/v1/game-credentials/claim")
                .contentType(MediaType.APPLICATION_JSON)
                .content(
                    """
                    {"credential":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","audience":"loot-game-server-v1"}
                    """))
        .andExpect(status().isUnauthorized());

    verifyNoInteractions(useCase);
  }

  @Test
  void authenticatedPrincipalCanIssueCredential() throws Exception {
    when(useCase.issue(any()))
        .thenReturn(
            new IssuedGameCredential(
                "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                Instant.parse("2026-08-08T00:00:30Z")));

    var principal = new AuthenticatedPrincipal(ACCOUNT_ID, "player-one");
    var authentication =
        UsernamePasswordAuthenticationToken.authenticated(principal, "fixture", List.of());

    mvc.perform(post("/api/v1/game-credentials").with(authentication(authentication)).with(csrf()))
        .andExpect(status().isCreated())
        .andExpect(jsonPath("$.credential").value("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"))
        .andExpect(jsonPath("$.expiresAt").value("2026-08-08T00:00:30Z"));
  }

  @Test
  void metaSessionBearerAuthenticatesPublicApiWithoutCsrfToken() throws Exception {
    var principal = new AuthenticatedPrincipal(ACCOUNT_ID, "player-one");
    when(desktopAuth.authenticate("MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM"))
        .thenReturn(Optional.of(principal));
    when(useCase.issue(any()))
        .thenReturn(
            new IssuedGameCredential(
                "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA",
                Instant.parse("2026-08-08T00:00:30Z")));

    mvc.perform(
            post("/api/v1/game-credentials")
                .header("Authorization", "Bearer MMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMMM"))
        .andExpect(status().isCreated());
  }

  @Test
  void publicCredentialRejectsMissingOrInvalidMetaSessionBearer() throws Exception {
    when(desktopAuth.authenticate("wrong-meta-session")).thenReturn(Optional.empty());

    mvc.perform(post("/api/v1/game-credentials")).andExpect(status().isUnauthorized());
    mvc.perform(
            post("/api/v1/game-credentials").header("Authorization", "Bearer wrong-meta-session"))
        .andExpect(status().isUnauthorized());

    verifyNoInteractions(useCase);
  }

  @Test
  void authenticatedGameServerCanClaimCredential() throws Exception {
    when(useCase.claim(any()))
        .thenReturn(new ClaimGameCredentialResult.Claimed(ACCOUNT_ID, "player-one"));

    mvc.perform(
            post("/internal/v1/game-credentials/claim")
                .header("Authorization", "Bearer fixture-service-value")
                .contentType(MediaType.APPLICATION_JSON)
                .content(
                    """
                    {"credential":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","audience":"loot-game-server-v1"}
                    """))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.accountId").value(ACCOUNT_ID.value().toString()))
        .andExpect(jsonPath("$.nickname").value("player-one"));
  }

  @Test
  void claimRejectsUnknownJsonFields() throws Exception {
    mvc.perform(
            post("/internal/v1/game-credentials/claim")
                .header("Authorization", "Bearer fixture-service-value")
                .contentType(MediaType.APPLICATION_JSON)
                .content(
                    """
                    {"credential":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA","audience":"loot-game-server-v1","unexpected":true}
                    """))
        .andExpect(status().isBadRequest())
        .andExpect(jsonPath("$.code").value("INVALID"));
  }
}
