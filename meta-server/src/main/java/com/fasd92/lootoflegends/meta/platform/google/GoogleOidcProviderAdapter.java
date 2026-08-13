package com.fasd92.lootoflegends.meta.platform.google;

import com.fasd92.lootoflegends.meta.identity.application.ExternalIdentityProvider;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.IOException;
import java.net.URI;
import java.net.URLEncoder;
import java.net.http.HttpClient;
import java.net.http.HttpRequest;
import java.net.http.HttpResponse;
import java.nio.charset.StandardCharsets;
import java.time.Clock;
import java.time.Duration;
import java.util.Objects;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.http.MediaType;
import org.springframework.stereotype.Component;

@Component
public final class GoogleOidcProviderAdapter implements ExternalIdentityProvider {
  private static final Duration HTTP_TIMEOUT = Duration.ofSeconds(10);
  private static final URI AUTHORIZATION_ENDPOINT =
      URI.create("https://accounts.google.com/o/oauth2/v2/auth");
  private static final URI TOKEN_ENDPOINT = URI.create("https://oauth2.googleapis.com/token");
  private static final URI TOKEN_INFO_ENDPOINT =
      URI.create("https://oauth2.googleapis.com/tokeninfo");

  private final ObjectMapper json;
  private final Clock clock;
  private final HttpClient http;
  private final String clientId;
  private final String clientSecret;
  private final URI callbackUri;
  private final URI authorizationEndpoint;
  private final URI tokenEndpoint;
  private final URI tokenInfoEndpoint;

  @Autowired
  public GoogleOidcProviderAdapter(
      ObjectMapper json,
      Clock clock,
      @Value("${loot.identity.google.client-id:}") String clientId,
      @Value("${loot.identity.google.client-secret:}") String clientSecret,
      @Value("${loot.identity.google.callback-uri:}") String callbackUri) {
    this(
        json,
        clock,
        HttpClient.newBuilder()
            .connectTimeout(HTTP_TIMEOUT)
            .followRedirects(HttpClient.Redirect.NEVER)
            .build(),
        clientId,
        clientSecret,
        URI.create(callbackUri),
        AUTHORIZATION_ENDPOINT,
        TOKEN_ENDPOINT,
        TOKEN_INFO_ENDPOINT);
  }

  GoogleOidcProviderAdapter(
      ObjectMapper json,
      Clock clock,
      HttpClient http,
      String clientId,
      String clientSecret,
      URI callbackUri,
      URI authorizationEndpoint,
      URI tokenEndpoint,
      URI tokenInfoEndpoint) {
    this.json = Objects.requireNonNull(json, "json");
    this.clock = Objects.requireNonNull(clock, "clock");
    this.http = Objects.requireNonNull(http, "http");
    this.clientId = Objects.requireNonNull(clientId, "clientId");
    this.clientSecret = Objects.requireNonNull(clientSecret, "clientSecret");
    this.callbackUri = Objects.requireNonNull(callbackUri, "callbackUri");
    this.authorizationEndpoint =
        Objects.requireNonNull(authorizationEndpoint, "authorizationEndpoint");
    this.tokenEndpoint = Objects.requireNonNull(tokenEndpoint, "tokenEndpoint");
    this.tokenInfoEndpoint = Objects.requireNonNull(tokenInfoEndpoint, "tokenInfoEndpoint");
  }

  @Override
  public URI authorizationUri(String state, String codeChallenge) {
    requireConfiguration();
    return URI.create(
        authorizationEndpoint
            + "?client_id="
            + encode(clientId)
            + "&redirect_uri="
            + encode(callbackUri.toString())
            + "&response_type=code&scope="
            + encode("openid email")
            + "&state="
            + encode(state)
            + "&code_challenge="
            + encode(codeChallenge)
            + "&code_challenge_method=S256");
  }

  @Override
  public ExternalIdentity exchange(String authorizationCode, String codeVerifier) {
    requireConfiguration();
    String tokenResponse =
        postForm(
            tokenEndpoint,
            "code="
                + encode(authorizationCode)
                + "&client_id="
                + encode(clientId)
                + "&client_secret="
                + encode(clientSecret)
                + "&redirect_uri="
                + encode(callbackUri.toString())
                + "&grant_type=authorization_code&code_verifier="
                + encode(codeVerifier));
    JsonNode tokenJson = parse(tokenResponse);
    String idToken = requiredText(tokenJson, "id_token");

    String tokenInfoResponse = get(tokenInfoEndpoint + "?id_token=" + encode(idToken));
    JsonNode tokenInfo = parse(tokenInfoResponse);
    String issuer = requiredText(tokenInfo, "iss");
    String subject = requiredText(tokenInfo, "sub");
    String email = requiredText(tokenInfo, "email");
    if (!("https://accounts.google.com".equals(issuer) || "accounts.google.com".equals(issuer))
        || !hasAudience(tokenInfo.path("aud"), clientId)
        || tokenInfo.path("exp").asLong(0) <= clock.instant().getEpochSecond()
        || !tokenInfo.path("email_verified").asBoolean(false)) {
      throw rejected("Google OIDC assertion validation failed");
    }
    return new ExternalIdentity(issuer, subject, email);
  }

  private String postForm(URI uri, String body) {
    HttpRequest request =
        HttpRequest.newBuilder(uri)
            .timeout(HTTP_TIMEOUT)
            .header("Content-Type", MediaType.APPLICATION_FORM_URLENCODED_VALUE)
            .POST(HttpRequest.BodyPublishers.ofString(body))
            .build();
    return send(request);
  }

  private String get(String uri) {
    HttpRequest request =
        HttpRequest.newBuilder(URI.create(uri)).timeout(HTTP_TIMEOUT).GET().build();
    return send(request);
  }

  private String send(HttpRequest request) {
    try {
      HttpResponse<String> response =
          http.send(request, HttpResponse.BodyHandlers.ofString(StandardCharsets.UTF_8));
      if (response.statusCode() >= 500) {
        throw new ExternalIdentityProvider.Unavailable(
            new IllegalStateException("Google OIDC dependency unavailable"));
      }
      if (response.statusCode() < 200 || response.statusCode() >= 300) {
        throw rejected("Google OIDC request rejected");
      }
      return response.body();
    } catch (InterruptedException exception) {
      Thread.currentThread().interrupt();
      throw new ExternalIdentityProvider.Unavailable(exception);
    } catch (IOException exception) {
      throw new ExternalIdentityProvider.Unavailable(exception);
    }
  }

  private JsonNode parse(String body) {
    try {
      return json.readTree(body);
    } catch (IOException exception) {
      throw rejected("Google OIDC response was malformed", exception);
    }
  }

  private void requireConfiguration() {
    if (clientId.isBlank()
        || clientSecret.isBlank()
        || !callbackUri.isAbsolute()
        || !"https".equals(callbackUri.getScheme())
        || !"/v1/identity/google/callback".equals(callbackUri.getPath())
        || callbackUri.getQuery() != null
        || callbackUri.getFragment() != null) {
      throw new ExternalIdentityProvider.Unavailable(
          new IllegalStateException("Google OIDC configuration is incomplete"));
    }
  }

  private static boolean hasAudience(JsonNode audience, String expected) {
    if (audience.isTextual()) {
      return expected.equals(audience.asText());
    }
    if (audience.isArray()) {
      for (JsonNode entry : audience) {
        if (expected.equals(entry.asText())) {
          return true;
        }
      }
    }
    return false;
  }

  private static String requiredText(JsonNode object, String field) {
    String value = object.path(field).asText();
    if (value.isBlank()) {
      throw rejected("Google OIDC response is missing " + field);
    }
    return value;
  }

  private static ExternalIdentityProvider.Rejected rejected(String message) {
    return rejected(message, null);
  }

  private static ExternalIdentityProvider.Rejected rejected(String message, Throwable cause) {
    return new ExternalIdentityProvider.Rejected(new IllegalArgumentException(message, cause));
  }

  private static String encode(String value) {
    return URLEncoder.encode(value, StandardCharsets.UTF_8);
  }
}
