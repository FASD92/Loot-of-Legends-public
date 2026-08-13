package com.fasd92.lootoflegends.meta.platform.http;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.header;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.fasd92.lootoflegends.meta.app.MetaServerApplication;
import com.fasd92.lootoflegends.meta.assets.application.AssetStore;
import com.fasd92.lootoflegends.meta.identity.api.DesktopAuthUseCase;
import com.fasd92.lootoflegends.meta.platform.observability.MetaMetrics;
import com.fasd92.lootoflegends.meta.settlement.application.SettlementInbox;
import java.time.Duration;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.AutoConfigureMockMvc;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.http.HttpHeaders;
import org.springframework.test.context.bean.override.mockito.MockitoBean;
import org.springframework.test.web.servlet.MockMvc;

@SpringBootTest(
    classes = MetaServerApplication.class,
    properties = {
      "loot.internal.service-token=fixture-service-value",
      "loot.settlement.apply-enabled=false",
      "loot.metrics.enabled=true",
      "loot.metrics.read-token=fixture-metrics-value",
      "loot.metrics.source-identity-digest=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "spring.autoconfigure.exclude=org.springframework.boot.autoconfigure.jdbc.DataSourceAutoConfiguration,org.springframework.boot.autoconfigure.flyway.FlywayAutoConfiguration"
    })
@AutoConfigureMockMvc
class PrivateMetricsHttpTest {
  private static final String PATH = "/private/metrics/v1";

  @Autowired private MockMvc mvc;

  @Autowired private MetaMetrics metrics;
  @MockitoBean private DesktopAuthUseCase desktopAuth;
  @MockitoBean private SettlementInbox settlementInbox;
  @MockitoBean private AssetStore assetStore;

  @Test
  void authenticatedGetReturnsFixedLabelFreeSchemaAndNoStore() throws Exception {
    metrics.recordApplied(Duration.ofMillis(17));
    metrics.recordRetry();
    metrics.recordConflict();

    var response =
        mvc.perform(get(PATH).header(HttpHeaders.AUTHORIZATION, "Bearer fixture-metrics-value"))
            .andExpect(status().isOk())
            .andExpect(header().string(HttpHeaders.CACHE_CONTROL, "no-store"))
            .andExpect(jsonPath("$.schemaVersion").value(1))
            .andExpect(jsonPath("$.source").value("meta"))
            .andExpect(
                jsonPath("$.sourceIdentityDigest")
                    .value("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"))
            .andExpect(jsonPath("$.capturedAtUnixMillis").isNumber())
            .andExpect(jsonPath("$.metrics[0].name").value("settlement_apply_total"))
            .andExpect(jsonPath("$.metrics[0].value").value(1))
            .andExpect(jsonPath("$.metrics[1].name").value("settlement_retry_total"))
            .andExpect(jsonPath("$.metrics[2].name").value("settlement_conflict_total"))
            .andExpect(jsonPath("$.metrics[3].name").value("settlement_apply_latency_ms"))
            .andExpect(jsonPath("$.metrics[0].labels").doesNotExist())
            .andReturn()
            .getResponse();

    byte[] body = response.getContentAsByteArray();
    assertTrue(body.length <= 64 * 1024);
    String json = response.getContentAsString();
    for (String forbidden :
        new String[] {
          "accountId", "sessionId", "roomId", "battleInstanceId", "nickname", "token"
        }) {
      assertFalse(json.contains(forbidden));
    }
  }

  @Test
  void wrongMissingTokenUnknownVersionAndMutationFailClosed() throws Exception {
    mvc.perform(get(PATH)).andExpect(status().isUnauthorized());
    mvc.perform(get(PATH).header(HttpHeaders.AUTHORIZATION, "Bearer wrong-metrics-value"))
        .andExpect(status().isUnauthorized());
    mvc.perform(
            get("/private/metrics/v2")
                .header(HttpHeaders.AUTHORIZATION, "Bearer fixture-metrics-value"))
        .andExpect(status().isNotFound());
    mvc.perform(post(PATH).header(HttpHeaders.AUTHORIZATION, "Bearer fixture-metrics-value"))
        .andExpect(status().isMethodNotAllowed());
  }
}
