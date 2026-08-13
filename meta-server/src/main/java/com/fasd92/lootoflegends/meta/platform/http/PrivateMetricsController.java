package com.fasd92.lootoflegends.meta.platform.http;

import com.fasd92.lootoflegends.meta.platform.observability.MetaMetrics;
import java.time.Clock;
import java.util.List;
import java.util.regex.Pattern;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.boot.autoconfigure.condition.ConditionalOnProperty;
import org.springframework.http.CacheControl;
import org.springframework.http.HttpHeaders;
import org.springframework.http.HttpStatus;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RequestHeader;
import org.springframework.web.bind.annotation.RestController;

@RestController
@ConditionalOnProperty(prefix = "loot.metrics", name = "enabled", havingValue = "true")
public final class PrivateMetricsController {
  private static final Pattern SHA256 = Pattern.compile("[0-9a-f]{64}");

  private final MetaMetrics metrics;
  private final Clock clock;
  private final PrivateBearerToken token;
  private final String sourceIdentityDigest;

  public PrivateMetricsController(
      MetaMetrics metrics,
      Clock clock,
      @Value("${loot.metrics.read-token}") String readToken,
      @Value("${loot.metrics.source-identity-digest}") String sourceIdentityDigest) {
    if (!SHA256.matcher(sourceIdentityDigest).matches()) {
      throw new IllegalArgumentException("loot.metrics.source-identity-digest must be sha256");
    }
    this.metrics = metrics;
    this.clock = clock;
    this.token = new PrivateBearerToken(readToken, "loot.metrics.read-token");
    this.sourceIdentityDigest = sourceIdentityDigest;
  }

  @GetMapping(path = "/private/metrics/v1", produces = MediaType.APPLICATION_JSON_VALUE)
  ResponseEntity<?> read(
      @RequestHeader(value = HttpHeaders.AUTHORIZATION, required = false) String authorization) {
    if (!token.matches(authorization)) {
      return ResponseEntity.status(HttpStatus.UNAUTHORIZED)
          .cacheControl(CacheControl.noStore())
          .body(new ErrorResponse("UNAUTHORIZED"));
    }
    return ResponseEntity.ok()
        .cacheControl(CacheControl.noStore())
        .body(
            new Response(
                1,
                "meta",
                sourceIdentityDigest,
                clock.instant().toEpochMilli(),
                metrics.snapshot()));
  }

  public record Response(
      int schemaVersion,
      String source,
      String sourceIdentityDigest,
      long capturedAtUnixMillis,
      List<MetaMetrics.Metric> metrics) {}

  public record ErrorResponse(String code) {}
}
