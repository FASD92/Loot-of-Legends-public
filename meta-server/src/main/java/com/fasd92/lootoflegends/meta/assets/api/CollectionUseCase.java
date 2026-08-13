package com.fasd92.lootoflegends.meta.assets.api;

import com.fasd92.lootoflegends.meta.identity.api.AccountId;
import java.util.List;

public interface CollectionUseCase {
  CollectionSnapshot collection(AccountId accountId);

  record CollectionSnapshot(List<Item> items, String wallet, long pendingSettlementCount) {
    public CollectionSnapshot {
      items = List.copyOf(items);
    }
  }

  record Item(String itemId, String quantity, String value) {}

  final class DependencyUnavailable extends RuntimeException {
    public DependencyUnavailable(Throwable cause) {
      super(cause);
    }
  }
}
