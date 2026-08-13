package com.fasd92.lootoflegends.meta.platform.observability;

import com.fasd92.lootoflegends.meta.settlement.application.SettlementMetrics;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.atomic.AtomicLong;
import org.springframework.stereotype.Component;

@Component
public final class MetaMetrics implements SettlementMetrics {
  private final AtomicLong applied = new AtomicLong();
  private final AtomicLong retries = new AtomicLong();
  private final AtomicLong conflicts = new AtomicLong();
  private final AtomicLong applyLatencyMillis = new AtomicLong();

  @Override
  public void recordApplied(Duration latency) {
    if (latency.isNegative()) {
      throw new IllegalArgumentException("latency must not be negative");
    }
    applied.incrementAndGet();
    applyLatencyMillis.set(latency.toMillis());
  }

  @Override
  public void recordRetry() {
    retries.incrementAndGet();
  }

  @Override
  public void recordConflict() {
    conflicts.incrementAndGet();
  }

  public List<Metric> snapshot() {
    return List.of(
        new Metric("settlement_apply_total", applied.get()),
        new Metric("settlement_retry_total", retries.get()),
        new Metric("settlement_conflict_total", conflicts.get()),
        new Metric("settlement_apply_latency_ms", applyLatencyMillis.get()));
  }

  public record Metric(String name, long value) {}
}
