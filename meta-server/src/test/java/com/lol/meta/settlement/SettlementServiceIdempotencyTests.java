package com.lol.meta.settlement;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import com.lol.meta.settlement.api.InventoryDeltaRequest;
import com.lol.meta.settlement.api.SettlementReason;
import com.lol.meta.settlement.api.SettlementRequest;
import com.lol.meta.settlement.api.SettlementResponse;
import com.lol.meta.settlement.api.SettlementStatus;
import com.lol.meta.settlement.repository.SettlementRecordRepository;
import com.lol.meta.settlement.repository.SettlementRecordRow;
import com.lol.meta.support.AbstractContainerIntegrationTest;
import java.time.Instant;
import java.util.List;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.jdbc.core.JdbcTemplate;

class SettlementServiceIdempotencyTests extends AbstractContainerIntegrationTest {

  private static final Instant STARTED_AT = Instant.parse("2026-05-15T01:00:00Z");
  private static final Instant FINISHED_AT = Instant.parse("2026-05-15T01:05:00Z");

  @Autowired private JdbcTemplate jdbcTemplate;
  @Autowired private SettlementRecordRepository settlementRecordRepository;
  @Autowired private SettlementRequestHasher settlementRequestHasher;
  @Autowired private SettlementService settlementService;

  @BeforeEach
  void cleanTables() {
    jdbcTemplate.update("DELETE FROM settlement_records");
    jdbcTemplate.update("DELETE FROM inventories");
    jdbcTemplate.update("DELETE FROM wallets");
    jdbcTemplate.update("DELETE FROM accounts");
  }

  @Test
  void newSettlementRequestInsertsAppliedRecord() {
    SettlementRequest request = validRequest();
    seedWallet(request.accountId(), 1_000L);

    SettlementResponse response = settlementService.apply(request);
    SettlementRecordRow row =
        settlementRecordRepository.findBySettlementId(request.settlementId()).orElseThrow();

    assertThat(response)
        .isEqualTo(
            new SettlementResponse(request.settlementId(), SettlementStatus.APPLIED, false));
    assertThat(row.status()).isEqualTo("APPLIED");
    assertThat(row.requestHash()).isEqualTo(settlementRequestHasher.hash(request));
    assertThat(inventoryQuantity(request.accountId(), 30001L)).isEqualTo(1);
    assertThat(walletGold(request.accountId())).isEqualTo(1_120L);
  }

  @Test
  void sameSettlementIdAndSamePayloadReturnsDuplicateWithoutSecondInsert() {
    SettlementRequest request = validRequest();
    seedWallet(request.accountId(), 1_000L);
    settlementService.apply(request);

    SettlementResponse duplicateResponse = settlementService.apply(request);

    assertThat(duplicateResponse)
        .isEqualTo(
            new SettlementResponse(request.settlementId(), SettlementStatus.APPLIED, true));
    assertThat(recordCount(request.settlementId())).isEqualTo(1);
    assertThat(inventoryQuantity(request.accountId(), 30001L)).isEqualTo(1);
    assertThat(walletGold(request.accountId())).isEqualTo(1_120L);
  }

  @Test
  void sameSettlementIdAndDifferentPayloadFailsAsConflictWithoutChangingExistingRecord() {
    SettlementRequest original = validRequest();
    SettlementRequest conflict = requestWithGoldDelta(121L);
    seedWallet(original.accountId(), 1_000L);
    settlementService.apply(original);
    String originalHash =
        settlementRecordRepository
            .findBySettlementId(original.settlementId())
            .orElseThrow()
            .requestHash();

    assertThatThrownBy(() -> settlementService.apply(conflict))
        .isInstanceOf(SettlementConflictException.class);

    SettlementRecordRow row =
        settlementRecordRepository.findBySettlementId(original.settlementId()).orElseThrow();
    assertThat(row.requestHash()).isEqualTo(originalHash);
    assertThat(recordCount(original.settlementId())).isEqualTo(1);
  }

  @Test
  void settlementRecordCanBeFoundForUpdate() {
    SettlementRequest request = validRequest();
    seedWallet(request.accountId(), 1_000L);
    settlementService.apply(request);

    assertThat(settlementRecordRepository.findBySettlementIdForUpdate(request.settlementId()))
        .contains(
            new SettlementRecordRow(
                request.settlementId(),
                request.accountId(),
                request.sessionId(),
                request.roomId(),
                "APPLIED",
                request.goldDelta(),
                settlementRequestHasher.hash(request)));
  }

  @Test
  void negativeInventoryResultFailsAsAssetConflict() {
    SettlementRequest request =
        requestWithGoldAndInventoryDeltas(
            0L, List.of(new InventoryDeltaRequest(30001L, -2, 9001L)));
    jdbcTemplate.update(
        "INSERT INTO inventories (account_id, item_id, quantity) VALUES (?, ?, ?)",
        request.accountId(),
        30001L,
        1);

    assertThatThrownBy(() -> settlementService.apply(request))
        .isInstanceOf(SettlementAssetConflictException.class);

    assertThat(recordCount(request.settlementId())).isZero();
  }

  @Test
  void negativeWalletResultFailsAsAssetConflict() {
    SettlementRequest request = requestWithGoldAndInventoryDeltas(-101L, List.of());
    seedWallet(request.accountId(), 100L);

    assertThatThrownBy(() -> settlementService.apply(request))
        .isInstanceOf(SettlementAssetConflictException.class);
  }

  @Test
  void nonZeroGoldDeltaWithoutWalletFailsAsAssetConflict() {
    SettlementRequest request = requestWithGoldAndInventoryDeltas(120L, List.of());

    assertThatThrownBy(() -> settlementService.apply(request))
        .isInstanceOf(SettlementAssetConflictException.class);
  }

  @Test
  void zeroGoldDeltaWithInventoryDeltaSucceedsWithoutWallet() {
    SettlementRequest request =
        requestWithGoldAndInventoryDeltas(
            0L, List.of(new InventoryDeltaRequest(30002L, 2, 9002L)));

    SettlementResponse response = settlementService.apply(request);

    assertThat(response)
        .isEqualTo(
            new SettlementResponse(request.settlementId(), SettlementStatus.APPLIED, false));
    assertThat(inventoryQuantity(request.accountId(), 30002L)).isEqualTo(2);
    assertThat(walletCount(request.accountId())).isZero();
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

  private Integer walletCount(long accountId) {
    return jdbcTemplate.queryForObject(
        """
        SELECT COUNT(*)
        FROM wallets
        WHERE account_id = ?
        """,
        Integer.class,
        accountId);
  }

  private void seedWallet(long accountId, long gold) {
    jdbcTemplate.update(
        "INSERT INTO wallets (account_id, gold) VALUES (?, ?)", accountId, gold);
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

  private SettlementRequest requestWithGoldDelta(long goldDelta) {
    return requestWithGoldAndInventoryDeltas(
        goldDelta, List.of(new InventoryDeltaRequest(30001L, 1, 9001L)));
  }

  private SettlementRequest requestWithGoldAndInventoryDeltas(
      long goldDelta, List<InventoryDeltaRequest> inventoryDeltas) {
    return new SettlementRequest(
        "room-42-session-1001-finished-0001",
        1001L,
        77L,
        42L,
        STARTED_AT,
        FINISHED_AT,
        goldDelta,
        inventoryDeltas,
        SettlementReason.NORMAL);
  }
}
