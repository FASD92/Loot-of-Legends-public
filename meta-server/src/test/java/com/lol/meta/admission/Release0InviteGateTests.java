package com.lol.meta.admission;

import static org.assertj.core.api.Assertions.assertThat;

import org.junit.jupiter.api.Test;

class Release0InviteGateTests {

  @Test
  void disabledGateAcceptsMissingInviteCode() {
    Release0InviteGate gate = Release0InviteGate.forTest(false, "");

    assertThat(gate.accepts(null)).isTrue();
    assertThat(gate.accepts("")).isTrue();
  }

  @Test
  void enabledGateAcceptsOnlyConfiguredInviteCode() {
    Release0InviteGate gate = Release0InviteGate.forTest(true, "portfolio-2026");

    assertThat(gate.accepts("portfolio-2026")).isTrue();
    assertThat(gate.accepts(" portfolio-2026 ")).isTrue();
    assertThat(gate.accepts("wrong-code")).isFalse();
    assertThat(gate.accepts("")).isFalse();
    assertThat(gate.accepts(null)).isFalse();
  }

  @Test
  void enabledGateRejectsWhenConfiguredCodeIsBlank() {
    Release0InviteGate gate = Release0InviteGate.forTest(true, "   ");

    assertThat(gate.accepts("portfolio-2026")).isFalse();
  }
}
