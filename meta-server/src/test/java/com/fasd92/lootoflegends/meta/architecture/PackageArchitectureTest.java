package com.fasd92.lootoflegends.meta.architecture;

import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Pattern;
import org.junit.jupiter.api.Test;

class PackageArchitectureTest {
  private static final Path MAIN_SOURCE = Path.of("src/main/java");
  private static final Pattern IDENTITY_INTERNAL_IMPORT =
      Pattern.compile(
          "(?m)^\\s*import\\s+com\\.fasd92\\.lootoflegends\\.meta\\.identity\\.(domain|application|internal)(\\.|;)");
  private static final Pattern SPRING_REFERENCE =
      Pattern.compile("(?m)^\\s*import\\s+org\\.springframework\\.|@org\\.springframework\\.");

  @Test
  void productionSourcesRespectPackageBoundaries() throws IOException {
    List<String> violations = new ArrayList<>();
    try (var paths = Files.walk(MAIN_SOURCE)) {
      for (Path path : paths.filter(source -> source.toString().endsWith(".java")).toList()) {
        violations.addAll(findViolations(path, Files.readString(path)));
      }
    }

    assertTrue(violations.isEmpty(), String.join(System.lineSeparator(), violations));
  }

  @Test
  void rulesRejectForbiddenExamples() {
    Path gameAccess = MAIN_SOURCE.resolve("example/gameaccess/BadImport.java");
    Path domain = MAIN_SOURCE.resolve("example/gameaccess/domain/SpringDomain.java");

    assertTrue(
        findViolations(gameAccess, "import com.fasd92.lootoflegends.meta.identity.domain.Account;")
            .stream()
            .anyMatch(message -> message.contains("identity internal")));
    assertTrue(
        findViolations(domain, "import org.springframework.stereotype.Component;").stream()
            .anyMatch(message -> message.contains("Spring reference")));
  }

  private static List<String> findViolations(Path path, String source) {
    String normalizedPath = path.toString().replace('\\', '/');
    List<String> violations = new ArrayList<>();

    if (normalizedPath.contains("/gameaccess/")
        && IDENTITY_INTERNAL_IMPORT.matcher(source).find()) {
      violations.add(path + ": gameaccess imports identity internal package");
    }
    if (normalizedPath.contains("/domain/") && SPRING_REFERENCE.matcher(source).find()) {
      violations.add(path + ": domain contains a Spring reference");
    }
    if (source.contains("DevAuthController") || source.contains("/dev-auth")) {
      violations.add(path + ": release source contains a dev-auth route");
    }

    return violations;
  }
}
