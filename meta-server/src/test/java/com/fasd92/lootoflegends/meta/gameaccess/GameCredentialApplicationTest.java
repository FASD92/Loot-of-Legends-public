package com.fasd92.lootoflegends.meta.gameaccess;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertTrue;

import ch.qos.logback.classic.Logger;
import ch.qos.logback.classic.spi.ILoggingEvent;
import ch.qos.logback.core.read.ListAppender;
import com.fasd92.lootoflegends.meta.gameaccess.api.ClaimGameCredentialCommand;
import com.fasd92.lootoflegends.meta.gameaccess.api.ClaimGameCredentialResult;
import com.fasd92.lootoflegends.meta.gameaccess.api.ClaimRejectionReason;
import com.fasd92.lootoflegends.meta.gameaccess.api.IssueGameCredentialCommand;
import com.fasd92.lootoflegends.meta.gameaccess.application.GameCredentialApplication;
import com.fasd92.lootoflegends.meta.gameaccess.application.GameCredentialStore;
import com.fasd92.lootoflegends.meta.identity.api.AccountId;
import java.security.SecureRandom;
import java.time.Clock;
import java.time.Instant;
import java.time.ZoneOffset;
import java.util.Base64;
import java.util.HashMap;
import java.util.Map;
import java.util.UUID;
import org.junit.jupiter.api.Test;
import org.slf4j.LoggerFactory;

class GameCredentialApplicationTest {
  private static final Instant NOW = Instant.parse("2026-08-08T00:00:00Z");
  private static final AccountId ACCOUNT_ID =
      new AccountId(UUID.fromString("00000000-0000-4000-8000-000000000001"));

  @Test
  void issuesThirtyTwoRandomBytesForThirtySeconds() {
    var store = new FakeStore();
    var application = application(store, NOW);

    var issued = application.issue(new IssueGameCredentialCommand(ACCOUNT_ID, "player-one"));

    assertEquals(32, Base64.getUrlDecoder().decode(issued.credential()).length);
    assertFalse(issued.credential().contains("="));
    assertEquals(NOW.plusSeconds(30), issued.expiresAt());
    assertEquals(64, store.entries.keySet().iterator().next().length());
    assertFalse(store.entries.containsKey(issued.credential()));
  }

  @Test
  void enforcesAudienceExpiryAndSingleUse() {
    var store = new FakeStore();
    var application = application(store, NOW);
    var issued = application.issue(new IssueGameCredentialCommand(ACCOUNT_ID, "player-one"));

    var wrongAudience =
        application.claim(new ClaimGameCredentialCommand(issued.credential(), "another-server"));
    assertRejected(wrongAudience, ClaimRejectionReason.WRONG_AUDIENCE);
    assertRejected(
        application.claim(
            new ClaimGameCredentialCommand("not-a-credential", "loot-game-server-v1")),
        ClaimRejectionReason.INVALID);

    var firstClaim =
        application.claim(
            new ClaimGameCredentialCommand(issued.credential(), "loot-game-server-v1"));
    var claimed = assertInstanceOf(ClaimGameCredentialResult.Claimed.class, firstClaim);
    assertEquals(ACCOUNT_ID, claimed.accountId());
    assertEquals("player-one", claimed.nickname());

    var duplicate =
        application.claim(
            new ClaimGameCredentialCommand(issued.credential(), "loot-game-server-v1"));
    assertRejected(duplicate, ClaimRejectionReason.ALREADY_CONSUMED);

    var expiring = application.issue(new IssueGameCredentialCommand(ACCOUNT_ID, "player-two"));
    var afterExpiry = application(store, NOW.plusSeconds(31));
    var expired =
        afterExpiry.claim(
            new ClaimGameCredentialCommand(expiring.credential(), "loot-game-server-v1"));
    assertRejected(expired, ClaimRejectionReason.EXPIRED);
  }

  @Test
  void logsNeverContainRawCredential() {
    Logger logger = (Logger) LoggerFactory.getLogger(GameCredentialApplication.class);
    var appender = new ListAppender<ILoggingEvent>();
    appender.start();
    logger.addAppender(appender);
    try {
      var application = application(new FakeStore(), NOW);
      var issued = application.issue(new IssueGameCredentialCommand(ACCOUNT_ID, "player-one"));
      application.claim(new ClaimGameCredentialCommand(issued.credential(), "loot-game-server-v1"));

      assertTrue(
          appender.list.stream()
              .noneMatch(event -> event.getFormattedMessage().contains(issued.credential())));
    } finally {
      logger.detachAppender(appender);
    }
  }

  private static GameCredentialApplication application(FakeStore store, Instant now) {
    return new GameCredentialApplication(
        store, Clock.fixed(now, ZoneOffset.UTC), new SecureRandom());
  }

  private static void assertRejected(
      ClaimGameCredentialResult result, ClaimRejectionReason reason) {
    var rejected = assertInstanceOf(ClaimGameCredentialResult.Rejected.class, result);
    assertEquals(reason, rejected.reason());
  }

  private static final class FakeStore implements GameCredentialStore {
    private final Map<String, Entry> entries = new HashMap<>();

    @Override
    public boolean putIfAbsent(
        String hash,
        AccountId accountId,
        String nickname,
        String audience,
        Instant expiresAt,
        Instant purgeAt) {
      return entries.putIfAbsent(hash, new Entry(accountId, nickname, audience, expiresAt, false))
          == null;
    }

    @Override
    public ClaimGameCredentialResult claim(String hash, String audience, Instant now) {
      Entry entry = entries.get(hash);
      if (entry == null) {
        return new ClaimGameCredentialResult.Rejected(ClaimRejectionReason.INVALID);
      }
      if (entry.consumed()) {
        return new ClaimGameCredentialResult.Rejected(ClaimRejectionReason.ALREADY_CONSUMED);
      }
      if (!now.isBefore(entry.expiresAt())) {
        return new ClaimGameCredentialResult.Rejected(ClaimRejectionReason.EXPIRED);
      }
      if (!entry.audience().equals(audience)) {
        return new ClaimGameCredentialResult.Rejected(ClaimRejectionReason.WRONG_AUDIENCE);
      }
      entries.put(
          hash,
          new Entry(
              entry.accountId(), entry.nickname(), entry.audience(), entry.expiresAt(), true));
      return new ClaimGameCredentialResult.Claimed(entry.accountId(), entry.nickname());
    }

    private record Entry(
        AccountId accountId,
        String nickname,
        String audience,
        Instant expiresAt,
        boolean consumed) {}
  }
}
