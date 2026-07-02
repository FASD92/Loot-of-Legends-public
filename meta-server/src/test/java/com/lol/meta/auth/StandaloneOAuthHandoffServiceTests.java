package com.lol.meta.auth;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import java.time.Clock;
import java.time.Instant;
import java.time.ZoneOffset;
import java.util.Map;
import org.junit.jupiter.api.Test;
import org.springframework.test.util.ReflectionTestUtils;

class StandaloneOAuthHandoffServiceTests {

  @Test
  void acceptsOnlyLoopbackCallbackUri() {
    StandaloneOAuthHandoffService service = StandaloneOAuthHandoffService.forTest();

    assertThat(service.isAllowedCallback("http://127.0.0.1:53421/release0/oauth/callback"))
        .isTrue();
    assertThat(service.isAllowedCallback("http://localhost:53421/release0/oauth/callback"))
        .isTrue();
    assertThat(service.isAllowedCallback("https://example.com/callback")).isFalse();
    assertThat(service.isAllowedCallback("http://192.168.0.10:53421/callback")).isFalse();
    assertThat(service.isAllowedCallback("http://127.0.0.1:53421/release0/oauth/callback?a=b"))
        .isFalse();
    assertThat(service.isAllowedCallback("http://127.0.0.1:53421/release0/oauth/callback#done"))
        .isFalse();
  }

  @Test
  void issuedCodeIsOneTime() {
    StandaloneOAuthHandoffService service =
        StandaloneOAuthHandoffService.forTest(
            Clock.fixed(Instant.ofEpochSecond(1000), ZoneOffset.UTC));

    String code = service.issueCode(77L, "state-1");

    assertThat(service.claimCode(code, "state-1").accountId()).isEqualTo(77L);
    assertThatThrownBy(() -> service.claimCode(code, "state-1"))
        .isInstanceOf(StandaloneOAuthHandoffService.InvalidHandoffException.class);
  }

  @Test
  void rejectsExpiredCode() {
    MutableClock clock = new MutableClock(Instant.ofEpochSecond(1000));
    StandaloneOAuthHandoffService service = StandaloneOAuthHandoffService.forTest(clock);
    String code = service.issueCode(77L, "state-1");

    clock.now = Instant.ofEpochSecond(1061);

    assertThatThrownBy(() -> service.claimCode(code, "state-1"))
        .isInstanceOf(StandaloneOAuthHandoffService.InvalidHandoffException.class);
  }

  @Test
  void rejectsStateMismatch() {
    StandaloneOAuthHandoffService service = StandaloneOAuthHandoffService.forTest();
    String code = service.issueCode(77L, "state-1");

    assertThatThrownBy(() -> service.claimCode(code, "state-2"))
        .isInstanceOf(StandaloneOAuthHandoffService.InvalidHandoffException.class);
  }

  @Test
  void prunesExpiredUnclaimedCodesWhenIssuingNewCode() {
    MutableClock clock = new MutableClock(Instant.ofEpochSecond(1000));
    StandaloneOAuthHandoffService service = StandaloneOAuthHandoffService.forTest(clock);
    service.issueCode(77L, "state-1");

    clock.now = Instant.ofEpochSecond(1061);
    service.issueCode(88L, "state-2");

    assertThat(pendingHandoffCount(service)).isEqualTo(1);
  }

  private static int pendingHandoffCount(StandaloneOAuthHandoffService service) {
    Map<?, ?> handoffs = (Map<?, ?>) ReflectionTestUtils.getField(service, "handoffs");
    return handoffs.size();
  }

  private static final class MutableClock extends Clock {
    private Instant now;

    MutableClock(Instant now) {
      this.now = now;
    }

    @Override
    public ZoneOffset getZone() {
      return ZoneOffset.UTC;
    }

    @Override
    public Clock withZone(java.time.ZoneId zone) {
      return this;
    }

    @Override
    public Instant instant() {
      return now;
    }
  }
}
