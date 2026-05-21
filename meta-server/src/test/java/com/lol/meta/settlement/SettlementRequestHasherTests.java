package com.lol.meta.settlement;

import static org.assertj.core.api.Assertions.assertThat;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.lol.meta.settlement.api.InventoryDeltaRequest;
import com.lol.meta.settlement.api.SettlementReason;
import com.lol.meta.settlement.api.SettlementRequest;
import java.time.Instant;
import java.time.OffsetDateTime;
import java.util.List;
import org.junit.jupiter.api.Test;

class SettlementRequestHasherTests {

  private static final Instant STARTED_AT = Instant.parse("2026-05-15T01:00:00Z");
  private static final Instant FINISHED_AT = Instant.parse("2026-05-15T01:05:00Z");

  private final SettlementRequestHasher hasher = new SettlementRequestHasher(new ObjectMapper());

  @Test
  void sameRequestProducesSameLowercaseSha256Hash() {
    String firstHash = hasher.hash(validRequest());
    String secondHash = hasher.hash(validRequest());

    assertThat(firstHash).isEqualTo(secondHash);
    assertThat(firstHash).matches("[0-9a-f]{64}");
  }

  @Test
  void equivalentInstantsProduceSameHash() {
    SettlementRequest first = requestWithStartedAt(Instant.parse("2026-05-15T01:00:00Z"));
    SettlementRequest second =
        requestWithStartedAt(OffsetDateTime.parse("2026-05-15T10:00:00+09:00").toInstant());

    assertThat(hasher.hash(first)).isEqualTo(hasher.hash(second));
  }

  @Test
  void differentSettlementIdProducesDifferentHash() {
    SettlementRequest first = requestWithSettlementId("settlement-001");
    SettlementRequest second = requestWithSettlementId("settlement-002");

    assertThat(hasher.hash(first)).isNotEqualTo(hasher.hash(second));
  }

  @Test
  void differentGoldDeltaProducesDifferentHash() {
    SettlementRequest first = requestWithGoldDelta(120L);
    SettlementRequest second = requestWithGoldDelta(121L);

    assertThat(hasher.hash(first)).isNotEqualTo(hasher.hash(second));
  }

  @Test
  void differentInventoryDeltaContentProducesDifferentHash() {
    SettlementRequest first =
        requestWithInventoryDeltas(List.of(new InventoryDeltaRequest(30001L, 1, 9001L)));
    SettlementRequest second =
        requestWithInventoryDeltas(List.of(new InventoryDeltaRequest(30001L, 2, 9001L)));

    assertThat(hasher.hash(first)).isNotEqualTo(hasher.hash(second));
  }

  @Test
  void differentInventoryDeltaOrderProducesDifferentHash() {
    InventoryDeltaRequest firstDelta = new InventoryDeltaRequest(30001L, 1, 9001L);
    InventoryDeltaRequest secondDelta = new InventoryDeltaRequest(30002L, 2, 9002L);
    SettlementRequest first = requestWithInventoryDeltas(List.of(firstDelta, secondDelta));
    SettlementRequest second = requestWithInventoryDeltas(List.of(secondDelta, firstDelta));

    assertThat(hasher.hash(first)).isNotEqualTo(hasher.hash(second));
  }

  private SettlementRequest validRequest() {
    return new SettlementRequest(
        "room-42-session-1001-finished-0001",
        1001L,
        77L,
        42L,
        STARTED_AT,
        FINISHED_AT,
        120L,
        List.of(new InventoryDeltaRequest(30001L, 1, 9001L)),
        SettlementReason.NORMAL);
  }

  private SettlementRequest requestWithStartedAt(Instant startedAt) {
    return new SettlementRequest(
        "room-42-session-1001-finished-0001",
        1001L,
        77L,
        42L,
        startedAt,
        FINISHED_AT,
        120L,
        List.of(new InventoryDeltaRequest(30001L, 1, 9001L)),
        SettlementReason.NORMAL);
  }

  private SettlementRequest requestWithSettlementId(String settlementId) {
    return new SettlementRequest(
        settlementId,
        1001L,
        77L,
        42L,
        STARTED_AT,
        FINISHED_AT,
        120L,
        List.of(new InventoryDeltaRequest(30001L, 1, 9001L)),
        SettlementReason.NORMAL);
  }

  private SettlementRequest requestWithGoldDelta(long goldDelta) {
    return new SettlementRequest(
        "room-42-session-1001-finished-0001",
        1001L,
        77L,
        42L,
        STARTED_AT,
        FINISHED_AT,
        goldDelta,
        List.of(new InventoryDeltaRequest(30001L, 1, 9001L)),
        SettlementReason.NORMAL);
  }

  private SettlementRequest requestWithInventoryDeltas(
      List<InventoryDeltaRequest> inventoryDeltas) {
    return new SettlementRequest(
        "room-42-session-1001-finished-0001",
        1001L,
        77L,
        42L,
        STARTED_AT,
        FINISHED_AT,
        120L,
        inventoryDeltas,
        SettlementReason.NORMAL);
  }
}
