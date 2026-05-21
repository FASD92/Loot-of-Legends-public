package com.lol.meta.settlement;

import static org.assertj.core.api.Assertions.assertThat;

import com.lol.meta.support.AbstractContainerIntegrationTest;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.data.redis.core.RedisCallback;
import org.springframework.data.redis.core.StringRedisTemplate;

class SettlementProcessingGuardTests extends AbstractContainerIntegrationTest {

  private static final String SETTLEMENT_ID = "room-42-session-1001-finished-0001";

  @Autowired private SettlementProcessingGuard settlementProcessingGuard;
  @Autowired private StringRedisTemplate redisTemplate;

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
  void acquireFailsWhileSameSettlementIsAlreadyGuarded() {
    SettlementProcessingGuard.Lease lease =
        settlementProcessingGuard.acquire(SETTLEMENT_ID).orElseThrow();

    assertThat(settlementProcessingGuard.acquire(SETTLEMENT_ID)).isEmpty();

    settlementProcessingGuard.release(lease);
  }

  @Test
  void releaseAllowsSettlementToBeGuardedAgain() {
    SettlementProcessingGuard.Lease lease =
        settlementProcessingGuard.acquire(SETTLEMENT_ID).orElseThrow();
    settlementProcessingGuard.release(lease);

    assertThat(settlementProcessingGuard.acquire(SETTLEMENT_ID)).isPresent();
  }

  @Test
  void releaseWithDifferentOwnerDoesNotDeleteExistingGuard() {
    SettlementProcessingGuard.Lease lease =
        settlementProcessingGuard.acquire(SETTLEMENT_ID).orElseThrow();
    settlementProcessingGuard.release(
        new SettlementProcessingGuard.Lease(SETTLEMENT_ID, "different-owner"));

    assertThat(settlementProcessingGuard.acquire(SETTLEMENT_ID)).isEmpty();

    settlementProcessingGuard.release(lease);
  }
}
