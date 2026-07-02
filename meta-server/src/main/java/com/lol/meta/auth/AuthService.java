package com.lol.meta.auth;

import org.springframework.dao.DuplicateKeyException;
import org.springframework.stereotype.Service;

@Service
public final class AuthService {

  private final PlayerAccountRepository playerAccountRepository;
  private final CurrentAuthAccountProvider currentAuthAccountProvider;

  public AuthService(
      PlayerAccountRepository playerAccountRepository,
      CurrentAuthAccountProvider currentAuthAccountProvider) {
    this.playerAccountRepository = playerAccountRepository;
    this.currentAuthAccountProvider = currentAuthAccountProvider;
  }

  public AuthSession currentSession() {
    return currentAuthAccountProvider
        .currentAccountId()
        .flatMap(playerAccountRepository::findAccount)
        .map(
            account ->
                new AuthSession(
                    true, account.accountId(), account.nicknameRequired(), account.nickname()))
        .orElseGet(AuthSession::unauthenticated);
  }

  public AuthSession updateNickname(String rawNickname) {
    PlayerNickname nickname = PlayerNickname.parse(rawNickname);
    Long accountId =
        currentAuthAccountProvider
            .currentAccountId()
            .orElseThrow(AuthenticationRequiredException::new);
    return updateAuthenticatedNickname(accountId, nickname);
  }

  public NicknameAvailability checkNickname(String rawNickname) {
    PlayerNickname nickname = PlayerNickname.parse(rawNickname);
    boolean available = !playerAccountRepository.existsByNickname(nickname);
    return new NicknameAvailability(
        available, available ? "사용 가능한 플레이어 이름입니다" : "이미 사용 중인 플레이어 이름입니다");
  }

  private AuthSession updateAuthenticatedNickname(long accountId, PlayerNickname nickname) {
    if (playerAccountRepository.existsByNickname(nickname)) {
      throw new DuplicateNicknameException();
    }

    try {
      if (!playerAccountRepository.updateNickname(accountId, nickname)) {
        throw new NicknameAlreadySetException();
      }
    } catch (DuplicateKeyException exception) {
      throw new DuplicateNicknameException(exception);
    }
    return new AuthSession(true, accountId, false, nickname.value());
  }

  public record AuthSession(
      boolean authenticated, Long accountId, boolean nicknameRequired, String nickname) {

    public static AuthSession unauthenticated() {
      return new AuthSession(false, null, false, null);
    }
  }

  public record NicknameAvailability(boolean available, String message) {}

  public static final class AuthenticationRequiredException extends RuntimeException {}

  public static final class NicknameAlreadySetException extends RuntimeException {}

  public static final class DuplicateNicknameException extends RuntimeException {

    public DuplicateNicknameException() {}

    public DuplicateNicknameException(Throwable cause) {
      super(cause);
    }
  }
}
