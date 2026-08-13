package com.fasd92.lootoflegends.meta.assets.application;

import com.fasd92.lootoflegends.meta.identity.api.AccountId;
import java.math.BigInteger;
import java.util.List;
import java.util.Map;
import java.util.Optional;

public interface AssetStore {
  AssetSnapshot snapshot(AccountId accountId);

  Optional<CatalogSnapshot> catalog(int catalogVersion);

  void apply(AccountId accountId, List<InventoryMutation> items, BigInteger walletDelta);

  record AssetSnapshot(List<InventoryItem> items, BigInteger wallet) {
    public AssetSnapshot {
      items = List.copyOf(items);
    }
  }

  record InventoryItem(BigInteger itemId, BigInteger quantity, BigInteger unitValue) {}

  record CatalogSnapshot(Map<BigInteger, BigInteger> unitValues) {
    public CatalogSnapshot {
      unitValues = Map.copyOf(unitValues);
    }
  }

  record InventoryMutation(BigInteger itemId, BigInteger quantity, BigInteger unitValue) {}

  final class Unavailable extends RuntimeException {
    public Unavailable(Throwable cause) {
      super(cause);
    }
  }
}
