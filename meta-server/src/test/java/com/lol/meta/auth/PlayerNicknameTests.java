package com.lol.meta.auth;

import static org.assertj.core.api.Assertions.assertThat;
import static org.assertj.core.api.Assertions.assertThatThrownBy;

import org.junit.jupiter.api.Test;

class PlayerNicknameTests {

  @Test
  void acceptsAsciiDigitsAndCompleteHangul() {
    assertThat(PlayerNickname.parse("player123").value()).isEqualTo("player123");
    assertThat(PlayerNickname.parse("Player123").value()).isEqualTo("Player123");
    assertThat(PlayerNickname.parse("가나다12").value()).isEqualTo("가나다12");
  }

  @Test
  void rejectsSpacesSpecialCharactersJamoAndTooLongNames() {
    assertThatThrownBy(() -> PlayerNickname.parse("player 1"))
        .isInstanceOf(IllegalArgumentException.class)
        .hasMessageContaining("nickname contains unsupported characters");
    assertThatThrownBy(() -> PlayerNickname.parse("player_1"))
        .isInstanceOf(IllegalArgumentException.class)
        .hasMessageContaining("nickname contains unsupported characters");
    assertThatThrownBy(() -> PlayerNickname.parse("ㄱ"))
        .isInstanceOf(IllegalArgumentException.class)
        .hasMessageContaining("nickname contains unsupported characters");
    assertThatThrownBy(() -> PlayerNickname.parse("abcdefghijklmnopqrstu"))
        .isInstanceOf(IllegalArgumentException.class)
        .hasMessageContaining("nickname must be 2-12 characters");
  }

  @Test
  void comparisonIsCaseSensitive() {
    assertThat(PlayerNickname.parse("player").value())
        .isNotEqualTo(PlayerNickname.parse("Player").value());
  }
}
