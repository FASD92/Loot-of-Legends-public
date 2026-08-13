package com.fasd92.lootoflegends.meta.assets.api;

import com.fasd92.lootoflegends.meta.identity.api.AccountId;
import java.math.BigInteger;
import java.util.List;

public interface AssetApplyUseCase {
  void validate(AssetDelta delta);

  void apply(AssetDelta delta);

  record AssetDelta(
      AccountId accountId,
      int catalogVersion,
      List<ItemDelta> itemDeltas,
      BigInteger finalAssetValue) {
    public AssetDelta {
      itemDeltas = List.copyOf(itemDeltas);
    }
  }

  record ItemDelta(BigInteger itemId, BigInteger quantity) {}

  final class Invalid extends RuntimeException {}

  final class DependencyUnavailable extends RuntimeException {
    public DependencyUnavailable(Throwable cause) {
      super(cause);
    }
  }
}
