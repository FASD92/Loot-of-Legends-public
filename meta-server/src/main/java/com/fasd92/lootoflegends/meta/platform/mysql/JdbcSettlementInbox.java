package com.fasd92.lootoflegends.meta.platform.mysql;

import com.fasd92.lootoflegends.meta.identity.api.AccountId;
import com.fasd92.lootoflegends.meta.settlement.api.SettlementUseCase.Status;
import com.fasd92.lootoflegends.meta.settlement.application.SettlementInbox;
import java.util.Optional;
import org.springframework.dao.DataAccessException;
import org.springframework.jdbc.core.simple.JdbcClient;
import org.springframework.stereotype.Component;

@Component
public final class JdbcSettlementInbox implements SettlementInbox {
  private final JdbcClient jdbc;

  public JdbcSettlementInbox(JdbcClient jdbc) {
    this.jdbc = jdbc;
  }

  @Override
  public void insertIfAbsent(
      byte[] settlementId, AccountId accountId, byte[] payloadHash, byte[] canonicalPayload) {
    try {
      jdbc.sql(
              """
              INSERT INTO settlement_inbox
                  (settlement_id, account_id, payload_hash, canonical_payload, status)
              VALUES
                  (:settlementId, :accountId, :payloadHash, :canonicalPayload, 'ACCEPTED_PENDING')
              ON DUPLICATE KEY UPDATE settlement_id = settlement_id
              """)
          .param("settlementId", settlementId)
          .param("accountId", accountId.bytes())
          .param("payloadHash", payloadHash)
          .param("canonicalPayload", canonicalPayload)
          .update();
    } catch (DataAccessException exception) {
      throw new Unavailable(exception);
    }
  }

  @Override
  public Optional<StoredSettlement> findForUpdate(byte[] settlementId) {
    return find(settlementId, true);
  }

  @Override
  public Optional<StoredSettlement> find(byte[] settlementId) {
    return find(settlementId, false);
  }

  @Override
  public Optional<StoredSettlement> findNextPendingForUpdate() {
    try {
      return jdbc.sql(
              """
              SELECT settlement_id, payload_hash, canonical_payload, status
              FROM settlement_inbox
              WHERE status = 'ACCEPTED_PENDING'
              ORDER BY accepted_at, settlement_id
              LIMIT 1
              FOR UPDATE SKIP LOCKED
              """)
          .query(this::storedSettlement)
          .optional();
    } catch (DataAccessException | IllegalArgumentException exception) {
      throw new Unavailable(exception);
    }
  }

  @Override
  public void markApplied(byte[] settlementId) {
    try {
      int updated =
          jdbc.sql(
                  """
                  UPDATE settlement_inbox
                  SET status = 'APPLIED', applied_at = CURRENT_TIMESTAMP(6)
                  WHERE settlement_id = :settlementId AND status = 'ACCEPTED_PENDING'
                  """)
              .param("settlementId", settlementId)
              .update();
      if (updated != 1) {
        throw new Unavailable(new IllegalStateException("pending settlement update was lost"));
      }
    } catch (DataAccessException exception) {
      throw new Unavailable(exception);
    }
  }

  @Override
  public long countPending(AccountId accountId) {
    try {
      return jdbc.sql(
              """
              SELECT COUNT(*)
              FROM settlement_inbox
              WHERE account_id = :accountId AND status = 'ACCEPTED_PENDING'
              """)
          .param("accountId", accountId.bytes())
          .query(Long.class)
          .single();
    } catch (DataAccessException exception) {
      throw new Unavailable(exception);
    }
  }

  private Optional<StoredSettlement> find(byte[] settlementId, boolean lock) {
    String lockingClause = lock ? " FOR UPDATE" : "";
    try {
      return jdbc.sql(
              "SELECT settlement_id, payload_hash, canonical_payload, status FROM settlement_inbox WHERE settlement_id = :settlementId"
                  + lockingClause)
          .param("settlementId", settlementId)
          .query(this::storedSettlement)
          .optional();
    } catch (DataAccessException | IllegalArgumentException exception) {
      throw new Unavailable(exception);
    }
  }

  private StoredSettlement storedSettlement(java.sql.ResultSet result, int rowNumber)
      throws java.sql.SQLException {
    return new StoredSettlement(
        result.getBytes("settlement_id"),
        result.getBytes("payload_hash"),
        result.getBytes("canonical_payload"),
        Status.fromStorage(result.getString("status")));
  }
}
