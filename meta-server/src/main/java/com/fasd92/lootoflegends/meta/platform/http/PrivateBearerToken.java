package com.fasd92.lootoflegends.meta.platform.http;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;

final class PrivateBearerToken {
  private static final String PREFIX = "Bearer ";
  private final byte[] expected;

  PrivateBearerToken(String token, String propertyName) {
    if (token == null
        || token.length() < 16
        || token.length() > 512
        || token.chars().anyMatch(value -> value < 0x21 || value > 0x7e)) {
      throw new IllegalArgumentException(propertyName + " must be configured");
    }
    expected = digest(token);
  }

  boolean matches(String authorization) {
    return authorization != null
        && authorization.startsWith(PREFIX)
        && MessageDigest.isEqual(expected, digest(authorization.substring(PREFIX.length())));
  }

  private static byte[] digest(String value) {
    try {
      return MessageDigest.getInstance("SHA-256").digest(value.getBytes(StandardCharsets.UTF_8));
    } catch (NoSuchAlgorithmException error) {
      throw new IllegalStateException("SHA-256 unavailable", error);
    }
  }
}
