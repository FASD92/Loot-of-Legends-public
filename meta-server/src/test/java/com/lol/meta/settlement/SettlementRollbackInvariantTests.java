package com.lol.meta.settlement;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import com.lol.meta.settlement.api.InventoryDeltaRequest;
import com.lol.meta.settlement.api.SettlementReason;
import com.lol.meta.settlement.api.SettlementRequest;
import com.lol.meta.support.AbstractContainerIntegrationTest;
import java.time.Instant;
import java.util.List;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.jdbc.core.JdbcTemplate;

class SettlementRollbackInvariantTests extends AbstractContainerIntegrationTest {

  private static final long ACCOUNT_ID = 77L;
  private static final Instant STARTED_AT = Instant.parse("2026-05-15T01:00:00Z");
  private static final Instant FINISHED_AT = Instant.parse("2026-05-15T01:05:00Z");

  @Autowired private JdbcTemplate jdbcTemplate;
  @Autowired private SettlementService settlementService;

  @BeforeEach
  void cleanTables() {
    jdbcTemplate.update("DELETE FROM settlement_records");
    jdbcTemplate.update("DELETE FROM inventories");
    jdbcTemplate.update("DELETE FROM wallets");
    jdbcTemplate.update("DELETE FROM accounts");
  }

  @Test
  void newInventoryRowIsRolledBackWhenWalletUpdateFails() {
    seedWallet(ACCOUNT_ID, 100L);
    SettlementRequest request =
        request(
            "rollback-new-inventory",
            -101L,
            List.of(new InventoryDeltaRequest(30001L, 3, 9001L)));

    assertThatThrownBy(() -> settlementService.apply(request))
        .isInstanceOf(SettlementAssetConflictException.class);

    assertThat(settlementRecordCount(request.settlementId())).isZero();
    assertThat(inventoryCount(ACCOUNT_ID, 30001L)).isZero();
    assertThat(walletGold(ACCOUNT_ID)).isEqualTo(100L);
  }

  @Test
  void existingInventoryUpdateIsRolledBackWhenWalletUpdateFails() {
    seedInventory(ACCOUNT_ID, 30001L, 5);
    seedWallet(ACCOUNT_ID, 100L);
    SettlementRequest request =
        request(
            "rollback-existing-inventory",
            -101L,
            List.of(new InventoryDeltaRequest(30001L, 3, 9001L)));

    assertThatThrownBy(() -> settlementService.apply(request))
        .isInstanceOf(SettlementAssetConflictException.class);

    assertThat(settlementRecordCount(request.settlementId())).isZero();
    assertThat(inventoryQuantity(ACCOUNT_ID, 30001L)).isEqualTo(5);
    assertThat(walletGold(ACCOUNT_ID)).isEqualTo(100L);
  }

  private void seedInventory(long accountId, long itemId, int quantity) {
    jdbcTemplate.update(
        "INSERT INTO inventories (account_id, item_id, quantity) VALUES (?, ?, ?)",
        accountId,
        itemId,
        quantity);
  }

  private void seedWallet(long accountId, long gold) {
    jdbcTemplate.update(
        "INSERT INTO wallets (account_id, gold) VALUES (?, ?)", accountId, gold);
  }

  private Integer settlementRecordCount(String settlementId) {
    return jdbcTemplate.queryForObject(
        """
        SELECT COUNT(*)
        FROM settlement_records
        WHERE settlement_id = ?
        """,
        Integer.class,
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

  private SettlementRequest request(
      String settlementId, long goldDelta, List<InventoryDeltaRequest> inventoryDeltas) {
    return new SettlementRequest(
        settlementId,
        1001L,
        ACCOUNT_ID,
        42L,
        STARTED_AT,
        FINISHED_AT,
        goldDelta,
        inventoryDeltas,
        SettlementReason.NORMAL);
  }
}
