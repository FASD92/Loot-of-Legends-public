package com.fasd92.lootoflegends.meta.settlement.application;

import com.fasd92.lootoflegends.meta.assets.api.AssetApplyUseCase;
import com.fasd92.lootoflegends.meta.settlement.api.SettlementApplyUseCase;
import com.fasd92.lootoflegends.meta.settlement.domain.SettlementPayload;
import java.time.Duration;
import java.util.Optional;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Transactional;

@Service
public class SettlementApplyApplication implements SettlementApplyUseCase {
  private final SettlementInbox inbox;
  private final AssetApplyUseCase assets;
  private final SettlementMetrics metrics;

  public SettlementApplyApplication(
      SettlementInbox inbox, AssetApplyUseCase assets, SettlementMetrics metrics) {
    this.inbox = inbox;
    this.assets = assets;
    this.metrics = metrics;
  }

  @Override
  @Transactional
  public ApplyResult applyNext() {
    long startedAt = System.nanoTime();
    try {
      Optional<SettlementInbox.StoredSettlement> pending = inbox.findNextPendingForUpdate();
      if (pending.isEmpty()) {
        return ApplyResult.IDLE;
      }
      SettlementInbox.StoredSettlement stored = pending.orElseThrow();
      SettlementPayload payload =
          SettlementPayload.parse(
              stored.settlementId(), stored.payloadHash(), stored.canonicalPayload());
      assets.apply(
          new AssetApplyUseCase.AssetDelta(
              payload.accountId(),
              payload.catalogVersion(),
              payload.itemDeltas().stream()
                  .map(item -> new AssetApplyUseCase.ItemDelta(item.itemId(), item.quantity()))
                  .toList(),
              payload.finalAssetValue()));
      inbox.markApplied(payload.settlementId());
      metrics.recordApplied(Duration.ofNanos(System.nanoTime() - startedAt));
      return ApplyResult.APPLIED;
    } catch (SettlementInbox.Unavailable | AssetApplyUseCase.DependencyUnavailable unavailable) {
      metrics.recordRetry();
      throw new Retryable(unavailable);
    } catch (SettlementPayload.Invalid | AssetApplyUseCase.Invalid invalid) {
      throw new Invalid(invalid);
    }
  }
}
