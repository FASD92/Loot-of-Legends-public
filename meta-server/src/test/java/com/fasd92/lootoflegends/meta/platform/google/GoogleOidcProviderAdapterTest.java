package com.fasd92.lootoflegends.meta.platform.google;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasd92.lootoflegends.meta.identity.application.ExternalIdentityProvider;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.sun.net.httpserver.HttpExchange;
import com.sun.net.httpserver.HttpServer;
import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.URI;
import java.net.URLDecoder;
import java.net.http.HttpClient;
import java.nio.charset.StandardCharsets;
import java.time.Clock;
import java.time.Instant;
import java.time.ZoneOffset;
import java.util.HashMap;
import java.util.Map;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

class GoogleOidcProviderAdapterTest {
  private static final Instant NOW = Instant.parse("2026-08-10T00:00:00Z");
  private static final String CLIENT_ID = "desktop-client-id.apps.googleusercontent.com";
  private static final URI CALLBACK =
      URI.create("https://meta.example.invalid/v1/identity/google/callback");

  private HttpServer server;
  private String tokenRequestBody;
  private String tokenInfoQuery;
  private boolean wrongAudience;

  @BeforeEach
  void startServer() throws IOException {
    server = HttpServer.create(new InetSocketAddress("127.0.0.1", 0), 0);
    server.createContext("/token", this::token);
    server.createContext("/tokeninfo", this::tokenInfo);
    server.start();
  }

  @AfterEach
  void stopServer() {
    server.stop(0);
  }

  @Test
  void authorizationAndExchangeKeepRawProviderTokensInsideMetaAdapter() {
    GoogleOidcProviderAdapter adapter = adapter();

    URI authorization =
        adapter.authorizationUri(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    Map<String, String> authorizationQuery = query(authorization);
    assertEquals(CLIENT_ID, authorizationQuery.get("client_id"));
    assertEquals(CALLBACK.toString(), authorizationQuery.get("redirect_uri"));
    assertEquals("code", authorizationQuery.get("response_type"));
    assertEquals("openid email", authorizationQuery.get("scope"));
    assertEquals("S256", authorizationQuery.get("code_challenge_method"));

    ExternalIdentityProvider.ExternalIdentity identity =
        adapter.exchange(
            "provider-authorization-code", "ccccccccccccccccccccccccccccccccccccccccccc");

    assertEquals("https://accounts.google.com", identity.issuer());
    assertEquals("google-subject", identity.subject());
    assertEquals("player@example.invalid", identity.email());
    assertTrue(tokenRequestBody.contains("code=provider-authorization-code"));
    assertTrue(tokenRequestBody.contains("code_verifier="));
    assertTrue(tokenRequestBody.contains("client_secret=server-only-secret"));
    assertTrue(tokenInfoQuery.contains("id_token=raw-provider-id-token"));
    assertFalse(identity.toString().contains("raw-provider"));
  }

  @Test
  void tokenInfoWithWrongAudienceIsRejected() {
    wrongAudience = true;

    assertThrows(
        ExternalIdentityProvider.Rejected.class,
        () ->
            adapter()
                .exchange(
                    "provider-authorization-code", "ccccccccccccccccccccccccccccccccccccccccccc"));
  }

  private GoogleOidcProviderAdapter adapter() {
    URI base = URI.create("http://127.0.0.1:" + server.getAddress().getPort());
    return new GoogleOidcProviderAdapter(
        new ObjectMapper(),
        Clock.fixed(NOW, ZoneOffset.UTC),
        HttpClient.newHttpClient(),
        CLIENT_ID,
        "server-only-secret",
        CALLBACK,
        URI.create("https://accounts.google.com/o/oauth2/v2/auth"),
        base.resolve("/token"),
        base.resolve("/tokeninfo"));
  }

  private void token(HttpExchange exchange) throws IOException {
    tokenRequestBody = new String(exchange.getRequestBody().readAllBytes(), StandardCharsets.UTF_8);
    respond(
        exchange,
        200,
        """
        {"access_token":"raw-provider-access-token","id_token":"raw-provider-id-token"}
        """);
  }

  private void tokenInfo(HttpExchange exchange) throws IOException {
    tokenInfoQuery = exchange.getRequestURI().getRawQuery();
    respond(
        exchange,
        200,
        """
        {"iss":"https://accounts.google.com","sub":"google-subject","aud":"%s","exp":"%d","email":"player@example.invalid","email_verified":"true"}
        """
            .formatted(
                wrongAudience ? "wrong-client" : CLIENT_ID, NOW.plusSeconds(300).getEpochSecond()));
  }

  private static void respond(HttpExchange exchange, int status, String body) throws IOException {
    byte[] bytes = body.getBytes(StandardCharsets.UTF_8);
    exchange.getResponseHeaders().add("Content-Type", "application/json");
    exchange.sendResponseHeaders(status, bytes.length);
    exchange.getResponseBody().write(bytes);
    exchange.close();
  }

  private static Map<String, String> query(URI uri) {
    var result = new HashMap<String, String>();
    for (String pair : uri.getRawQuery().split("&")) {
      String[] parts = pair.split("=", 2);
      result.put(decode(parts[0]), decode(parts[1]));
    }
    return result;
  }

  private static String decode(String value) {
    return URLDecoder.decode(value, StandardCharsets.UTF_8);
  }
}
