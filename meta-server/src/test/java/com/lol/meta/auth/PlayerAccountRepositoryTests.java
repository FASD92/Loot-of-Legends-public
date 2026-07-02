package com.lol.meta.auth;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import com.lol.meta.support.AbstractContainerIntegrationTest;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.dao.DuplicateKeyException;
import org.springframework.jdbc.core.JdbcTemplate;

class PlayerAccountRepositoryTests extends AbstractContainerIntegrationTest {

  @Autowired private JdbcTemplate jdbcTemplate;
  @Autowired private PlayerAccountRepository playerAccountRepository;

  @BeforeEach
  void cleanTables() {
    jdbcTemplate.update("DELETE FROM player_oauth_identity");
    jdbcTemplate.update("DELETE FROM player_account");
  }

  @Test
  void caseDifferentNicknamesCanCoexist() {
    insertAccount(101L);
    insertAccount(102L);

    assertThat(playerAccountRepository.updateNickname(101L, PlayerNickname.parse("Player")))
        .isTrue();
    assertThat(playerAccountRepository.updateNickname(102L, PlayerNickname.parse("player")))
        .isTrue();

    assertThat(findNickname(101L)).isEqualTo("Player");
    assertThat(findNickname(102L)).isEqualTo("player");
  }

  @Test
  void exactDuplicateNicknameIsRejectedByUniqueConstraint() {
    insertAccount(101L);
    insertAccount(102L);
    playerAccountRepository.updateNickname(101L, PlayerNickname.parse("player"));

    assertThatThrownBy(
            () -> playerAccountRepository.updateNickname(102L, PlayerNickname.parse("player")))
        .isInstanceOf(DuplicateKeyException.class);
  }

  @Test
  void existsByNicknameIsCaseSensitive() {
    insertAccount(101L);
    playerAccountRepository.updateNickname(101L, PlayerNickname.parse("Player"));

    assertThat(playerAccountRepository.existsByNickname(PlayerNickname.parse("Player"))).isTrue();
    assertThat(playerAccountRepository.existsByNickname(PlayerNickname.parse("player"))).isFalse();
  }

  @Test
  void initialNicknameAssignmentDoesNotOverwriteExistingNickname() {
    insertAccount(101L);
    playerAccountRepository.updateNickname(101L, PlayerNickname.parse("Player"));

    assertThat(playerAccountRepository.updateNickname(101L, PlayerNickname.parse("Renamed")))
        .isFalse();
    assertThat(findNickname(101L)).isEqualTo("Player");
  }

  @Test
  void findsAccountWithNickname() {
    insertAccount(101L);
    playerAccountRepository.updateNickname(101L, PlayerNickname.parse("Player"));

    assertThat(playerAccountRepository.findAccount(101L))
        .contains(new PlayerAccount(101L, "Player"));
  }

  @Test
  void findsAccountWithoutNickname() {
    insertAccount(101L);

    assertThat(playerAccountRepository.findAccount(101L)).contains(new PlayerAccount(101L, null));
  }

  @Test
  void missingAccountLookupReturnsEmpty() {
    assertThat(playerAccountRepository.findAccount(999L)).isEmpty();
  }

  @Test
  void createsOauthAccountAndFindsItByProviderSubject() {
    PlayerAccount account =
        playerAccountRepository.createOAuthAccount("google", "google-subject-1");

    assertThat(account.accountId()).isPositive();
    assertThat(account.nickname()).isNull();
    assertThat(playerAccountRepository.findAccount(account.accountId()))
        .contains(new PlayerAccount(account.accountId(), null));
    assertThat(playerAccountRepository.findByOAuthIdentity("google", "google-subject-1"))
        .contains(new PlayerAccount(account.accountId(), null));
  }

  @Test
  void oauthProviderSubjectLookupIsCaseSensitive() {
    PlayerAccount account = playerAccountRepository.createOAuthAccount("google", "GoogleSubject");

    assertThat(playerAccountRepository.findByOAuthIdentity("google", "GoogleSubject"))
        .contains(new PlayerAccount(account.accountId(), null));
    assertThat(playerAccountRepository.findByOAuthIdentity("google", "googlesubject")).isEmpty();
  }

  @Test
  void duplicateOauthIdentityCreationRollsBackPlayerAccountInsert() {
    playerAccountRepository.createOAuthAccount("google", "google-subject-1");

    assertThatThrownBy(
            () -> playerAccountRepository.createOAuthAccount("google", "google-subject-1"))
        .isInstanceOf(DuplicateKeyException.class);

    assertThat(countRows("player_account")).isEqualTo(1);
    assertThat(countRows("player_oauth_identity")).isEqualTo(1);
  }

  private void insertAccount(long accountId) {
    jdbcTemplate.update("INSERT INTO player_account (account_id) VALUES (?)", accountId);
  }

  private String findNickname(long accountId) {
    return jdbcTemplate.queryForObject(
        """
        SELECT nickname
        FROM player_account
        WHERE account_id = ?
        """,
        String.class,
        accountId);
  }

  private int countRows(String tableName) {
    Integer count = jdbcTemplate.queryForObject("SELECT COUNT(*) FROM " + tableName, Integer.class);
    return count == null ? 0 : count;
  }
}
