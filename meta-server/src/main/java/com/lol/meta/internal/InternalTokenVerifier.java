package com.lol.meta.internal;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import org.springframework.stereotype.Component;
import org.springframework.util.StringUtils;

@Component
public final class InternalTokenVerifier {

  private final byte[] expectedToken;

  public InternalTokenVerifier(InternalApiProperties properties) {
    this.expectedToken = properties.token().getBytes(StandardCharsets.UTF_8);
  }

  public boolean matches(String candidate) {
    if (!StringUtils.hasText(candidate)) {
      return false;
    }
    return MessageDigest.isEqual(expectedToken, candidate.getBytes(StandardCharsets.UTF_8));
  }
}
