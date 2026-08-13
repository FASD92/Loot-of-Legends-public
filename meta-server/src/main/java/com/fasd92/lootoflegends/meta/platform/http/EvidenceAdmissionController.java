package com.fasd92.lootoflegends.meta.platform.http;

import com.fasd92.lootoflegends.meta.identity.api.EvidenceAdmissionUseCase;
import com.fasd92.lootoflegends.meta.identity.api.IssueEvidenceSessionCommand;
import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.util.regex.Pattern;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.boot.autoconfigure.condition.ConditionalOnProperty;
import org.springframework.http.CacheControl;
import org.springframework.http.HttpHeaders;
import org.springframework.http.HttpStatus;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.http.converter.HttpMessageNotReadableException;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestHeader;
import org.springframework.web.bind.annotation.RestController;

@RestController
@ConditionalOnProperty(prefix = "loot.evidence", name = "enabled", havingValue = "true")
public final class EvidenceAdmissionController {
  private static final Pattern RUN_ID = Pattern.compile("[A-Za-z0-9][A-Za-z0-9._-]{0,63}");

  private final EvidenceAdmissionUseCase useCase;
  private final Clock clock;
  private final PrivateBearerToken token;
  private final int participantMaximum;
  private final Duration sessionTtl;
  private final Instant environmentExpiresAt;

  public EvidenceAdmissionController(
      EvidenceAdmissionUseCase useCase,
      Clock clock,
      @Value("${loot.evidence.admission-token}") String admissionToken,
      @Value("${loot.evidence.participant-maximum}") int participantMaximum,
      @Value("${loot.evidence.session-ttl}") Duration sessionTtl,
      @Value("${loot.evidence.environment-expires-at}") Instant environmentExpiresAt) {
    if (participantMaximum <= 0 || sessionTtl.isNegative() || sessionTtl.isZero()) {
      throw new IllegalArgumentException("invalid evidence admission configuration");
    }
    this.useCase = useCase;
    this.clock = clock;
    this.token = new PrivateBearerToken(admissionToken, "loot.evidence.admission-token");
    this.participantMaximum = participantMaximum;
    this.sessionTtl = sessionTtl;
    this.environmentExpiresAt = environmentExpiresAt;
  }

  @PostMapping(
      path = "/private/evidence/v1/meta-sessions",
      consumes = MediaType.APPLICATION_JSON_VALUE,
      produces = MediaType.APPLICATION_JSON_VALUE)
  ResponseEntity<?> issue(
      @RequestHeader(value = HttpHeaders.AUTHORIZATION, required = false) String authorization,
      @RequestBody Request request) {
    if (!token.matches(authorization)) {
      return error(HttpStatus.UNAUTHORIZED, "UNAUTHORIZED");
    }
    if (request.runId() == null
        || !RUN_ID.matcher(request.runId()).matches()
        || request.participantIndex() <= 0
        || request.participantIndex() > participantMaximum) {
      throw new IllegalArgumentException("invalid evidence admission request");
    }

    Instant expiresAt = clock.instant().plus(sessionTtl);
    if (environmentExpiresAt.isBefore(expiresAt)) {
      expiresAt = environmentExpiresAt;
    }
    var issued =
        useCase.issueEvidenceSession(
            new IssueEvidenceSessionCommand(
                request.runId(), request.participantIndex(), expiresAt));
    return ResponseEntity.status(HttpStatus.CREATED)
        .cacheControl(CacheControl.noStore())
        .body(new Response(issued.metaSession(), issued.expiresAt()));
  }

  @ExceptionHandler(EvidenceAdmissionUseCase.Rejected.class)
  ResponseEntity<ErrorResponse> rejected(EvidenceAdmissionUseCase.Rejected rejected) {
    return switch (rejected.reason()) {
      case REPLAYED -> error(HttpStatus.CONFLICT, "REPLAYED");
      case DEPENDENCY_UNAVAILABLE ->
          error(HttpStatus.SERVICE_UNAVAILABLE, "DEPENDENCY_UNAVAILABLE");
    };
  }

  @ExceptionHandler({IllegalArgumentException.class, HttpMessageNotReadableException.class})
  ResponseEntity<ErrorResponse> invalid() {
    return error(HttpStatus.BAD_REQUEST, "INVALID");
  }

  private static ResponseEntity<ErrorResponse> error(HttpStatus status, String code) {
    return ResponseEntity.status(status)
        .cacheControl(CacheControl.noStore())
        .body(new ErrorResponse(code));
  }

  public record Request(String runId, int participantIndex) {}

  public record Response(String metaSession, Instant expiresAt) {}

  public record ErrorResponse(String code) {}
}
