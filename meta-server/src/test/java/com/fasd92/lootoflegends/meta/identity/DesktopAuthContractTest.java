package com.fasd92.lootoflegends.meta.identity;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Files;
import java.nio.file.Path;
import org.junit.jupiter.api.Test;

class DesktopAuthContractTest {
  private static final ObjectMapper JSON = new ObjectMapper();
  private static final Path SCHEMA =
      Path.of("..").resolve("contracts/meta-api/desktop-auth-handoff-v1.schema.json");

  @Test
  void schemaFreezesGoogleOidcPkceAndLoopbackHandoff() throws Exception {
    JsonNode schema = JSON.readTree(Files.readString(SCHEMA));

    assertEquals("1.0.0", schema.path("version").asText());
    assertEquals("GOOGLE_OIDC", schema.path("provider").path("name").asText());
    assertEquals(
        "/v1/identity/google/callback", schema.path("provider").path("callbackPath").asText());
    assertEquals("S256", schema.path("pkcePolicy").path("method").asText());
    assertEquals(120, schema.path("handoffPolicy").path("ttlSeconds").asInt());
    assertTrue(schema.path("handoffPolicy").path("singleUse").asBoolean());
    assertEquals("127.0.0.1", schema.path("loopbackPolicy").path("host").asText());
    assertEquals(49152, schema.path("loopbackPolicy").path("minimumPort").asInt());
    assertEquals(65535, schema.path("loopbackPolicy").path("maximumPort").asInt());
    assertEquals(
        "/api/v1/desktop-auth/attempts",
        schema.path("endpoints").path("start").path("path").asText());
    assertEquals(
        "/api/v1/desktop-auth/exchanges",
        schema.path("endpoints").path("exchange").path("path").asText());
  }

  @Test
  void unityContractContainsNoRawProviderTokenField() throws Exception {
    String schema = Files.readString(SCHEMA);

    assertFalse(schema.contains("accessToken"));
    assertFalse(schema.contains("refreshToken"));
    assertFalse(schema.contains("idToken"));
    assertTrue(schema.contains("metaSession"));
  }
}
