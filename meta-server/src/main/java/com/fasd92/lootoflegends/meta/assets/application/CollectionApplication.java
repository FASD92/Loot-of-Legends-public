package com.fasd92.lootoflegends.meta.assets.application;

import com.fasd92.lootoflegends.meta.assets.api.CollectionUseCase;
import com.fasd92.lootoflegends.meta.identity.api.AccountId;
import com.fasd92.lootoflegends.meta.settlement.api.SettlementPendingQuery;
import java.util.List;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Transactional;

@Service
public class CollectionApplication implements CollectionUseCase {
  private final AssetStore assets;
  private final SettlementPendingQuery settlements;

  public CollectionApplication(AssetStore assets, SettlementPendingQuery settlements) {
    this.assets = assets;
    this.settlements = settlements;
  }

  @Override
  @Transactional(readOnly = true)
  public CollectionSnapshot collection(AccountId accountId) {
    try {
      AssetStore.AssetSnapshot snapshot = assets.snapshot(accountId);
      List<Item> items =
          snapshot.items().stream()
              .map(
                  item ->
                      new Item(
                          item.itemId().toString(),
                          item.quantity().toString(),
                          item.quantity().multiply(item.unitValue()).toString()))
              .toList();
      return new CollectionSnapshot(
          items, snapshot.wallet().toString(), settlements.countPending(accountId));
    } catch (AssetStore.Unavailable | SettlementPendingQuery.Unavailable unavailable) {
      throw new DependencyUnavailable(unavailable);
    }
  }
}
