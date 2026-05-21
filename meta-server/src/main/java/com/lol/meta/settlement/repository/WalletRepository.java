package com.lol.meta.settlement.repository;

import org.springframework.jdbc.UncategorizedSQLException;
import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.stereotype.Repository;

@Repository
public class WalletRepository {

  private final JdbcTemplate jdbcTemplate;

  public WalletRepository(JdbcTemplate jdbcTemplate) {
    this.jdbcTemplate = jdbcTemplate;
  }

  public int applyGoldDelta(long accountId, long goldDelta) {
    try {
      return jdbcTemplate.update(
          """
          UPDATE wallets
          SET gold = gold + ?
          WHERE account_id = ?
          """,
          goldDelta,
          accountId);
    } catch (UncategorizedSQLException exception) {
      throw MySqlConstraintExceptionTranslator.translate(exception);
    }
  }
}
