package com.fasd92.lootoflegends.meta.settlement;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.springframework.security.test.web.servlet.request.SecurityMockMvcRequestPostProcessors.authentication;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.fasd92.lootoflegends.meta.app.MetaServerApplication;
import com.fasd92.lootoflegends.meta.identity.api.AccountId;
import com.fasd92.lootoflegends.meta.identity.api.AuthenticatedPrincipal;
import com.fasd92.lootoflegends.meta.settlement.api.SettlementApplyUseCase;
import com.fasd92.lootoflegends.meta.settlement.api.SettlementApplyUseCase.ApplyResult;
import com.fasd92.lootoflegends.meta.settlement.application.SettlementApplyWorker;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.Base64;
import java.util.HexFormat;
import java.util.List;
import java.util.Properties;
import java.util.UUID;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;
import org.junit.jupiter.params.provider.ValueSource;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.AutoConfigureMockMvc;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.http.MediaType;
import org.springframework.jdbc.core.simple.JdbcClient;
import org.springframework.security.authentication.UsernamePasswordAuthenticationToken;
import org.springframework.test.context.DynamicPropertyRegistry;
import org.springframework.test.context.DynamicPropertySource;
import org.springframework.test.web.servlet.MockMvc;
import org.testcontainers.containers.MySQLContainer;
import org.testcontainers.junit.jupiter.Container;
import org.testcontainers.junit.jupiter.Testcontainers;
import org.testcontainers.utility.DockerImageName;

@Testcontainers
@SpringBootTest(
    classes = MetaServerApplication.class,
    properties = {
      "loot.internal.service-token=fixture-service-value",
      "loot.settlement.apply-initial-delay-ms=600000"
    })
@AutoConfigureMockMvc
class SettlementAcceptanceTest {
  private static final ObjectMapper JSON = new ObjectMapper();
  private static final String AUTHORIZATION = "Bearer fixture-service-value";
  private static final AccountId FIRST_ACCOUNT_ID =
      new AccountId(UUID.fromString("00010203-0405-0607-0809-0a0b0c0d0e0f"));
  private static final AccountId SECOND_ACCOUNT_ID =
      new AccountId(UUID.fromString("10111213-1415-1617-1819-1a1b1c1d1e1f"));
  private static final String MYSQL_IMAGE =
      loadImages().getProperty("loot.testcontainers.mysql.image");

  @Container
  private static final MySQLContainer<?> MYSQL =
      new MySQLContainer<>(DockerImageName.parse(MYSQL_IMAGE).asCompatibleSubstituteFor("mysql"))
          .withDatabaseName("loot")
          .withCommand("--log-bin-trust-function-creators=1");

  @Autowired private MockMvc mvc;
  @Autowired private JdbcClient jdbc;
  @Autowired private SettlementApplyUseCase apply;
  @Autowired private SettlementApplyWorker worker;

  @DynamicPropertySource
  static void database(DynamicPropertyRegistry properties) {
    properties.add("spring.datasource.url", MYSQL::getJdbcUrl);
    properties.add("spring.datasource.username", MYSQL::getUsername);
    properties.add("spring.datasource.password", MYSQL::getPassword);
  }

  @BeforeEach
  void clearInbox() {
    jdbc.sql("DROP TRIGGER IF EXISTS fail_settlement_apply").update();
    jdbc.sql("DELETE FROM account_inventory").update();
    jdbc.sql("DELETE FROM account_wallet").update();
    jdbc.sql("DELETE FROM settlement_inbox").update();
  }

  @Test
  void flywayCreatesDurableInboxOnPinnedMySql() {
    Integer tableCount =
        jdbc.sql(
                """
                SELECT COUNT(*)
                FROM information_schema.tables
                WHERE table_schema = DATABASE() AND table_name = 'settlement_inbox'
                """)
            .query(Integer.class)
            .single();
    Integer migrationCount =
        jdbc.sql("SELECT COUNT(*) FROM flyway_schema_history WHERE success = 1")
            .query(Integer.class)
            .single();

    assertEquals(1, tableCount);
    assertEquals(2, migrationCount);
    assertEquals(2, jdbc.sql("SELECT COUNT(*) FROM item_catalog").query(Integer.class).single());
    assertEquals(
        400, jdbc.sql("SELECT SUM(unit_value) FROM item_catalog").query(Integer.class).single());
  }

  @ParameterizedTest
  @CsvSource({"0,0", "0,1", "1,0", "1,1", "2,0", "2,1", "3,0", "3,1"})
  void everyCanonicalSettlementGoldenIsAccepted(int batchIndex, int intentIndex) throws Exception {
    accept(goldenSubmission(batchIndex, intentIndex))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.status").value("AcceptedPending"));
  }

  @Test
  void internalEndpointRequiresServiceAuthentication() throws Exception {
    mvc.perform(
            post("/internal/v1/settlements")
                .contentType(MediaType.APPLICATION_JSON)
                .content(goldenSubmission().json()))
        .andExpect(status().isUnauthorized());

    assertEquals(0, inboxCount());
  }

  @Test
  void sameIdAndHashReplaysWithoutDuplicateRow() throws Exception {
    Submission submission = goldenSubmission();

    accept(submission)
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.settlementId").value(submission.settlementId()))
        .andExpect(jsonPath("$.status").value("AcceptedPending"));
    accept(submission)
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.settlementId").value(submission.settlementId()))
        .andExpect(jsonPath("$.status").value("AcceptedPending"));

    assertEquals(1, inboxCount());
    mvc.perform(
            get("/internal/v1/settlements/{settlementId}", submission.settlementId())
                .header("Authorization", AUTHORIZATION))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.status").value("AcceptedPending"));
  }

  @Test
  void sameIdAndHashReplaysExistingAppliedStatus() throws Exception {
    Submission submission = goldenSubmission();
    accept(submission).andExpect(status().isOk());
    jdbc.sql("UPDATE settlement_inbox SET status = 'APPLIED' WHERE settlement_id = UNHEX(:id)")
        .param("id", submission.settlementId())
        .update();

    accept(submission).andExpect(status().isOk()).andExpect(jsonPath("$.status").value("Applied"));
    assertEquals(1, inboxCount());
  }

  @Test
  void sameIdAndDifferentValidHashConflictsWithoutReplacingAcceptedPayload() throws Exception {
    Submission accepted = goldenSubmission();
    byte[] changedPayload = accepted.payload().clone();
    changedPayload[changedPayload.length - 1] ^= 1;
    Submission conflicting = submission(changedPayload);

    accept(accepted).andExpect(status().isOk());
    accept(conflicting)
        .andExpect(status().isConflict())
        .andExpect(jsonPath("$.code").value("CONFLICT"));

    assertEquals(1, inboxCount());
    byte[] storedHash =
        jdbc.sql("SELECT payload_hash FROM settlement_inbox WHERE settlement_id = UNHEX(:id)")
            .param("id", accepted.settlementId())
            .query(byte[].class)
            .single();
    assertTrue(MessageDigest.isEqual(HexFormat.of().parseHex(accepted.payloadHash()), storedHash));
  }

  @Test
  void nonCanonicalBase64IsRejectedBeforeDurableMutation() throws Exception {
    Submission canonical = goldenSubmission(1, 0);
    String padded = Base64.getEncoder().encodeToString(canonical.payload());
    assertTrue(padded.endsWith("="));
    String unpadded = padded.substring(0, padded.length() - 1);
    Submission nonCanonical =
        new Submission(
            canonical.settlementId(),
            canonical.payloadHash(),
            canonical.payload(),
            JSON.writeValueAsString(
                new SubmissionBody(canonical.settlementId(), canonical.payloadHash(), unpadded)));

    accept(nonCanonical)
        .andExpect(status().isBadRequest())
        .andExpect(jsonPath("$.code").value("INVALID"));
    assertEquals(0, inboxCount());
  }

  @Test
  void nullSubmissionIsRejectedBeforeDurableMutation() throws Exception {
    mvc.perform(
            post("/internal/v1/settlements")
                .header("Authorization", AUTHORIZATION)
                .contentType(MediaType.APPLICATION_JSON)
                .content("null"))
        .andExpect(status().isBadRequest())
        .andExpect(jsonPath("$.code").value("INVALID"));
    assertEquals(0, inboxCount());
  }

  @Test
  void collectionShowsPendingCountWithoutPredictingAssetQuantity() throws Exception {
    accept(goldenSubmission()).andExpect(status().isOk());

    mvc.perform(get("/api/v1/collection").with(authentication(playerAuthentication())))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.items.length()").value(0))
        .andExpect(jsonPath("$.wallet").value("0"))
        .andExpect(jsonPath("$.pendingSettlementCount").value(1))
        .andExpect(jsonPath("$.freshness").value("Fresh"));
  }

  @Test
  void applyMutatesAssetsOnceAndCollectionShowsOnlyAppliedValues() throws Exception {
    Submission submission = goldenSubmission();
    accept(submission).andExpect(status().isOk());

    worker.applyNext();
    assertEquals(ApplyResult.IDLE, apply.applyNext());
    accept(submission).andExpect(jsonPath("$.status").value("Applied"));

    mvc.perform(get("/api/v1/collection").with(authentication(playerAuthentication())))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.items.length()").value(1))
        .andExpect(jsonPath("$.items[0].itemId").value("2"))
        .andExpect(jsonPath("$.items[0].quantity").value("1"))
        .andExpect(jsonPath("$.items[0].value").value("300"))
        .andExpect(jsonPath("$.wallet").value("300"))
        .andExpect(jsonPath("$.pendingSettlementCount").value(0));

    assertEquals(1, inventoryQuantity(2));
    assertEquals(300, walletBalance());

    mvc.perform(
            get("/api/v1/collection").with(authentication(playerAuthentication(SECOND_ACCOUNT_ID))))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.items.length()").value(0))
        .andExpect(jsonPath("$.wallet").value("0"))
        .andExpect(jsonPath("$.pendingSettlementCount").value(0));
  }

  @ParameterizedTest
  @ValueSource(strings = {"CATALOG", "ITEM", "FINAL_VALUE"})
  void permanentlyInvalidSettlementIsRejectedBeforeInboxAndDoesNotBlockValidProgress(
      String invalidField) throws Exception {
    accept(permanentlyInvalidSubmission(invalidField))
        .andExpect(status().isBadRequest())
        .andExpect(jsonPath("$.code").value("INVALID"));
    assertEquals(0, inboxCount());
    assertEquals(0, inventoryQuantity(2));
    assertEquals(0, walletBalance());

    accept(goldenSubmission()).andExpect(status().isOk());
    assertEquals(ApplyResult.APPLIED, apply.applyNext());
    assertEquals(1, inboxCount());
    assertEquals(1, inventoryQuantity(2));
    assertEquals(300, walletBalance());
  }

  @Test
  void unsupportedCatalogDoesNotMutateAssetsOrAppliedStatus() throws Exception {
    Submission submission = permanentlyInvalidSubmission("CATALOG");
    jdbc.sql(
            """
            INSERT INTO settlement_inbox
                (settlement_id, account_id, payload_hash, canonical_payload, status)
            VALUES
                (UNHEX(:id), :accountId, UNHEX(:hash), :payload, 'ACCEPTED_PENDING')
            """)
        .param("id", submission.settlementId())
        .param("accountId", FIRST_ACCOUNT_ID.bytes())
        .param("hash", submission.payloadHash())
        .param("payload", submission.payload())
        .update();

    assertThrows(SettlementApplyUseCase.Invalid.class, apply::applyNext);
    assertEquals(0, inventoryQuantity(2));
    assertEquals(0, walletBalance());
    assertEquals("ACCEPTED_PENDING", settlementStatus());
  }

  @ParameterizedTest
  @ValueSource(strings = {"BEFORE_WALLET", "BEFORE_STATUS", "AFTER_STATUS"})
  void failureAfterEachApplyStepRollsBackEveryMutation(String failurePoint) throws Exception {
    accept(goldenSubmission()).andExpect(status().isOk());
    createFailureTrigger(failurePoint);

    assertThrows(SettlementApplyUseCase.Retryable.class, apply::applyNext);
    assertEquals(0, inventoryQuantity(2));
    assertEquals(0, walletBalance());
    assertEquals("ACCEPTED_PENDING", settlementStatus());

    jdbc.sql("DROP TRIGGER fail_settlement_apply").update();
    assertEquals(ApplyResult.APPLIED, apply.applyNext());
    assertEquals(1, inventoryQuantity(2));
    assertEquals(300, walletBalance());
    assertEquals("APPLIED", settlementStatus());
  }

  private org.springframework.test.web.servlet.ResultActions accept(Submission submission)
      throws Exception {
    return mvc.perform(
        post("/internal/v1/settlements")
            .header("Authorization", AUTHORIZATION)
            .contentType(MediaType.APPLICATION_JSON)
            .content(submission.json()));
  }

  private int inboxCount() {
    return jdbc.sql("SELECT COUNT(*) FROM settlement_inbox").query(Integer.class).single();
  }

  private int inventoryQuantity(long itemId) {
    return jdbc.sql("SELECT COALESCE(SUM(quantity), 0) FROM account_inventory WHERE item_id = :id")
        .param("id", itemId)
        .query(Integer.class)
        .single();
  }

  private int walletBalance() {
    return jdbc.sql("SELECT COALESCE(SUM(balance), 0) FROM account_wallet")
        .query(Integer.class)
        .single();
  }

  private String settlementStatus() {
    return jdbc.sql("SELECT status FROM settlement_inbox").query(String.class).single();
  }

  private void createFailureTrigger(String failurePoint) {
    String sql =
        switch (failurePoint) {
          case "BEFORE_WALLET" ->
              """
              CREATE TRIGGER fail_settlement_apply
              BEFORE INSERT ON account_wallet
              FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'injected failure'
              """;
          case "BEFORE_STATUS" ->
              """
              CREATE TRIGGER fail_settlement_apply
              BEFORE UPDATE ON settlement_inbox
              FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'injected failure'
              """;
          case "AFTER_STATUS" ->
              """
              CREATE TRIGGER fail_settlement_apply
              AFTER UPDATE ON settlement_inbox
              FOR EACH ROW SIGNAL SQLSTATE '45000' SET MESSAGE_TEXT = 'injected failure'
              """;
          default -> throw new IllegalArgumentException("unknown failure point");
        };
    jdbc.sql(sql).update();
  }

  private static UsernamePasswordAuthenticationToken playerAuthentication() {
    return playerAuthentication(FIRST_ACCOUNT_ID);
  }

  private static UsernamePasswordAuthenticationToken playerAuthentication(AccountId accountId) {
    return UsernamePasswordAuthenticationToken.authenticated(
        new AuthenticatedPrincipal(accountId, "player-one"), "fixture", List.of());
  }

  private static Submission goldenSubmission() throws Exception {
    return goldenSubmission(0, 0);
  }

  private static Submission goldenSubmission(int batchIndex, int intentIndex) throws Exception {
    JsonNode contract =
        JSON.readTree(
            Files.readString(
                Path.of("..", "contracts", "settlement", "golden", "settlement-intent-v1.json")));
    JsonNode intent =
        contract.path("goldenBatches").get(batchIndex).path("expectedIntents").get(intentIndex);
    byte[] payload = HexFormat.of().parseHex(intent.path("expectedPayloadHex").asText());
    Submission submission = submission(payload);
    assertEquals(intent.path("expectedId").asText(), submission.settlementId());
    assertEquals(intent.path("expectedHash").asText(), submission.payloadHash());
    return submission;
  }

  private static Submission permanentlyInvalidSubmission(String field) throws Exception {
    byte[] payload = goldenSubmission().payload().clone();
    switch (field) {
      case "CATALOG" -> payload[71] = 2;
      case "ITEM" -> payload[81] = 99;
      case "FINAL_VALUE" -> payload[97] = 45;
      default -> throw new IllegalArgumentException("unknown invalid field");
    }
    return submission(payload);
  }

  private static Submission submission(byte[] payload) throws Exception {
    String settlementId = HexFormat.of().formatHex(payload, 20, 36);
    String payloadHash =
        HexFormat.of().formatHex(MessageDigest.getInstance("SHA-256").digest(payload));
    String encoded = Base64.getEncoder().encodeToString(payload);
    String json = JSON.writeValueAsString(new SubmissionBody(settlementId, payloadHash, encoded));
    return new Submission(settlementId, payloadHash, payload, json);
  }

  private static Properties loadImages() {
    var properties = new Properties();
    try (var stream =
        SettlementAcceptanceTest.class
            .getClassLoader()
            .getResourceAsStream("testcontainers.properties")) {
      if (stream == null) {
        throw new IllegalStateException("testcontainers.properties is missing");
      }
      properties.load(stream);
      return properties;
    } catch (IOException exception) {
      throw new IllegalStateException("cannot read test container image", exception);
    }
  }

  private record SubmissionBody(String settlementId, String payloadHash, String canonicalPayload) {}

  private record Submission(String settlementId, String payloadHash, byte[] payload, String json) {}
}
