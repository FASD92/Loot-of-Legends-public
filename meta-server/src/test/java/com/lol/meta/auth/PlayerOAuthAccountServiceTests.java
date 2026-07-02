package com.lol.meta.auth;

import static org.assertj.core.api.Assertions.assertThat;

import com.lol.meta.support.AbstractContainerIntegrationTest;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.jdbc.core.JdbcTemplate;

class PlayerOAuthAccountServiceTests extends AbstractContainerIntegrationTest {

  @Autowired private JdbcTemplate jdbcTemplate;
  @Autowired private PlayerOAuthAccountService playerOAuthAccountService;

  @BeforeEach
  void cleanTables() {
    jdbcTemplate.update("DELETE FROM player_oauth_identity");
    jdbcTemplate.update("DELETE FROM player_account");
  }

  @Test
  void resolveOAuthAccountCreatesAccountOnceForSameProviderSubject() {
    PlayerAccount first =
        playerOAuthAccountService.resolveOAuthAccount("google", "google-subject-1");
    PlayerAccount second =
        playerOAuthAccountService.resolveOAuthAccount("google", "google-subject-1");

    assertThat(second).isEqualTo(first);
    assertThat(countRows("player_account")).isEqualTo(1);
    assertThat(countRows("player_oauth_identity")).isEqualTo(1);
  }

  @Test
  void resolveOAuthAccountKeepsDifferentProviderSubjectsSeparate() {
    PlayerAccount first =
        playerOAuthAccountService.resolveOAuthAccount("google", "google-subject-1");
    PlayerAccount second =
        playerOAuthAccountService.resolveOAuthAccount("google", "google-subject-2");

    assertThat(second.accountId()).isNotEqualTo(first.accountId());
    assertThat(countRows("player_account")).isEqualTo(2);
    assertThat(countRows("player_oauth_identity")).isEqualTo(2);
  }

  private int countRows(String tableName) {
    Integer count = jdbcTemplate.queryForObject("SELECT COUNT(*) FROM " + tableName, Integer.class);
    return count == null ? 0 : count;
  }
}
