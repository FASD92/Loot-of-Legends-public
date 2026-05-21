package com.lol.meta.settlement.repository;

import java.sql.ResultSet;
import java.sql.SQLException;
import java.util.Optional;
import org.springframework.dao.EmptyResultDataAccessException;
import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.stereotype.Repository;

@Repository
public class SettlementRecordRepository {

  private final JdbcTemplate jdbcTemplate;

  public SettlementRecordRepository(JdbcTemplate jdbcTemplate) {
    this.jdbcTemplate = jdbcTemplate;
  }

  public int insert(SettlementRecordRow row) {
    return jdbcTemplate.update(
        """
        INSERT INTO settlement_records (
          settlement_id,
          account_id,
          session_id,
          room_id,
          status,
          gold_delta,
          request_hash
        )
        VALUES (?, ?, ?, ?, ?, ?, ?)
        """,
        row.settlementId(),
        row.accountId(),
        row.sessionId(),
        row.roomId(),
        row.status(),
        row.goldDelta(),
        row.requestHash());
  }

  public Optional<SettlementRecordRow> findBySettlementId(String settlementId) {
    try {
      return Optional.of(
          jdbcTemplate.queryForObject(
              """
              SELECT
                settlement_id,
                account_id,
                session_id,
                room_id,
                status,
                gold_delta,
                request_hash
              FROM settlement_records
              WHERE settlement_id = ?
              """,
              SettlementRecordRepository::mapRow,
              settlementId));
    } catch (EmptyResultDataAccessException ignored) {
      return Optional.empty();
    }
  }

  public Optional<SettlementRecordRow> findBySettlementIdForUpdate(String settlementId) {
    try {
      return Optional.of(
          jdbcTemplate.queryForObject(
              """
              SELECT
                settlement_id,
                account_id,
                session_id,
                room_id,
                status,
                gold_delta,
                request_hash
              FROM settlement_records
              WHERE settlement_id = ?
              FOR UPDATE
              """,
              SettlementRecordRepository::mapRow,
              settlementId));
    } catch (EmptyResultDataAccessException ignored) {
      return Optional.empty();
    }
  }

  private static SettlementRecordRow mapRow(ResultSet resultSet, int rowNumber)
      throws SQLException {
    return new SettlementRecordRow(
        resultSet.getString("settlement_id"),
        resultSet.getLong("account_id"),
        resultSet.getLong("session_id"),
        resultSet.getLong("room_id"),
        resultSet.getString("status"),
        resultSet.getLong("gold_delta"),
        resultSet.getString("request_hash"));
  }
}
