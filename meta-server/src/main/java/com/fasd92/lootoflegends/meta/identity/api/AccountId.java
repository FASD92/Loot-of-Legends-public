package com.fasd92.lootoflegends.meta.identity.api;

import java.nio.ByteBuffer;
import java.util.Objects;
import java.util.UUID;

public record AccountId(UUID value) {
  public AccountId {
    Objects.requireNonNull(value, "value");
  }

  public byte[] bytes() {
    return ByteBuffer.allocate(16)
        .putLong(value.getMostSignificantBits())
        .putLong(value.getLeastSignificantBits())
        .array();
  }

  public static AccountId fromBytes(byte[] bytes) {
    Objects.requireNonNull(bytes, "bytes");
    if (bytes.length != 16) {
      throw new IllegalArgumentException("account identity must be 16 bytes");
    }
    ByteBuffer buffer = ByteBuffer.wrap(bytes);
    return new AccountId(new UUID(buffer.getLong(), buffer.getLong()));
  }
}
