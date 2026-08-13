package com.fasd92.lootoflegends.meta.settlement.domain;

import com.fasd92.lootoflegends.meta.identity.api.AccountId;
import java.math.BigInteger;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

public final class SettlementPayload {
  private static final byte[] PAYLOAD_HEADER =
      "settlement-intent-v1".getBytes(StandardCharsets.US_ASCII);
  private static final byte[] ID_HEADER = "settlement-v1".getBytes(StandardCharsets.US_ASCII);
  private static final int SETTLEMENT_ID_OFFSET = 20;
  private static final int ACCOUNT_ID_OFFSET = 36;
  private static final int ROOM_ID_OFFSET = 52;
  private static final int BATTLE_ID_OFFSET = 60;
  private static final int OUTCOME_OFFSET = 68;
  private static final int EXIT_STATUS_OFFSET = 69;
  private static final int CATALOG_VERSION_OFFSET = 70;
  private static final int ITEM_COUNT_OFFSET = 72;
  private static final int ITEMS_OFFSET = 74;
  private static final int FIXED_PAYLOAD_BYTES = 98;
  private static final int ITEM_DELTA_BYTES = 16;

  private final byte[] settlementId;
  private final AccountId accountId;
  private final byte[] payloadHash;
  private final byte[] canonicalPayload;
  private final int catalogVersion;
  private final List<ItemDelta> itemDeltas;
  private final BigInteger finalAssetValue;

  private SettlementPayload(
      byte[] settlementId,
      AccountId accountId,
      byte[] payloadHash,
      byte[] canonicalPayload,
      int catalogVersion,
      List<ItemDelta> itemDeltas,
      BigInteger finalAssetValue) {
    this.settlementId = settlementId.clone();
    this.accountId = accountId;
    this.payloadHash = payloadHash.clone();
    this.canonicalPayload = canonicalPayload.clone();
    this.catalogVersion = catalogVersion;
    this.itemDeltas = List.copyOf(itemDeltas);
    this.finalAssetValue = finalAssetValue;
  }

  public static SettlementPayload parse(
      byte[] settlementId, byte[] payloadHash, byte[] canonicalPayload) {
    if (settlementId == null
        || settlementId.length != 16
        || payloadHash == null
        || payloadHash.length != 32
        || canonicalPayload == null
        || canonicalPayload.length < FIXED_PAYLOAD_BYTES
        || !Arrays.equals(PAYLOAD_HEADER, Arrays.copyOf(canonicalPayload, PAYLOAD_HEADER.length))) {
      throw new Invalid();
    }
    int itemCount = unsigned16(canonicalPayload, ITEM_COUNT_OFFSET);
    if (canonicalPayload.length != FIXED_PAYLOAD_BYTES + ITEM_DELTA_BYTES * itemCount
        || !Arrays.equals(
            settlementId,
            Arrays.copyOfRange(canonicalPayload, SETTLEMENT_ID_OFFSET, ACCOUNT_ID_OFFSET))
        || !MessageDigest.isEqual(payloadHash, sha256(canonicalPayload))) {
      throw new Invalid();
    }

    byte[] accountBytes = Arrays.copyOfRange(canonicalPayload, ACCOUNT_ID_OFFSET, ROOM_ID_OFFSET);
    if (!Arrays.equals(settlementId, derivedId(canonicalPayload, accountBytes))) {
      throw new Invalid();
    }
    if (unsigned64(canonicalPayload, ROOM_ID_OFFSET).signum() == 0
        || unsigned64(canonicalPayload, BATTLE_ID_OFFSET).signum() == 0) {
      throw new Invalid();
    }

    int outcome = Byte.toUnsignedInt(canonicalPayload[OUTCOME_OFFSET]);
    int exitStatus = Byte.toUnsignedInt(canonicalPayload[EXIT_STATUS_OFFSET]);
    int catalogVersion = unsigned16(canonicalPayload, CATALOG_VERSION_OFFSET);
    if (outcome < 1
        || outcome > 3
        || exitStatus < 1
        || exitStatus > 2
        || catalogVersion == 0
        || (outcome == 3 && exitStatus != 2)) {
      throw new Invalid();
    }

    List<ItemDelta> itemDeltas = new ArrayList<>(itemCount);
    BigInteger previousItemId = BigInteger.ZERO;
    int offset = ITEMS_OFFSET;
    for (int index = 0; index < itemCount; index++) {
      BigInteger itemId = unsigned64(canonicalPayload, offset);
      BigInteger quantity = unsigned64(canonicalPayload, offset + 8);
      if (itemId.compareTo(previousItemId) <= 0 || quantity.signum() == 0) {
        throw new Invalid();
      }
      itemDeltas.add(new ItemDelta(itemId, quantity));
      previousItemId = itemId;
      offset += ITEM_DELTA_BYTES;
    }
    BigInteger finalAssetValue = unsigned64(canonicalPayload, offset);
    if (outcome == 2 && (!itemDeltas.isEmpty() || finalAssetValue.signum() != 0)) {
      throw new Invalid();
    }
    return new SettlementPayload(
        settlementId,
        AccountId.fromBytes(accountBytes),
        payloadHash,
        canonicalPayload,
        catalogVersion,
        itemDeltas,
        finalAssetValue);
  }

  public byte[] settlementId() {
    return settlementId.clone();
  }

  public AccountId accountId() {
    return accountId;
  }

  public byte[] payloadHash() {
    return payloadHash.clone();
  }

  public byte[] canonicalPayload() {
    return canonicalPayload.clone();
  }

  public int catalogVersion() {
    return catalogVersion;
  }

  public List<ItemDelta> itemDeltas() {
    return itemDeltas;
  }

  public BigInteger finalAssetValue() {
    return finalAssetValue;
  }

  private static byte[] derivedId(byte[] payload, byte[] accountId) {
    MessageDigest digest = digest();
    digest.update(ID_HEADER);
    digest.update(payload, ROOM_ID_OFFSET, 8);
    digest.update(payload, BATTLE_ID_OFFSET, 8);
    digest.update(accountId);
    return Arrays.copyOf(digest.digest(), 16);
  }

  private static int unsigned16(byte[] bytes, int offset) {
    return (Byte.toUnsignedInt(bytes[offset]) << 8) | Byte.toUnsignedInt(bytes[offset + 1]);
  }

  private static BigInteger unsigned64(byte[] bytes, int offset) {
    return new BigInteger(1, Arrays.copyOfRange(bytes, offset, offset + 8));
  }

  private static byte[] sha256(byte[] value) {
    return digest().digest(value);
  }

  private static MessageDigest digest() {
    try {
      return MessageDigest.getInstance("SHA-256");
    } catch (NoSuchAlgorithmException impossible) {
      throw new IllegalStateException("SHA-256 is unavailable", impossible);
    }
  }

  public record ItemDelta(BigInteger itemId, BigInteger quantity) {}

  public static final class Invalid extends RuntimeException {}
}
