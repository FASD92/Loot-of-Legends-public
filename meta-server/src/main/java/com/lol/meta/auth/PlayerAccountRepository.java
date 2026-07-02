package com.lol.meta.auth;

import java.sql.PreparedStatement;
import java.sql.Statement;
import java.util.List;
import java.util.Optional;
import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.jdbc.support.GeneratedKeyHolder;
import org.springframework.jdbc.support.KeyHolder;
import org.springframework.stereotype.Repository;
import org.springframework.transaction.annotation.Transactional;

@Repository
public class PlayerAccountRepository {

  private final JdbcTemplate jdbcTemplate;

  public PlayerAccountRepository(JdbcTemplate jdbcTemplate) {
    this.jdbcTemplate = jdbcTemplate;
  }

  public boolean existsByNickname(PlayerNickname nickname) {
    Integer count =
        jdbcTemplate.queryForObject(
            """
            SELECT COUNT(*)
            FROM player_account
            WHERE nickname = ?
            """,
            Integer.class,
            nickname.value());
    return count != null && count > 0;
  }

  public Optional<PlayerAccount> findAccount(long accountId) {
    return queryAccount(
        """
        SELECT account_id, nickname
        FROM player_account
        WHERE account_id = ?
        """,
        accountId);
  }

  public Optional<PlayerAccount> findByOAuthIdentity(String provider, String providerSubject) {
    return queryAccount(
        """
        SELECT a.account_id, a.nickname
        FROM player_oauth_identity i
        JOIN player_account a ON a.account_id = i.account_id
        WHERE i.provider = ?
          AND i.provider_subject = ?
        """,
        provider,
        providerSubject);
  }

  @Transactional
  public PlayerAccount createOAuthAccount(String provider, String providerSubject) {
    KeyHolder keyHolder = new GeneratedKeyHolder();
    jdbcTemplate.update(
        connection -> {
          PreparedStatement statement =
              connection.prepareStatement(
                  "INSERT INTO player_account () VALUES ()", Statement.RETURN_GENERATED_KEYS);
          return statement;
        },
        keyHolder);
    Number key = keyHolder.getKey();
    if (key == null) {
      throw new IllegalStateException("player_account generated key is missing");
    }
    long accountId = key.longValue();
    jdbcTemplate.update(
        """
        INSERT INTO player_oauth_identity (provider, provider_subject, account_id)
        VALUES (?, ?, ?)
        """,
        provider,
        providerSubject,
        accountId);
    return new PlayerAccount(accountId, null);
  }

  public boolean updateNickname(long accountId, PlayerNickname nickname) {
    int updated =
        jdbcTemplate.update(
            """
            UPDATE player_account
            SET nickname = ?
            WHERE account_id = ?
              AND nickname IS NULL
            """,
            nickname.value(),
            accountId);
    return updated == 1;
  }

  private Optional<PlayerAccount> queryAccount(String sql, Object... args) {
    List<PlayerAccount> accounts =
        jdbcTemplate.query(
            sql,
            (rs, rowNum) -> new PlayerAccount(rs.getLong("account_id"), rs.getString("nickname")),
            args);
    return accounts.stream().findFirst();
  }
}
