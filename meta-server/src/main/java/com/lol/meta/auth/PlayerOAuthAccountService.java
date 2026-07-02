package com.lol.meta.auth;

import org.springframework.dao.DuplicateKeyException;
import org.springframework.stereotype.Service;

@Service
public final class PlayerOAuthAccountService implements OAuthAccountResolver {

  private final PlayerAccountRepository playerAccountRepository;

  public PlayerOAuthAccountService(PlayerAccountRepository playerAccountRepository) {
    this.playerAccountRepository = playerAccountRepository;
  }

  @Override
  public PlayerAccount resolveOAuthAccount(String provider, String providerSubject) {
    String normalizedProvider = requireText(provider, "provider");
    String normalizedProviderSubject = requireText(providerSubject, "providerSubject");
    return playerAccountRepository
        .findByOAuthIdentity(normalizedProvider, normalizedProviderSubject)
        .orElseGet(() -> createOrFind(normalizedProvider, normalizedProviderSubject));
  }

  private PlayerAccount createOrFind(String provider, String providerSubject) {
    try {
      return playerAccountRepository.createOAuthAccount(provider, providerSubject);
    } catch (DuplicateKeyException exception) {
      return playerAccountRepository
          .findByOAuthIdentity(provider, providerSubject)
          .orElseThrow(() -> exception);
    }
  }

  private static String requireText(String value, String fieldName) {
    if (value == null || value.isBlank()) {
      throw new IllegalArgumentException(fieldName + " must not be blank");
    }
    return value;
  }
}
