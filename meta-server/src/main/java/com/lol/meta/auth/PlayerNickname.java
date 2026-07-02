package com.lol.meta.auth;

import java.nio.charset.StandardCharsets;

public record PlayerNickname(String value) {

  private static final int MIN_VISIBLE_CHARACTERS = 2;
  private static final int MAX_VISIBLE_CHARACTERS = 12;
  private static final int MAX_UTF8_BYTES = 32;

  public PlayerNickname {
    validate(value);
  }

  public static PlayerNickname parse(String value) {
    return new PlayerNickname(value);
  }

  public enum FailureReason {
    MISSING,
    LENGTH,
    UTF8_BYTE_LIMIT,
    UNSUPPORTED_CHARACTERS
  }

  public static final class InvalidNicknameException extends IllegalArgumentException {

    private final FailureReason reason;

    private InvalidNicknameException(FailureReason reason, String message) {
      super(message);
      this.reason = reason;
    }

    public FailureReason reason() {
      return reason;
    }
  }

  private static void validate(String value) {
    if (value == null || value.isBlank()) {
      throw invalid(FailureReason.MISSING, "nickname must not be blank");
    }

    value
        .codePoints()
        .filter(codePoint -> !isAllowedCodePoint(codePoint))
        .findAny()
        .ifPresent(
            codePoint -> {
              throw invalid(
                  FailureReason.UNSUPPORTED_CHARACTERS,
                  "nickname contains unsupported characters");
            });

    int visibleCharacters = value.codePointCount(0, value.length());
    if (visibleCharacters < MIN_VISIBLE_CHARACTERS
        || visibleCharacters > MAX_VISIBLE_CHARACTERS) {
      throw invalid(FailureReason.LENGTH, "nickname must be 2-12 characters");
    }

    if (value.getBytes(StandardCharsets.UTF_8).length > MAX_UTF8_BYTES) {
      throw invalid(FailureReason.UTF8_BYTE_LIMIT, "nickname exceeds UTF-8 byte limit");
    }
  }

  private static InvalidNicknameException invalid(FailureReason reason, String message) {
    return new InvalidNicknameException(reason, message);
  }

  private static boolean isAllowedCodePoint(int codePoint) {
    return isAsciiLetter(codePoint)
        || isAsciiDigit(codePoint)
        || (codePoint >= 0xAC00 && codePoint <= 0xD7A3);
  }

  private static boolean isAsciiLetter(int codePoint) {
    return (codePoint >= 'A' && codePoint <= 'Z') || (codePoint >= 'a' && codePoint <= 'z');
  }

  private static boolean isAsciiDigit(int codePoint) {
    return codePoint >= '0' && codePoint <= '9';
  }
}
