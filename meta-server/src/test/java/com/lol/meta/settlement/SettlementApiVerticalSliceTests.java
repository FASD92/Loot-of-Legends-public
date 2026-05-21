package com.lol.meta.settlement;

import static org.assertj.core.api.Assertions.assertThat;
import static org.hamcrest.Matchers.is;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.lol.meta.internal.InternalTokenInterceptor;
import com.lol.meta.support.AbstractContainerIntegrationTest;
import java.util.Map;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.AutoConfigureMockMvc;
import org.springframework.data.redis.core.RedisCallback;
import org.springframework.data.redis.core.StringRedisTemplate;
import org.springframework.http.MediaType;
import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.test.web.servlet.MockMvc;
import org.springframework.test.web.servlet.request.MockHttpServletRequestBuilder;

@AutoConfigureMockMvc
class SettlementApiVerticalSliceTests extends AbstractContainerIntegrationTest {

  private static final String INTERNAL_TOKEN = "test-internal-token";
  private static final String SESSION_TOKEN = "test-session-token";
  private static final long ACCOUNT_ID = 77L;
  private static final long ITEM_ID = 30001L;
  private static final String SETTLEMENT_ID = "room-42-session-1001-finished-0001";

  @Autowired private JdbcTemplate jdbcTemplate;
  @Autowired private MockMvc mockMvc;
  @Autowired private SettlementProcessingGuard settlementProcessingGuard;
  @Autowired private StringRedisTemplate redisTemplate;

  @BeforeEach
  void cleanState() {
    jdbcTemplate.update("DELETE FROM settlement_records");
    jdbcTemplate.update("DELETE FROM inventories");
    jdbcTemplate.update("DELETE FROM wallets");
    jdbcTemplate.update("DELETE FROM accounts");
    redisTemplate.execute(
        (RedisCallback<Void>)
            connection -> {
              connection.serverCommands().flushDb();
              return null;
            });
  }

  @Test
  void validRequestAppliesSettlementAcrossHttpRedisAndMySql() throws Exception {
    seedWallet(ACCOUNT_ID, 1_000L);
    putValidSession(SESSION_TOKEN, ACCOUNT_ID);

    mockMvc
        .perform(validSettlementPost(SETTLEMENT_ID, SESSION_TOKEN, 120L, 1))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.settlementId", is(SETTLEMENT_ID)))
        .andExpect(jsonPath("$.status", is("APPLIED")))
        .andExpect(jsonPath("$.duplicate", is(false)));

    assertThat(recordCount(SETTLEMENT_ID)).isEqualTo(1);
    assertThat(inventoryQuantity(ACCOUNT_ID, ITEM_ID)).isEqualTo(1);
    assertThat(walletGold(ACCOUNT_ID)).isEqualTo(1_120L);
    assertThat(processingGuardValue(SETTLEMENT_ID)).isNull();
  }

  @Test
  void duplicateRequestDoesNotApplyAssetsAgain() throws Exception {
    seedWallet(ACCOUNT_ID, 1_000L);
    putValidSession(SESSION_TOKEN, ACCOUNT_ID);

    mockMvc.perform(validSettlementPost(SETTLEMENT_ID, SESSION_TOKEN, 120L, 1))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.duplicate", is(false)));

    mockMvc.perform(validSettlementPost(SETTLEMENT_ID, SESSION_TOKEN, 120L, 1))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.settlementId", is(SETTLEMENT_ID)))
        .andExpect(jsonPath("$.status", is("APPLIED")))
        .andExpect(jsonPath("$.duplicate", is(true)));

    assertThat(recordCount(SETTLEMENT_ID)).isEqualTo(1);
    assertThat(inventoryQuantity(ACCOUNT_ID, ITEM_ID)).isEqualTo(1);
    assertThat(walletGold(ACCOUNT_ID)).isEqualTo(1_120L);
    assertThat(processingGuardValue(SETTLEMENT_ID)).isNull();
  }

  @Test
  void differentPayloadForExistingSettlementIdReturnsConflictWithoutChangingAssets()
      throws Exception {
    seedWallet(ACCOUNT_ID, 1_000L);
    putValidSession(SESSION_TOKEN, ACCOUNT_ID);

    mockMvc.perform(validSettlementPost(SETTLEMENT_ID, SESSION_TOKEN, 120L, 1))
        .andExpect(status().isOk());
    String originalHash = requestHash(SETTLEMENT_ID);

    mockMvc.perform(validSettlementPost(SETTLEMENT_ID, SESSION_TOKEN, 121L, 1))
        .andExpect(status().isConflict());

    assertThat(recordCount(SETTLEMENT_ID)).isEqualTo(1);
    assertThat(requestHash(SETTLEMENT_ID)).isEqualTo(originalHash);
    assertThat(inventoryQuantity(ACCOUNT_ID, ITEM_ID)).isEqualTo(1);
    assertThat(walletGold(ACCOUNT_ID)).isEqualTo(1_120L);
    assertThat(processingGuardValue(SETTLEMENT_ID)).isNull();
  }

  @Test
  void missingInternalTokenIsRejectedBeforeSettlementIsApplied() throws Exception {
    putValidSession(SESSION_TOKEN, ACCOUNT_ID);

    mockMvc
        .perform(
            post("/internal/settlements")
                .header(SettlementSessionValidator.HEADER_NAME, SESSION_TOKEN)
                .contentType(MediaType.APPLICATION_JSON)
                .content(settlementJson(SETTLEMENT_ID, 120L, 1)))
        .andExpect(status().isUnauthorized());

    assertThat(recordCount(SETTLEMENT_ID)).isZero();
  }

  @Test
  void missingSessionTokenIsRejectedBeforeSettlementIsApplied() throws Exception {
    mockMvc
        .perform(
            post("/internal/settlements")
                .header(InternalTokenInterceptor.HEADER_NAME, INTERNAL_TOKEN)
                .contentType(MediaType.APPLICATION_JSON)
                .content(settlementJson(SETTLEMENT_ID, 120L, 1)))
        .andExpect(status().isUnauthorized());

    assertThat(recordCount(SETTLEMENT_ID)).isZero();
    assertThat(processingGuardValue(SETTLEMENT_ID)).isNull();
  }

  @Test
  void invalidSessionTokenIsRejectedBeforeSettlementIsApplied() throws Exception {
    putSession("wrong-session-token", 88L, futureExpiresAt(), "PLAYER");

    mockMvc
        .perform(validSettlementPost(SETTLEMENT_ID, "wrong-session-token", 120L, 1))
        .andExpect(status().isUnauthorized());

    assertThat(recordCount(SETTLEMENT_ID)).isZero();
    assertThat(processingGuardValue(SETTLEMENT_ID)).isNull();
  }

  @Test
  void assetFailureRollsBackRecordAndInventoryThroughHttpEndpoint() throws Exception {
    seedWallet(ACCOUNT_ID, 100L);
    putValidSession(SESSION_TOKEN, ACCOUNT_ID);
    String settlementId = "rollback-through-api";

    mockMvc
        .perform(validSettlementPost(settlementId, SESSION_TOKEN, -101L, 3))
        .andExpect(status().isConflict());

    assertThat(recordCount(settlementId)).isZero();
    assertThat(inventoryCount(ACCOUNT_ID, ITEM_ID)).isZero();
    assertThat(walletGold(ACCOUNT_ID)).isEqualTo(100L);
    assertThat(processingGuardValue(settlementId)).isNull();
  }

  @Test
  void existingProcessingGuardReturnsConflictWithoutApplyingSettlement() throws Exception {
    seedWallet(ACCOUNT_ID, 1_000L);
    putValidSession(SESSION_TOKEN, ACCOUNT_ID);
    SettlementProcessingGuard.Lease lease =
        settlementProcessingGuard.acquire(SETTLEMENT_ID).orElseThrow();

    try {
      mockMvc.perform(validSettlementPost(SETTLEMENT_ID, SESSION_TOKEN, 120L, 1))
          .andExpect(status().isConflict());

      assertThat(recordCount(SETTLEMENT_ID)).isZero();
      assertThat(inventoryCount(ACCOUNT_ID, ITEM_ID)).isZero();
      assertThat(walletGold(ACCOUNT_ID)).isEqualTo(1_000L);
      assertThat(processingGuardValue(SETTLEMENT_ID)).isNotNull();
    } finally {
      settlementProcessingGuard.release(lease);
    }
  }

  private MockHttpServletRequestBuilder validSettlementPost(
      String settlementId, String sessionToken, long goldDelta, int quantityDelta) {
    return post("/internal/settlements")
        .header(InternalTokenInterceptor.HEADER_NAME, INTERNAL_TOKEN)
        .header(SettlementSessionValidator.HEADER_NAME, sessionToken)
        .contentType(MediaType.APPLICATION_JSON)
        .content(settlementJson(settlementId, goldDelta, quantityDelta));
  }

  private String settlementJson(String settlementId, long goldDelta, int quantityDelta) {
    return """
        {
          "settlementId": "%s",
          "sessionId": 1001,
          "accountId": 77,
          "roomId": 42,
          "startedAt": "2026-05-15T01:00:00Z",
          "finishedAt": "2026-05-15T01:05:00Z",
          "goldDelta": %d,
          "inventoryDeltas": [
            { "itemId": 30001, "quantityDelta": %d, "sourceDropId": 9001 }
          ],
          "reason": "NORMAL"
        }
        """
        .formatted(settlementId, goldDelta, quantityDelta);
  }

  private void putValidSession(String token, long accountId) {
    putSession(token, accountId, futureExpiresAt(), "PLAYER");
  }

  private void putSession(String token, long accountId, String expiresAt, String role) {
    redisTemplate
        .opsForHash()
        .putAll(
            "session:" + token,
            Map.of(
                "accountId", Long.toString(accountId),
                "expiresAt", expiresAt,
                "role", role));
  }

  private String futureExpiresAt() {
    return Long.toString(System.currentTimeMillis() + 60_000L);
  }

  private void seedWallet(long accountId, long gold) {
    jdbcTemplate.update(
        "INSERT INTO wallets (account_id, gold) VALUES (?, ?)", accountId, gold);
  }

  private Integer recordCount(String settlementId) {
    return jdbcTemplate.queryForObject(
        """
        SELECT COUNT(*)
        FROM settlement_records
        WHERE settlement_id = ?
        """,
        Integer.class,
        settlementId);
  }

  private String requestHash(String settlementId) {
    return jdbcTemplate.queryForObject(
        """
        SELECT request_hash
        FROM settlement_records
        WHERE settlement_id = ?
        """,
        String.class,
        settlementId);
  }

  private Integer inventoryCount(long accountId, long itemId) {
    return jdbcTemplate.queryForObject(
        """
        SELECT COUNT(*)
        FROM inventories
        WHERE account_id = ?
          AND item_id = ?
        """,
        Integer.class,
        accountId,
        itemId);
  }

  private Integer inventoryQuantity(long accountId, long itemId) {
    return jdbcTemplate.queryForObject(
        """
        SELECT quantity
        FROM inventories
        WHERE account_id = ?
          AND item_id = ?
        """,
        Integer.class,
        accountId,
        itemId);
  }

  private Long walletGold(long accountId) {
    return jdbcTemplate.queryForObject(
        """
        SELECT gold
        FROM wallets
        WHERE account_id = ?
        """,
        Long.class,
        accountId);
  }

  private String processingGuardValue(String settlementId) {
    return redisTemplate.opsForValue().get("settlement:processing:" + settlementId);
  }
}
