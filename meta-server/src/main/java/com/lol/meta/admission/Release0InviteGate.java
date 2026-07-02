package com.lol.meta.admission;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.stereotype.Component;

@Component
public final class Release0InviteGate {

  private final boolean enabled;
  private final String inviteCode;

  public Release0InviteGate(
      @Value("${release0.invite.enabled:false}") boolean enabled,
      @Value("${release0.invite.code:}") String inviteCode) {
    this.enabled = enabled;
    this.inviteCode = normalize(inviteCode);
  }

  static Release0InviteGate forTest(boolean enabled, String inviteCode) {
    return new Release0InviteGate(enabled, inviteCode);
  }

  public boolean accepts(String submittedInviteCode) {
    if (!enabled) {
      return true;
    }
    if (inviteCode.isEmpty()) {
      return false;
    }

    byte[] expected = inviteCode.getBytes(StandardCharsets.UTF_8);
    byte[] actual = normalize(submittedInviteCode).getBytes(StandardCharsets.UTF_8);
    return MessageDigest.isEqual(expected, actual);
  }

  private static String normalize(String value) {
    return value == null ? "" : value.trim();
  }
}
