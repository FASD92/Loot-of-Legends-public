package com.lol.meta.settlement;

import static org.assertj.core.api.Assertions.assertThatCode;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import com.lol.meta.support.AbstractContainerIntegrationTest;
import java.util.Map;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.data.redis.core.RedisCallback;
import org.springframework.data.redis.core.StringRedisTemplate;

class SettlementSessionValidatorTests extends AbstractContainerIntegrationTest {

  private static final long ACCOUNT_ID = 77L;
  private static final String TOKEN = "session-token-001";

  @Autowired private StringRedisTemplate redisTemplate;
  @Autowired private SettlementSessionValidator settlementSessionValidator;

  @BeforeEach
  void cleanRedis() {
    redisTemplate.execute(
        (RedisCallback<Void>)
            connection -> {
              connection.serverCommands().flushDb();
              return null;
            });
  }

  @Test
  void validSessionTokenAllowsMatchingAccount() {
    putSession(TOKEN, ACCOUNT_ID, futureExpiresAt(), "PLAYER");

    assertThatCode(() -> settlementSessionValidator.validate(TOKEN, ACCOUNT_ID))
        .doesNotThrowAnyException();
  }

  @Test
  void missingOrBlankTokenFails() {
    assertThatThrownBy(() -> settlementSessionValidator.validate(null, ACCOUNT_ID))
        .isInstanceOf(SettlementSessionUnauthorizedException.class);
    assertThatThrownBy(() -> settlementSessionValidator.validate("   ", ACCOUNT_ID))
        .isInstanceOf(SettlementSessionUnauthorizedException.class);
  }

  @Test
  void missingSessionKeyFails() {
    assertThatThrownBy(() -> settlementSessionValidator.validate(TOKEN, ACCOUNT_ID))
        .isInstanceOf(SettlementSessionUnauthorizedException.class);
  }

  @Test
  void expiredSessionFails() {
    putSession(TOKEN, ACCOUNT_ID, pastExpiresAt(), "PLAYER");

    assertThatThrownBy(() -> settlementSessionValidator.validate(TOKEN, ACCOUNT_ID))
        .isInstanceOf(SettlementSessionUnauthorizedException.class);
  }

  @Test
  void accountMismatchFails() {
    putSession(TOKEN, 88L, futureExpiresAt(), "PLAYER");

    assertThatThrownBy(() -> settlementSessionValidator.validate(TOKEN, ACCOUNT_ID))
        .isInstanceOf(SettlementSessionUnauthorizedException.class);
  }

  @Test
  void roleMismatchFails() {
    putSession(TOKEN, ACCOUNT_ID, futureExpiresAt(), "ADMIN");

    assertThatThrownBy(() -> settlementSessionValidator.validate(TOKEN, ACCOUNT_ID))
        .isInstanceOf(SettlementSessionUnauthorizedException.class);
  }

  @Test
  void malformedSessionFieldsFail() {
    redisTemplate
        .opsForHash()
        .putAll(
            sessionKey("malformed-account"),
            Map.of(
                "accountId", "not-a-number",
                "expiresAt", futureExpiresAt(),
                "role", "PLAYER"));
    redisTemplate
        .opsForHash()
        .putAll(
            sessionKey("malformed-expiry"),
            Map.of(
                "accountId", Long.toString(ACCOUNT_ID),
                "expiresAt", "soon",
                "role", "PLAYER"));

    assertThatThrownBy(
            () -> settlementSessionValidator.validate("malformed-account", ACCOUNT_ID))
        .isInstanceOf(SettlementSessionUnauthorizedException.class);
    assertThatThrownBy(
            () -> settlementSessionValidator.validate("malformed-expiry", ACCOUNT_ID))
        .isInstanceOf(SettlementSessionUnauthorizedException.class);
  }

  private void putSession(String token, long accountId, String expiresAt, String role) {
    redisTemplate
        .opsForHash()
        .putAll(
            sessionKey(token),
            Map.of(
                "accountId", Long.toString(accountId),
                "expiresAt", expiresAt,
                "role", role));
  }

  private String sessionKey(String token) {
    return "session:" + token;
  }

  private String futureExpiresAt() {
    return Long.toString(System.currentTimeMillis() + 60_000L);
  }

  private String pastExpiresAt() {
    return Long.toString(System.currentTimeMillis() - 60_000L);
  }
}
