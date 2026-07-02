package com.lol.meta.firewall;

import jakarta.servlet.http.HttpServletRequest;
import java.util.Enumeration;
import java.util.Objects;
import java.util.Optional;

public final class TrustedClientAddressResolver {

  private final GameFirewallProperties properties;

  public TrustedClientAddressResolver(GameFirewallProperties properties) {
    this.properties = Objects.requireNonNull(properties, "properties");
  }

  public Optional<String> resolve(HttpServletRequest request) {
    Objects.requireNonNull(request, "request");
    if (properties.trustForwardedHeader()) {
      Enumeration<String> headers = request.getHeaders(properties.trustedForwardedHeader());
      if (headers.hasMoreElements()) {
        String value = headers.nextElement();
        if (headers.hasMoreElements()) {
          return Optional.empty();
        }
        return canonicalIpv4(value);
      }
    }
    return canonicalIpv4(request.getRemoteAddr());
  }

  private static Optional<String> canonicalIpv4(String value) {
    if (value == null) {
      return Optional.empty();
    }
    String trimmed = value.trim();
    if (trimmed.isEmpty() || trimmed.contains("/") || trimmed.contains(",")) {
      return Optional.empty();
    }
    String[] parts = trimmed.split("\\.", -1);
    if (parts.length != 4) {
      return Optional.empty();
    }

    int[] octets = new int[4];
    for (int index = 0; index < parts.length; index++) {
      String part = parts[index];
      if (part.isEmpty() || part.length() > 3) {
        return Optional.empty();
      }
      for (int charIndex = 0; charIndex < part.length(); charIndex++) {
        if (!Character.isDigit(part.charAt(charIndex))) {
          return Optional.empty();
        }
      }
      int octet = Integer.parseInt(part);
      if (octet > 255) {
        return Optional.empty();
      }
      octets[index] = octet;
    }

    return Optional.of(
        octets[0] + "." + octets[1] + "." + octets[2] + "." + octets[3]);
  }
}
