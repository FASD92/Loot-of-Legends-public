package com.fasd92.lootoflegends.meta.platform.http;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

final class PrivateBearerTokenTest {
  @Test
  void acceptsOnlyBoundedVisibleAsciiAndMatchesTheBearerValue() {
    var token = new PrivateBearerToken("m".repeat(43), "token");

    assertTrue(token.matches("Bearer " + "m".repeat(43)));
    assertFalse(token.matches("Bearer " + "n".repeat(43)));
    assertFalse(token.matches(null));
    assertThrows(
        IllegalArgumentException.class, () -> new PrivateBearerToken("x".repeat(15), "token"));
    assertThrows(
        IllegalArgumentException.class, () -> new PrivateBearerToken("x".repeat(513), "token"));
    assertThrows(
        IllegalArgumentException.class,
        () -> new PrivateBearerToken("x".repeat(42) + "\n", "token"));
    assertThrows(
        IllegalArgumentException.class,
        () -> new PrivateBearerToken("x".repeat(42) + "한", "token"));
  }
}
