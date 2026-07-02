package com.lol.meta.auth;

import static org.assertj.core.api.Assertions.assertThat;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;

import java.util.Optional;
import org.junit.jupiter.api.Test;
import org.springframework.dao.DuplicateKeyException;

class PlayerOAuthAccountServiceUnitTests {

  private final PlayerAccountRepository playerAccountRepository = mock(PlayerAccountRepository.class);
  private final PlayerOAuthAccountService playerOAuthAccountService =
      new PlayerOAuthAccountService(playerAccountRepository);

  @Test
  void resolveOAuthAccountPreservesOpaqueProviderSubject() {
    PlayerAccount account = new PlayerAccount(123L, null);
    when(playerAccountRepository.findByOAuthIdentity("google", " subject-with-spaces "))
        .thenReturn(Optional.of(account));

    assertThat(playerOAuthAccountService.resolveOAuthAccount("google", " subject-with-spaces "))
        .isEqualTo(account);

    verify(playerAccountRepository).findByOAuthIdentity("google", " subject-with-spaces ");
  }

  @Test
  void resolveOAuthAccountFallsBackToLookupWhenConcurrentCreateAlreadyInsertedIdentity() {
    PlayerAccount account = new PlayerAccount(123L, null);
    when(playerAccountRepository.findByOAuthIdentity("google", "google-subject"))
        .thenReturn(Optional.empty())
        .thenReturn(Optional.of(account));
    when(playerAccountRepository.createOAuthAccount("google", "google-subject"))
        .thenThrow(new DuplicateKeyException("duplicate oauth identity"));

    assertThat(playerOAuthAccountService.resolveOAuthAccount("google", "google-subject"))
        .isEqualTo(account);
  }
}
