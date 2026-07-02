package com.lol.meta.firewall;

import static org.assertj.core.api.Assertions.assertThat;

import org.junit.jupiter.api.Test;
import org.springframework.mock.web.MockHttpServletRequest;

class TrustedClientAddressResolverTests {

  @Test
  void trustsConfiguredForwardedHeaderWhenEnabled() {
    TrustedClientAddressResolver resolver =
        new TrustedClientAddressResolver(properties(true, true));
    MockHttpServletRequest request = request("10.0.0.5");
    request.addHeader("X-Release0-Client-IP", "203.0.113.21");
    request.addHeader("X-Forwarded-For", "198.51.100.99");

    assertThat(resolver.resolve(request)).contains("203.0.113.21");
  }

  @Test
  void ignoresForwardedHeaderAndUsesRemoteAddressWhenTrustIsDisabled() {
    TrustedClientAddressResolver resolver =
        new TrustedClientAddressResolver(properties(true, false));
    MockHttpServletRequest request = request("198.51.100.23");
    request.addHeader("X-Release0-Client-IP", "10.0.0.8");

    assertThat(resolver.resolve(request)).contains("198.51.100.23");
  }

  @Test
  void fallsBackToRemoteAddressWhenTrustedHeaderIsAbsent() {
    TrustedClientAddressResolver resolver =
        new TrustedClientAddressResolver(properties(true, true));
    MockHttpServletRequest request = request("198.51.100.24");

    assertThat(resolver.resolve(request)).contains("198.51.100.24");
  }

  @Test
  void rejectsUnsafeForwardedHeaderValues() {
    TrustedClientAddressResolver resolver =
        new TrustedClientAddressResolver(properties(true, true));

    for (String value :
        new String[] {
          "",
          "203.0.113.1, 203.0.113.2",
          "203.0.113.1/32",
          "::1",
          "not-an-ip",
        }) {
      MockHttpServletRequest request = request("198.51.100.25");
      request.addHeader("X-Release0-Client-IP", value);

      assertThat(resolver.resolve(request)).as(value).isEmpty();
    }
  }

  @Test
  void rejectsInvalidRemoteAddress() {
    TrustedClientAddressResolver resolver =
        new TrustedClientAddressResolver(properties(true, false));

    assertThat(resolver.resolve(request("::1"))).isEmpty();
    assertThat(resolver.resolve(request("198.51.100.1/32"))).isEmpty();
    assertThat(resolver.resolve(request("not-an-ip"))).isEmpty();
  }

  private static MockHttpServletRequest request(String remoteAddress) {
    MockHttpServletRequest request = new MockHttpServletRequest();
    request.setRemoteAddr(remoteAddress);
    return request;
  }

  private static GameFirewallProperties properties(boolean enabled, boolean trustHeader) {
    return new GameFirewallProperties(
        enabled,
        "/run/loot-of-legends/firewall-agent.sock",
        60,
        600,
        "X-Release0-Client-IP",
        trustHeader);
  }
}
