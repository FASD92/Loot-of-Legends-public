package com.fasd92.lootoflegends.meta.assets.application;

import com.fasd92.lootoflegends.meta.assets.api.AssetApplyUseCase;
import java.math.BigInteger;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Propagation;
import org.springframework.transaction.annotation.Transactional;

@Service
public class AssetApplyApplication implements AssetApplyUseCase {
  private static final BigInteger MAX_UINT64 =
      BigInteger.ONE.shiftLeft(64).subtract(BigInteger.ONE);

  private final AssetStore assets;

  public AssetApplyApplication(AssetStore assets) {
    this.assets = assets;
  }

  @Override
  @Transactional(propagation = Propagation.MANDATORY)
  public void validate(AssetDelta delta) {
    validatedMutations(delta);
  }

  @Override
  @Transactional(propagation = Propagation.MANDATORY)
  public void apply(AssetDelta delta) {
    List<AssetStore.InventoryMutation> mutations = validatedMutations(delta);
    try {
      assets.apply(delta.accountId(), mutations, delta.finalAssetValue());
    } catch (AssetStore.Unavailable unavailable) {
      throw new DependencyUnavailable(unavailable);
    }
  }

  private List<AssetStore.InventoryMutation> validatedMutations(AssetDelta delta) {
    try {
      Map<BigInteger, BigInteger> catalog =
          assets.catalog(delta.catalogVersion()).orElseThrow(Invalid::new).unitValues();
      List<AssetStore.InventoryMutation> mutations = new ArrayList<>(delta.itemDeltas().size());
      BigInteger calculatedValue = BigInteger.ZERO;
      for (ItemDelta item : delta.itemDeltas()) {
        BigInteger unitValue = catalog.get(item.itemId());
        if (unitValue == null) {
          throw new Invalid();
        }
        calculatedValue = calculatedValue.add(item.quantity().multiply(unitValue));
        if (calculatedValue.compareTo(MAX_UINT64) > 0) {
          throw new Invalid();
        }
        mutations.add(new AssetStore.InventoryMutation(item.itemId(), item.quantity(), unitValue));
      }
      if (!calculatedValue.equals(delta.finalAssetValue())) {
        throw new Invalid();
      }
      return mutations;
    } catch (AssetStore.Unavailable unavailable) {
      throw new DependencyUnavailable(unavailable);
    }
  }
}
