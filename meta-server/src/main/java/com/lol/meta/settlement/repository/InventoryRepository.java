package com.lol.meta.settlement.repository;

import org.springframework.jdbc.UncategorizedSQLException;
import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.stereotype.Repository;

@Repository // 이 클래스가 DB 접근 계층이라는 뜻
public class InventoryRepository {

  private final JdbcTemplate jdbcTemplate;  // SQL을 직접 실행할 때 쓰는 Spring 도구

  public InventoryRepository(JdbcTemplate jdbcTemplate) {
    this.jdbcTemplate = jdbcTemplate;
  }

  public int applyQuantityDelta(long accountId, long itemId, int quantityDelta) {
    try {
      return jdbcTemplate.update( // 반환값은 DB에서 영향받은 row 수
          """
          INSERT INTO inventories (account_id, item_id, quantity)
          VALUES (?, ?, ?)
          ON DUPLICATE KEY UPDATE quantity = quantity + VALUES(quantity)
          """,
          accountId,
          itemId,
          quantityDelta);
    } catch (UncategorizedSQLException exception) {
      throw MySqlConstraintExceptionTranslator.translate(exception);
    }
  }
}
