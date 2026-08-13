package com.fasd92.lootoflegends.meta.platform.http;

import static org.mockito.ArgumentMatchers.any;
import static org.mockito.Mockito.when;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.header;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.fasd92.lootoflegends.meta.app.MetaServerApplication;
import com.fasd92.lootoflegends.meta.assets.application.AssetStore;
import com.fasd92.lootoflegends.meta.gameaccess.api.GameCredentialUseCase;
import com.fasd92.lootoflegends.meta.identity.api.DesktopAuthUseCase;
import com.fasd92.lootoflegends.meta.identity.api.EvidenceAdmissionUseCase;
import com.fasd92.lootoflegends.meta.identity.api.IssuedMetaSession;
import com.fasd92.lootoflegends.meta.settlement.application.SettlementInbox;
import java.time.Instant;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.AutoConfigureMockMvc;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.http.HttpHeaders;
import org.springframework.http.MediaType;
import org.springframework.test.context.bean.override.mockito.MockitoBean;
import org.springframework.test.web.servlet.MockMvc;

@SpringBootTest(
    classes = MetaServerApplication.class,
    properties = {
      "loot.internal.service-token=fixture-service-value",
      "loot.settlement.apply-enabled=false",
      "loot.evidence.enabled=true",
      "loot.evidence.admission-token=fixture-evidence-value",
      "loot.evidence.participant-maximum=20",
      "loot.evidence.session-ttl=PT10M",
      "loot.evidence.environment-expires-at=2099-01-01T00:00:00Z",
      "spring.autoconfigure.exclude=org.springframework.boot.autoconfigure.jdbc.DataSourceAutoConfiguration,org.springframework.boot.autoconfigure.flyway.FlywayAutoConfiguration"
    })
@AutoConfigureMockMvc
class EvidenceAdmissionHttpTest {
  private static final String PATH = "/private/evidence/v1/meta-sessions";

  @Autowired private MockMvc mvc;

  @MockitoBean private EvidenceAdmissionUseCase evidenceAdmission;
  @MockitoBean private DesktopAuthUseCase desktopAuth;
  @MockitoBean private GameCredentialUseCase gameAccess;
  @MockitoBean private SettlementInbox settlementInbox;
  @MockitoBean private AssetStore assetStore;

  @Test
  void dedicatedBearerIssuesOnlyAMetaSessionAndDisablesCaching() throws Exception {
    when(evidenceAdmission.issueEvidenceSession(any()))
        .thenReturn(
            new IssuedMetaSession(
                "ddddddddddddddddddddddddddddddddddddddddddd",
                Instant.parse("2026-08-11T00:10:00Z")));

    mvc.perform(
            post(PATH)
                .header(HttpHeaders.AUTHORIZATION, "Bearer fixture-evidence-value")
                .contentType(MediaType.APPLICATION_JSON)
                .content("{\"runId\":\"run-a\",\"participantIndex\":1}"))
        .andExpect(status().isCreated())
        .andExpect(header().string(HttpHeaders.CACHE_CONTROL, "no-store"))
        .andExpect(jsonPath("$.metaSession").value("ddddddddddddddddddddddddddddddddddddddddddd"))
        .andExpect(jsonPath("$.expiresAt").value("2026-08-11T00:10:00Z"))
        .andExpect(jsonPath("$.accountId").doesNotExist())
        .andExpect(jsonPath("$.role").doesNotExist());
  }

  @Test
  void missingWrongOrMalformedAuthorizationFailsClosed() throws Exception {
    for (String authorization :
        new String[] {
          null, "Bearer wrong-evidence-value", "Basic fixture-evidence-value", "Bearer "
        }) {
      var request =
          post(PATH)
              .contentType(MediaType.APPLICATION_JSON)
              .content("{\"runId\":\"run-a\",\"participantIndex\":1}");
      if (authorization != null) {
        request.header(HttpHeaders.AUTHORIZATION, authorization);
      }
      mvc.perform(request).andExpect(status().isUnauthorized());
    }
  }

  @Test
  void boundedRequestRejectsUnknownPrivilegeAndParticipantOverflow() throws Exception {
    for (String body :
        new String[] {
          "{\"runId\":\"bad run\",\"participantIndex\":1}",
          "{\"runId\":\"run-a\",\"participantIndex\":0}",
          "{\"runId\":\"run-a\",\"participantIndex\":21}",
          "{\"runId\":\"run-a\",\"participantIndex\":1,\"accountId\":\"x\"}",
          "{\"runId\":\"run-a\",\"participantIndex\":1,\"role\":\"admin\"}"
        }) {
      mvc.perform(
              post(PATH)
                  .header(HttpHeaders.AUTHORIZATION, "Bearer fixture-evidence-value")
                  .contentType(MediaType.APPLICATION_JSON)
                  .content(body))
          .andExpect(status().isBadRequest());
    }
  }

  @Test
  void explicitReplayRejectionIsStable() throws Exception {
    when(evidenceAdmission.issueEvidenceSession(any()))
        .thenThrow(
            new EvidenceAdmissionUseCase.Rejected(
                EvidenceAdmissionUseCase.RejectionReason.REPLAYED));

    mvc.perform(
            post(PATH)
                .header(HttpHeaders.AUTHORIZATION, "Bearer fixture-evidence-value")
                .contentType(MediaType.APPLICATION_JSON)
                .content("{\"runId\":\"run-a\",\"participantIndex\":1}"))
        .andExpect(status().isConflict())
        .andExpect(jsonPath("$.code").value("REPLAYED"));
  }
}
