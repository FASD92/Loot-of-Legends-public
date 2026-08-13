package com.fasd92.lootoflegends.meta.platform.mysql;

import com.fasd92.lootoflegends.meta.assets.application.AssetStore;
import com.fasd92.lootoflegends.meta.identity.api.AccountId;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import org.springframework.dao.DataAccessException;
import org.springframework.jdbc.core.simple.JdbcClient;
import org.springframework.stereotype.Component;

@Component
public final class JdbcAssetStore implements AssetStore {
  private final JdbcClient jdbc;

  public JdbcAssetStore(JdbcClient jdbc) {
    this.jdbc = jdbc;
  }

  @Override
  public AssetSnapshot snapshot(AccountId accountId) {
    try {
      List<InventoryItem> items =
          jdbc.sql(
                  """
                  SELECT item_id, quantity, unit_value
                  FROM account_inventory
                  WHERE account_id = :accountId
                  ORDER BY item_id
                  """)
              .param("accountId", accountId.bytes())
              .query(
                  (result, rowNumber) ->
                      new InventoryItem(
                          integer(result.getBigDecimal("item_id")),
                          integer(result.getBigDecimal("quantity")),
                          integer(result.getBigDecimal("unit_value"))))
              .list();
      BigInteger wallet =
          jdbc.sql("SELECT balance FROM account_wallet WHERE account_id = :accountId")
              .param("accountId", accountId.bytes())
              .query((result, rowNumber) -> integer(result.getBigDecimal("balance")))
              .optional()
              .orElse(BigInteger.ZERO);
      return new AssetSnapshot(items, wallet);
    } catch (DataAccessException | ArithmeticException exception) {
      throw new Unavailable(exception);
    }
  }

  @Override
  public Optional<CatalogSnapshot> catalog(int catalogVersion) {
    try {
      Map<BigInteger, BigInteger> values = new LinkedHashMap<>();
      jdbc.sql(
              """
              SELECT item_id, unit_value
              FROM item_catalog
              WHERE catalog_version = :catalogVersion
              ORDER BY item_id
              """)
          .param("catalogVersion", catalogVersion)
          .query(
              (result, rowNumber) -> {
                values.put(
                    integer(result.getBigDecimal("item_id")),
                    integer(result.getBigDecimal("unit_value")));
                return rowNumber;
              })
          .list();
      return values.isEmpty() ? Optional.empty() : Optional.of(new CatalogSnapshot(values));
    } catch (DataAccessException | ArithmeticException exception) {
      throw new Unavailable(exception);
    }
  }

  @Override
  public void apply(AccountId accountId, List<InventoryMutation> items, BigInteger walletDelta) {
    try {
      for (InventoryMutation item : items) {
        jdbc.sql(
                """
                INSERT INTO account_inventory (account_id, item_id, quantity, unit_value)
                VALUES (:accountId, :itemId, :quantity, :unitValue)
                ON DUPLICATE KEY UPDATE quantity = quantity + VALUES(quantity)
                """)
            .param("accountId", accountId.bytes())
            .param("itemId", new BigDecimal(item.itemId()))
            .param("quantity", new BigDecimal(item.quantity()))
            .param("unitValue", new BigDecimal(item.unitValue()))
            .update();
      }
      if (walletDelta.signum() != 0) {
        jdbc.sql(
                """
                INSERT INTO account_wallet (account_id, balance)
                VALUES (:accountId, :balance)
                ON DUPLICATE KEY UPDATE balance = balance + VALUES(balance)
                """)
            .param("accountId", accountId.bytes())
            .param("balance", new BigDecimal(walletDelta))
            .update();
      }
    } catch (DataAccessException exception) {
      throw new Unavailable(exception);
    }
  }

  private static BigInteger integer(BigDecimal value) {
    return value.toBigIntegerExact();
  }
}
