package com.lol.meta;

import static org.assertj.core.api.Assertions.assertThat;

import com.lol.meta.support.AbstractContainerIntegrationTest;
import java.util.List;
import java.util.Set;
import java.util.stream.Collectors;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.jdbc.core.JdbcTemplate;

class SettlementSchemaMigrationTests extends AbstractContainerIntegrationTest {

  @Autowired private JdbcTemplate jdbcTemplate;

  @Test
  void flywayAppliesInitialSettlementSchemaMigration() {
    Integer applied =
        jdbcTemplate.queryForObject(
            """
            SELECT COUNT(*)
            FROM flyway_schema_history
            WHERE version = '1'
              AND script = 'V1__create_settlement_schema.sql'
              AND success = 1
            """,
            Integer.class);

    assertThat(applied).isEqualTo(1);
  }

  @Test
  void settlementTablesExist() {
    Set<String> tableNames =
        jdbcTemplate
            .queryForList(
                """
                SELECT table_name
                FROM information_schema.tables
                WHERE table_schema = DATABASE()
                  AND table_name IN ('accounts', 'inventories', 'wallets', 'settlement_records')
                """,
                String.class)
            .stream()
            .collect(Collectors.toSet());

    assertThat(tableNames)
        .containsExactlyInAnyOrder(
            "accounts", "inventories", "wallets", "settlement_records");
  }

  @Test
  void release0AuthTablesExist() {
    Set<String> tableNames =
        jdbcTemplate
            .queryForList(
                """
                SELECT table_name
                FROM information_schema.tables
                WHERE table_schema = DATABASE()
                  AND table_name IN ('player_account', 'player_oauth_identity')
                """,
                String.class)
            .stream()
            .collect(Collectors.toSet());

    assertThat(tableNames).containsExactlyInAnyOrder("player_account", "player_oauth_identity");
  }

  @Test
  void playerNicknameColumnUsesCaseSensitiveCollation() {
    String collation =
        jdbcTemplate.queryForObject(
            """
            SELECT collation_name
            FROM information_schema.columns
            WHERE table_schema = DATABASE()
              AND table_name = 'player_account'
              AND column_name = 'nickname'
            """,
            String.class);

    assertThat(collation).isEqualTo("utf8mb4_bin");
  }

  @Test
  void playerOauthIdentityColumnsUseCaseSensitiveCollation() {
    Set<String> columnCollations =
        jdbcTemplate
            .queryForList(
                """
                SELECT CONCAT(column_name, ':', collation_name)
                FROM information_schema.columns
                WHERE table_schema = DATABASE()
                  AND table_name = 'player_oauth_identity'
                  AND column_name IN ('provider', 'provider_subject')
                """,
                String.class)
            .stream()
            .collect(Collectors.toSet());

    assertThat(columnCollations)
        .containsExactlyInAnyOrder("provider:utf8mb4_bin", "provider_subject:utf8mb4_bin");
  }

  @Test
  void primaryKeysMatchSettlementSchemaContract() {
    assertPrimaryKeyColumns("accounts", List.of("account_id"));
    assertPrimaryKeyColumns("inventories", List.of("account_id", "item_id"));
    assertPrimaryKeyColumns("wallets", List.of("account_id"));
    assertPrimaryKeyColumns("settlement_records", List.of("settlement_id"));
  }

  @Test
  void nonNegativeCheckConstraintsExist() {
    Set<String> checkConstraints =
        jdbcTemplate
            .queryForList(
                """
                SELECT constraint_name
                FROM information_schema.table_constraints
                WHERE table_schema = DATABASE()
                  AND constraint_type = 'CHECK'
                  AND constraint_name IN (
                    'inventories_quantity_non_negative',
                    'wallets_gold_non_negative'
                  )
                """,
                String.class)
            .stream()
            .collect(Collectors.toSet());

    assertThat(checkConstraints)
        .containsExactlyInAnyOrder(
            "inventories_quantity_non_negative", "wallets_gold_non_negative");
  }

  @Test
  void settlementRecordRequestHashUsesFixedLengthSha256Column() {
    String columnContract =
        jdbcTemplate.queryForObject(
            """
            SELECT CONCAT(data_type, ':', character_maximum_length)
            FROM information_schema.columns
            WHERE table_schema = DATABASE()
              AND table_name = 'settlement_records'
              AND column_name = 'request_hash'
            """,
            String.class);

    assertThat(columnContract).isEqualTo("char:64");
  }

  private void assertPrimaryKeyColumns(String tableName, List<String> expectedColumns) {
    List<String> columns =
        jdbcTemplate.queryForList(
            """
            SELECT column_name
            FROM information_schema.key_column_usage
            WHERE table_schema = DATABASE()
              AND table_name = ?
              AND constraint_name = 'PRIMARY'
            ORDER BY ordinal_position
            """,
            String.class,
            tableName);

    assertThat(columns).containsExactlyElementsOf(expectedColumns);
  }
}
