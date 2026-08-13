package com.fasd92.lootoflegends.meta.gameaccess;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HashSet;
import java.util.Set;
import org.junit.jupiter.api.Test;

class GameCredentialContractTest {
  private static final ObjectMapper JSON = new ObjectMapper();
  private static final Path REPOSITORY_ROOT = Path.of("..");

  @Test
  void schemaFreezesCredentialAndHttpStatusSemantics() throws Exception {
    Path schemaPath = REPOSITORY_ROOT.resolve("contracts/meta-api/game-credential-v1.schema.json");
    JsonNode schema = JSON.readTree(Files.readString(schemaPath));

    assertEquals("1.0.0", schema.path("version").asText());
    assertEquals(32, schema.path("credentialPolicy").path("entropyBytes").asInt());
    assertEquals(30, schema.path("credentialPolicy").path("ttlSeconds").asInt());
    assertEquals("loot-game-server-v1", schema.path("credentialPolicy").path("audience").asText());
    assertEquals(
        Set.of("200", "400", "401", "409", "410", "422", "503"),
        fieldNames(schema.path("endpoints").path("claim").path("responses")));
  }

  @Test
  void goldenExamplesContainOnlySyntheticIdentity() throws Exception {
    Path goldenPath = REPOSITORY_ROOT.resolve("contracts/protocol/golden/session-auth-v1.json");
    JsonNode golden = JSON.readTree(Files.readString(goldenPath));

    assertEquals("1.0.0", golden.path("contractVersion").asText());
    assertTrue(golden.path("syntheticOnly").asBoolean());
    assertEquals(
        43,
        golden
            .path("metaApi")
            .path("issueSuccess")
            .path("response")
            .path("credential")
            .asText()
            .length());
  }

  private static Set<String> fieldNames(JsonNode node) {
    var names = new HashSet<String>();
    node.fieldNames().forEachRemaining(names::add);
    return names;
  }
}
