package com.lol.meta.auth;

import org.springframework.security.oauth2.core.OAuth2AuthenticationException;
import org.springframework.security.oauth2.core.OAuth2Error;
import org.springframework.security.oauth2.core.user.OAuth2User;

final class Release0OAuthAccountAttributes {

  private static final String GOOGLE_SUBJECT_ATTRIBUTE = "sub";

  private Release0OAuthAccountAttributes() {}

  static String requireGoogleSubject(OAuth2User user) {
    Object subject = user.getAttributes().get(GOOGLE_SUBJECT_ATTRIBUTE);
    if (subject instanceof String value && !value.isBlank()) {
      return value;
    }
    throw new OAuth2AuthenticationException(
        new OAuth2Error("release0_missing_google_subject"), "Google OAuth subject is missing");
  }

  static String nameAttributeKey(String configuredNameAttributeKey) {
    if (configuredNameAttributeKey == null || configuredNameAttributeKey.isBlank()) {
      return GOOGLE_SUBJECT_ATTRIBUTE;
    }
    return configuredNameAttributeKey;
  }
}
