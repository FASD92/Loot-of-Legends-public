package com.lol.meta;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import com.lol.meta.settlement.repository.InventoryRepository;
import com.lol.meta.settlement.repository.SettlementRecordRepository;
import com.lol.meta.settlement.repository.SettlementRecordRow;
import com.lol.meta.settlement.repository.WalletRepository;
import com.lol.meta.support.AbstractContainerIntegrationTest;
import java.util.Optional;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.dao.DataIntegrityViolationException;
import org.springframework.dao.DuplicateKeyException;
import org.springframework.jdbc.core.JdbcTemplate;

class SettlementRepositoryTests extends AbstractContainerIntegrationTest {

  private static final String REQUEST_HASH =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

  @Autowired private JdbcTemplate jdbcTemplate;
  @Autowired private InventoryRepository inventoryRepository;
  @Autowired private SettlementRecordRepository settlementRecordRepository;
  @Autowired private WalletRepository walletRepository;

  @BeforeEach
  void cleanTables() {
    jdbcTemplate.update("DELETE FROM settlement_records");
    jdbcTemplate.update("DELETE FROM inventories");
    jdbcTemplate.update("DELETE FROM wallets");
    jdbcTemplate.update("DELETE FROM accounts");
  }

  @Test
  void settlementRecordCanBeInsertedAndFound() {
    SettlementRecordRow row =
        new SettlementRecordRow(
            "settlement-001", 77L, 1001L, 42L, "PROCESSING", 120L, REQUEST_HASH);

    int inserted = settlementRecordRepository.insert(row);
    Optional<SettlementRecordRow> found =
        settlementRecordRepository.findBySettlementId("settlement-001");

    assertThat(inserted).isEqualTo(1);
    assertThat(found).contains(row);
  }

  @Test
  void duplicateSettlementIdFailsOnPrimaryKey() {
    SettlementRecordRow row =
        new SettlementRecordRow(
            "settlement-duplicate", 77L, 1001L, 42L, "PROCESSING", 120L, REQUEST_HASH);
    settlementRecordRepository.insert(row);

    assertThatThrownBy(() -> settlementRecordRepository.insert(row))
        .isInstanceOf(DuplicateKeyException.class);
  }

  @Test
  void inventoryPositiveDeltaCreatesOrIncrementsQuantity() {
    inventoryRepository.applyQuantityDelta(77L, 30001L, 2);
    inventoryRepository.applyQuantityDelta(77L, 30001L, 3);

    Integer quantity =
        jdbcTemplate.queryForObject(
            """
            SELECT quantity
            FROM inventories
            WHERE account_id = ?
              AND item_id = ?
            """,
            Integer.class,
            77L,
            30001L);

    assertThat(quantity).isEqualTo(5);
  }

  @Test
  void inventoryNegativeResultFailsOnCheckConstraint() {
    inventoryRepository.applyQuantityDelta(77L, 30001L, 1);

    assertThatThrownBy(() -> inventoryRepository.applyQuantityDelta(77L, 30001L, -2))
        .isInstanceOf(DataIntegrityViolationException.class);
  }

  @Test
  void walletPositiveDeltaIncrementsExistingWallet() {
    jdbcTemplate.update("INSERT INTO wallets (account_id, gold) VALUES (?, ?)", 77L, 100L);

    int updated = walletRepository.applyGoldDelta(77L, 25L);
    Long gold =
        jdbcTemplate.queryForObject(
            """
            SELECT gold
            FROM wallets
            WHERE account_id = ?
            """,
            Long.class,
            77L);

    assertThat(updated).isEqualTo(1);
    assertThat(gold).isEqualTo(125L);
  }

  @Test
  void walletNegativeResultFailsOnCheckConstraint() {
    jdbcTemplate.update("INSERT INTO wallets (account_id, gold) VALUES (?, ?)", 77L, 100L);

    assertThatThrownBy(() -> walletRepository.applyGoldDelta(77L, -101L))
        .isInstanceOf(DataIntegrityViolationException.class);
  }
}
