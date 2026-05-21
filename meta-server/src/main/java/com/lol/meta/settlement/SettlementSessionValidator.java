package com.lol.meta.settlement;

import java.util.Map;
import org.springframework.data.redis.core.StringRedisTemplate;
import org.springframework.stereotype.Component;

@Component
public final class SettlementSessionValidator {

  public static final String HEADER_NAME = "X-Session-Token";

  private static final String ACCOUNT_ID_FIELD = "accountId";
  private static final String EXPIRES_AT_FIELD = "expiresAt";
  private static final String REQUIRED_ROLE = "PLAYER";
  private static final String ROLE_FIELD = "role";
  private static final String SESSION_KEY_PREFIX = "session:";

  private final StringRedisTemplate redisTemplate;

  public SettlementSessionValidator(StringRedisTemplate redisTemplate) {
    this.redisTemplate = redisTemplate;
  }

  public void validate(String token, long requestAccountId) {
    if (token == null || token.isBlank()) {
      throw new SettlementSessionUnauthorizedException();
    }

    Map<Object, Object> session =
        redisTemplate.opsForHash().entries(SESSION_KEY_PREFIX + token.trim());
    if (session.isEmpty()) {
      throw new SettlementSessionUnauthorizedException();
    }

    long sessionAccountId = parseLongField(session.get(ACCOUNT_ID_FIELD));
    long expiresAt = parseLongField(session.get(EXPIRES_AT_FIELD));
    String role = stringField(session.get(ROLE_FIELD));

    if (sessionAccountId != requestAccountId) {
      throw new SettlementSessionUnauthorizedException();
    }
    if (expiresAt <= System.currentTimeMillis()) {
      throw new SettlementSessionUnauthorizedException();
    }
    if (!REQUIRED_ROLE.equals(role)) {
      throw new SettlementSessionUnauthorizedException();
    }
  }

  private static long parseLongField(Object value) {
    try {
      return Long.parseLong(stringField(value));
    } catch (NumberFormatException exception) {
      throw new SettlementSessionUnauthorizedException();
    }
  }

  private static String stringField(Object value) {
    if (value == null) {
      throw new SettlementSessionUnauthorizedException();
    }
    return value.toString();
  }
}
