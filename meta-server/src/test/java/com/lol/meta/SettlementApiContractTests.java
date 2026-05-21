package com.lol.meta;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.SerializationFeature;
import com.fasterxml.jackson.databind.json.JsonMapper;
import com.fasterxml.jackson.datatype.jsr310.JavaTimeModule;
import com.lol.meta.settlement.api.InventoryDeltaRequest;
import com.lol.meta.settlement.api.SettlementReason;
import com.lol.meta.settlement.api.SettlementRequest;
import com.lol.meta.settlement.api.SettlementResponse;
import com.lol.meta.settlement.api.SettlementStatus;
import jakarta.validation.ConstraintViolation;
import jakarta.validation.Validation;
import jakarta.validation.Validator;
import java.time.Instant;
import java.util.List;
import java.util.Set;
import java.util.stream.Collectors;
import org.junit.jupiter.api.Test;

class SettlementApiContractTests {

  private static final Instant STARTED_AT = Instant.parse("2026-05-15T01:00:00Z");
  private static final Instant FINISHED_AT = Instant.parse("2026-05-15T01:05:00Z");

  private final ObjectMapper objectMapper =
      JsonMapper.builder()
          .addModule(new JavaTimeModule())
          .disable(SerializationFeature.WRITE_DATES_AS_TIMESTAMPS)
          .build();
  private final Validator validator = Validation.buildDefaultValidatorFactory().getValidator();

  @Test
  void normalJsonBindsToSettlementRequest() throws Exception {
    SettlementRequest request =
        objectMapper.readValue(validJsonWithReason("NORMAL"), SettlementRequest.class);

    assertThat(request.settlementId()).isEqualTo("room-42-session-1001-finished-0001");
    assertThat(request.sessionId()).isEqualTo(1001L);
    assertThat(request.accountId()).isEqualTo(77L);
    assertThat(request.roomId()).isEqualTo(42L);
    assertThat(request.startedAt()).isEqualTo(STARTED_AT);
    assertThat(request.finishedAt()).isEqualTo(FINISHED_AT);
    assertThat(request.goldDelta()).isEqualTo(120L);
    assertThat(request.reason()).isEqualTo(SettlementReason.NORMAL);
    assertThat(request.inventoryDeltas())
        .containsExactly(new InventoryDeltaRequest(30001L, 1, 9001L));
    assertThat(validator.validate(request)).isEmpty();
  }

  @Test
  void supportedReasonsBindToEnumValues() throws Exception {
    for (SettlementReason reason : SettlementReason.values()) {
      SettlementRequest request =
          objectMapper.readValue(validJsonWithReason(reason.name()), SettlementRequest.class);

      assertThat(request.reason()).isEqualTo(reason);
    }
  }

  @Test
  void invalidReasonFailsJsonBinding() {
    assertThatThrownBy(
            () -> objectMapper.readValue(validJsonWithReason("INVALID"), SettlementRequest.class))
        .isInstanceOf(JsonProcessingException.class);
  }

  @Test
  void requiredFieldsRejectNullValues() {
    SettlementRequest request =
        new SettlementRequest(null, null, null, null, null, null, null, null, null);

    assertThat(violationPaths(request))
        .contains(
            "settlementId",
            "sessionId",
            "accountId",
            "roomId",
            "startedAt",
            "finishedAt",
            "goldDelta",
            "inventoryDeltas",
            "reason");
  }

  @Test
  void blankSettlementIdIsRejected() {
    SettlementRequest request =
        new SettlementRequest(
            "   ",
            1001L,
            77L,
            42L,
            STARTED_AT,
            FINISHED_AT,
            120L,
            List.of(new InventoryDeltaRequest(30001L, 1, 9001L)),
            SettlementReason.NORMAL);

    assertThat(violationPaths(request)).contains("settlementId");
  }

  @Test
  void nonPositiveIdsAreRejected() {
    SettlementRequest request =
        new SettlementRequest(
            "settlement-001",
            0L,
            -1L,
            0L,
            STARTED_AT,
            FINISHED_AT,
            120L,
            List.of(new InventoryDeltaRequest(30001L, 1, 9001L)),
            SettlementReason.NORMAL);

    assertThat(violationPaths(request)).contains("sessionId", "accountId", "roomId");
  }

  @Test
  void zeroQuantityDeltaIsRejected() {
    SettlementRequest request =
        new SettlementRequest(
            "settlement-001",
            1001L,
            77L,
            42L,
            STARTED_AT,
            FINISHED_AT,
            10L,
            List.of(new InventoryDeltaRequest(30001L, 0, 9001L)),
            SettlementReason.NORMAL);

    assertThat(violationPaths(request))
        .anyMatch(path -> path.endsWith("inventoryDeltas[0].quantityDelta"));
  }

  @Test
  void zeroGoldAndNoInventoryDeltaIsRejected() {
    SettlementRequest request =
        new SettlementRequest(
            "settlement-001",
            1001L,
            77L,
            42L,
            STARTED_AT,
            FINISHED_AT,
            0L,
            List.of(),
            SettlementReason.NORMAL);

    assertThat(violationMessages(request))
        .contains("settlement request must include non-zero gold or inventory delta");
  }

  @Test
  void zeroGoldWithInventoryDeltaIsAccepted() {
    SettlementRequest request =
        new SettlementRequest(
            "settlement-001",
            1001L,
            77L,
            42L,
            STARTED_AT,
            FINISHED_AT,
            0L,
            List.of(new InventoryDeltaRequest(30001L, 1, 9001L)),
            SettlementReason.NORMAL);

    assertThat(validator.validate(request)).isEmpty();
  }

  @Test
  void responseSerializesAppliedDuplicateFlag() throws Exception {
    assertResponseJson(new SettlementResponse("settlement-001", SettlementStatus.APPLIED, false));
    assertResponseJson(new SettlementResponse("settlement-001", SettlementStatus.APPLIED, true));
  }

  private String validJsonWithReason(String reason) {
    return """
        {
          "settlementId": "room-42-session-1001-finished-0001",
          "sessionId": 1001,
          "accountId": 77,
          "roomId": 42,
          "startedAt": "2026-05-15T01:00:00Z",
          "finishedAt": "2026-05-15T01:05:00Z",
          "goldDelta": 120,
          "inventoryDeltas": [
            { "itemId": 30001, "quantityDelta": 1, "sourceDropId": 9001 }
          ],
          "reason": "%s"
        }
        """
        .formatted(reason);
  }

  private Set<String> violationPaths(SettlementRequest request) {
    return validator.validate(request).stream()
        .map(violation -> violation.getPropertyPath().toString())
        .collect(Collectors.toSet());
  }

  private Set<String> violationMessages(SettlementRequest request) {
    return validator.validate(request).stream()
        .map(ConstraintViolation::getMessage)
        .collect(Collectors.toSet());
  }

  private void assertResponseJson(SettlementResponse response) throws Exception {
    JsonNode json = objectMapper.readTree(objectMapper.writeValueAsString(response));

    assertThat(json.get("settlementId").asText()).isEqualTo(response.settlementId());
    assertThat(json.get("status").asText()).isEqualTo("APPLIED");
    assertThat(json.get("duplicate").asBoolean()).isEqualTo(response.duplicate());
  }
}
