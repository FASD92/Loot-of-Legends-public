package com.lol.meta.auth;

public record PlayerAccount(long accountId, String nickname) {

  public boolean nicknameRequired() {
    return nickname == null || nickname.isBlank();
  }
}
