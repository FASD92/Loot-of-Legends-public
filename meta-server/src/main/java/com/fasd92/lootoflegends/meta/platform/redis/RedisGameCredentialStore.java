package com.fasd92.lootoflegends.meta.platform.redis;

import com.fasd92.lootoflegends.meta.gameaccess.api.ClaimGameCredentialResult;
import com.fasd92.lootoflegends.meta.gameaccess.api.ClaimRejectionReason;
import com.fasd92.lootoflegends.meta.gameaccess.application.GameCredentialStore;
import com.fasd92.lootoflegends.meta.identity.api.AccountId;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import org.springframework.dao.DataAccessException;
import org.springframework.data.redis.core.StringRedisTemplate;
import org.springframework.data.redis.core.script.DefaultRedisScript;
import org.springframework.stereotype.Component;

@Component
public final class RedisGameCredentialStore implements GameCredentialStore {
  private static final String KEY_PREFIX = "game-credential:v1:";
  private static final DefaultRedisScript<Long> ISSUE =
      new DefaultRedisScript<>(
          """
          if redis.call('EXISTS', KEYS[1]) == 1 then
            return 0
          end
          redis.call('HSET', KEYS[1],
            'state', 'ISSUED',
            'accountId', ARGV[1],
            'nickname', ARGV[2],
            'audience', ARGV[3],
            'expiresAtEpochMillis', ARGV[4])
          redis.call('PEXPIREAT', KEYS[1], ARGV[5])
          return 1
          """,
          Long.class);
  private static final DefaultRedisScript<String> CLAIM =
      new DefaultRedisScript<>(
          """
          local state = redis.call('HGET', KEYS[1], 'state')
          if not state then
            return 'INVALID'
          end
          if state == 'CONSUMED' then
            return 'ALREADY_CONSUMED'
          end
          if state == 'EXPIRED' then
            return 'EXPIRED'
          end
          local expiresAt = tonumber(redis.call('HGET', KEYS[1], 'expiresAtEpochMillis'))
          local now = tonumber(ARGV[2])
          if now >= expiresAt then
            redis.call('HSET', KEYS[1], 'state', 'EXPIRED')
            return 'EXPIRED'
          end
          if redis.call('HGET', KEYS[1], 'audience') ~= ARGV[1] then
            return 'WRONG_AUDIENCE'
          end
          redis.call('HSET', KEYS[1], 'state', 'CONSUMED')
          return 'CLAIMED'
          """,
          String.class);

  private final StringRedisTemplate redis;

  public RedisGameCredentialStore(StringRedisTemplate redis) {
    this.redis = redis;
  }

  @Override
  public boolean putIfAbsent(
      String hash,
      AccountId accountId,
      String nickname,
      String audience,
      Instant expiresAt,
      Instant purgeAt) {
    try {
      Long result =
          redis.execute(
              ISSUE,
              List.of(key(hash)),
              accountId.value().toString(),
              nickname,
              audience,
              Long.toString(expiresAt.toEpochMilli()),
              Long.toString(purgeAt.toEpochMilli()));
      return Long.valueOf(1L).equals(result);
    } catch (DataAccessException exception) {
      throw new GameCredentialStore.Unavailable(exception);
    }
  }

  @Override
  public ClaimGameCredentialResult claim(String hash, String audience, Instant now) {
    try {
      String redisKey = key(hash);
      String outcome =
          redis.execute(CLAIM, List.of(redisKey), audience, Long.toString(now.toEpochMilli()));
      if ("CLAIMED".equals(outcome)) {
        Map<Object, Object> values = redis.opsForHash().entries(redisKey);
        return new ClaimGameCredentialResult.Claimed(
            new AccountId(UUID.fromString(required(values, "accountId"))),
            required(values, "nickname"));
      }
      return new ClaimGameCredentialResult.Rejected(reason(outcome));
    } catch (DataAccessException | IllegalArgumentException exception) {
      throw new GameCredentialStore.Unavailable(exception);
    }
  }

  private static ClaimRejectionReason reason(String value) {
    try {
      return ClaimRejectionReason.valueOf(value);
    } catch (RuntimeException invalidStoreResult) {
      throw new GameCredentialStore.Unavailable(invalidStoreResult);
    }
  }

  private static String required(Map<Object, Object> values, String field) {
    Object value = values.get(field);
    if (value == null) {
      throw new IllegalArgumentException("credential record is missing " + field);
    }
    return value.toString();
  }

  private static String key(String hash) {
    return KEY_PREFIX + hash;
  }
}
