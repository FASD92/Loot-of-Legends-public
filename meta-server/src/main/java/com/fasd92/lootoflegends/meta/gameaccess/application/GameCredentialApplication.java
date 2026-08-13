package com.fasd92.lootoflegends.meta.gameaccess.application;

import com.fasd92.lootoflegends.meta.gameaccess.api.ClaimGameCredentialCommand;
import com.fasd92.lootoflegends.meta.gameaccess.api.ClaimGameCredentialResult;
import com.fasd92.lootoflegends.meta.gameaccess.api.ClaimRejectionReason;
import com.fasd92.lootoflegends.meta.gameaccess.api.GameCredentialUseCase;
import com.fasd92.lootoflegends.meta.gameaccess.api.IssueGameCredentialCommand;
import com.fasd92.lootoflegends.meta.gameaccess.api.IssuedGameCredential;
import com.fasd92.lootoflegends.meta.gameaccess.domain.GameCredentialPolicy;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.security.SecureRandom;
import java.time.Clock;
import java.time.Instant;
import java.util.Base64;
import java.util.HexFormat;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Service;

@Service
public final class GameCredentialApplication implements GameCredentialUseCase {
  private static final Logger LOG = LoggerFactory.getLogger(GameCredentialApplication.class);
  private static final int MAX_COLLISION_ATTEMPTS = 4;

  private final GameCredentialStore store;
  private final Clock clock;
  private final SecureRandom random;

  public GameCredentialApplication(GameCredentialStore store, Clock clock, SecureRandom random) {
    this.store = store;
    this.clock = clock;
    this.random = random;
  }

  @Override
  public IssuedGameCredential issue(IssueGameCredentialCommand command) {
    for (int attempt = 0; attempt < MAX_COLLISION_ATTEMPTS; attempt++) {
      byte[] bytes = new byte[GameCredentialPolicy.ENTROPY_BYTES];
      random.nextBytes(bytes);
      String credential = Base64.getUrlEncoder().withoutPadding().encodeToString(bytes);
      Instant expiresAt = clock.instant().plus(GameCredentialPolicy.TTL);
      boolean stored;
      try {
        stored =
            store.putIfAbsent(
                hash(credential),
                command.accountId(),
                command.nickname(),
                GameCredentialPolicy.AUDIENCE,
                expiresAt,
                expiresAt.plus(GameCredentialPolicy.OUTCOME_RETENTION));
      } catch (GameCredentialStore.Unavailable unavailable) {
        throw new GameCredentialUseCase.DependencyUnavailable(unavailable);
      }
      if (stored) {
        LOG.info("game credential issued");
        return new IssuedGameCredential(credential, expiresAt);
      }
    }
    throw new GameCredentialUseCase.DependencyUnavailable(
        new IllegalStateException("credential collision budget exhausted"));
  }

  @Override
  public ClaimGameCredentialResult claim(ClaimGameCredentialCommand command) {
    ClaimGameCredentialResult result;
    if (command == null || !GameCredentialPolicy.isEncodedCredential(command.credential())) {
      result = new ClaimGameCredentialResult.Rejected(ClaimRejectionReason.INVALID);
    } else if (command.audience() == null) {
      result = new ClaimGameCredentialResult.Rejected(ClaimRejectionReason.INVALID);
    } else {
      try {
        result = store.claim(hash(command.credential()), command.audience(), clock.instant());
      } catch (GameCredentialStore.Unavailable unavailable) {
        result =
            new ClaimGameCredentialResult.Rejected(ClaimRejectionReason.DEPENDENCY_UNAVAILABLE);
      }
    }
    LOG.info("game credential claim outcome={}", outcome(result));
    return result;
  }

  private static String outcome(ClaimGameCredentialResult result) {
    if (result instanceof ClaimGameCredentialResult.Claimed) {
      return "CLAIMED";
    }
    return ((ClaimGameCredentialResult.Rejected) result).reason().name();
  }

  private static String hash(String credential) {
    try {
      byte[] digest =
          MessageDigest.getInstance("SHA-256")
              .digest(credential.getBytes(StandardCharsets.US_ASCII));
      return HexFormat.of().formatHex(digest);
    } catch (NoSuchAlgorithmException impossible) {
      throw new IllegalStateException("SHA-256 is unavailable", impossible);
    }
  }
}
