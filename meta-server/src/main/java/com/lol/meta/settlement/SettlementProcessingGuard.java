package com.lol.meta.settlement;

import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.UUID;
import org.springframework.data.redis.core.StringRedisTemplate;
import org.springframework.data.redis.core.script.RedisScript;
import org.springframework.stereotype.Component;

@Component
public final class SettlementProcessingGuard {

  private static final Duration GUARD_TTL = Duration.ofSeconds(5);
  private static final String KEY_PREFIX = "settlement:processing:";
  private static final RedisScript<Long> RELEASE_SCRIPT =
      RedisScript.of(
          """
          if redis.call('get', KEYS[1]) == ARGV[1] then
            return redis.call('del', KEYS[1])
          end
          return 0
          """,
          Long.class);

  private final StringRedisTemplate redisTemplate;

  public SettlementProcessingGuard(StringRedisTemplate redisTemplate) {
    this.redisTemplate = redisTemplate;
  }

  public Optional<Lease> acquire(String settlementId) {
    String owner = UUID.randomUUID().toString();
    String key = key(settlementId);
    Boolean acquired = redisTemplate.opsForValue().setIfAbsent(key, owner, GUARD_TTL);
    if (!Boolean.TRUE.equals(acquired)) {
      return Optional.empty();
    }
    return Optional.of(new Lease(settlementId, owner));
  }

  public void release(Lease lease) {
    redisTemplate.execute(RELEASE_SCRIPT, List.of(key(lease.settlementId())), lease.owner());
  }

  private static String key(String settlementId) {
    return KEY_PREFIX + settlementId;
  }

  public record Lease(String settlementId, String owner) {}
}
