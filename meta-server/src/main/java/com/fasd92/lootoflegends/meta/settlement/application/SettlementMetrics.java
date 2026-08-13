package com.fasd92.lootoflegends.meta.settlement.application;

import java.time.Duration;

public interface SettlementMetrics {
  void recordApplied(Duration latency);

  void recordRetry();

  void recordConflict();
}
