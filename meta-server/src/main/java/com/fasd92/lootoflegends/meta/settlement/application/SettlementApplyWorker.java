package com.fasd92.lootoflegends.meta.settlement.application;

import com.fasd92.lootoflegends.meta.settlement.api.SettlementApplyUseCase;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.boot.autoconfigure.condition.ConditionalOnProperty;
import org.springframework.scheduling.annotation.Scheduled;
import org.springframework.stereotype.Component;

@Component
@ConditionalOnProperty(
    name = "loot.settlement.apply-enabled",
    havingValue = "true",
    matchIfMissing = true)
public final class SettlementApplyWorker {
  private static final Logger LOG = LoggerFactory.getLogger(SettlementApplyWorker.class);

  private final SettlementApplyUseCase useCase;

  public SettlementApplyWorker(SettlementApplyUseCase useCase) {
    this.useCase = useCase;
  }

  @Scheduled(
      initialDelayString = "${loot.settlement.apply-initial-delay-ms:0}",
      fixedDelayString = "${loot.settlement.apply-delay-ms:250}")
  public void applyNext() {
    try {
      useCase.applyNext();
    } catch (SettlementApplyUseCase.Retryable retryable) {
      LOG.warn("settlement apply outcome=RETRYABLE");
    } catch (SettlementApplyUseCase.Invalid invalid) {
      LOG.error("settlement apply outcome=INVALID");
    }
  }
}
