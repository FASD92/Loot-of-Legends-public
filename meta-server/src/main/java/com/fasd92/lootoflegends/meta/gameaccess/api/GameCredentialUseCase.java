package com.fasd92.lootoflegends.meta.gameaccess.api;

public interface GameCredentialUseCase {
  IssuedGameCredential issue(IssueGameCredentialCommand command);

  ClaimGameCredentialResult claim(ClaimGameCredentialCommand command);

  final class DependencyUnavailable extends RuntimeException {
    public DependencyUnavailable(Throwable cause) {
      super(cause);
    }
  }
}
