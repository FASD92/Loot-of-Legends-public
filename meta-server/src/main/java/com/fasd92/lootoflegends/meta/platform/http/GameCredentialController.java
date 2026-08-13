package com.fasd92.lootoflegends.meta.platform.http;

import com.fasd92.lootoflegends.meta.gameaccess.api.ClaimGameCredentialCommand;
import com.fasd92.lootoflegends.meta.gameaccess.api.ClaimGameCredentialResult;
import com.fasd92.lootoflegends.meta.gameaccess.api.ClaimRejectionReason;
import com.fasd92.lootoflegends.meta.gameaccess.api.GameCredentialUseCase;
import com.fasd92.lootoflegends.meta.gameaccess.api.IssueGameCredentialCommand;
import com.fasd92.lootoflegends.meta.gameaccess.api.IssuedGameCredential;
import com.fasd92.lootoflegends.meta.identity.api.AuthenticatedPrincipal;
import java.time.Instant;
import org.springframework.http.HttpStatus;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.http.converter.HttpMessageNotReadableException;
import org.springframework.security.core.Authentication;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RestController;

@RestController
public final class GameCredentialController {
  private final GameCredentialUseCase useCase;

  public GameCredentialController(GameCredentialUseCase useCase) {
    this.useCase = useCase;
  }

  @PostMapping(path = "/api/v1/game-credentials", produces = MediaType.APPLICATION_JSON_VALUE)
  ResponseEntity<?> issue(Authentication authentication) {
    if (!(authentication.getPrincipal() instanceof AuthenticatedPrincipal principal)) {
      return error(HttpStatus.UNAUTHORIZED, "UNAUTHORIZED");
    }
    IssuedGameCredential issued =
        useCase.issue(new IssueGameCredentialCommand(principal.accountId(), principal.nickname()));
    return ResponseEntity.status(HttpStatus.CREATED)
        .body(new IssueResponse(issued.credential(), issued.expiresAt()));
  }

  @PostMapping(
      path = "/internal/v1/game-credentials/claim",
      consumes = MediaType.APPLICATION_JSON_VALUE,
      produces = MediaType.APPLICATION_JSON_VALUE)
  ResponseEntity<?> claim(@RequestBody ClaimRequest request) {
    if (request.credential() == null
        || request.audience() == null
        || request.audience().isEmpty()
        || request.audience().length() > 64) {
      return error(HttpStatus.BAD_REQUEST, ClaimRejectionReason.INVALID.name());
    }
    ClaimGameCredentialResult result =
        useCase.claim(new ClaimGameCredentialCommand(request.credential(), request.audience()));
    if (result instanceof ClaimGameCredentialResult.Claimed claimed) {
      return ResponseEntity.ok(
          new ClaimedResponse(claimed.accountId().value().toString(), claimed.nickname()));
    }
    ClaimRejectionReason reason = ((ClaimGameCredentialResult.Rejected) result).reason();
    return error(status(reason), reason.name());
  }

  @ExceptionHandler(GameCredentialUseCase.DependencyUnavailable.class)
  ResponseEntity<ErrorResponse> dependencyUnavailable() {
    return error(
        HttpStatus.SERVICE_UNAVAILABLE, ClaimRejectionReason.DEPENDENCY_UNAVAILABLE.name());
  }

  @ExceptionHandler(HttpMessageNotReadableException.class)
  ResponseEntity<ErrorResponse> invalidBody() {
    return error(HttpStatus.BAD_REQUEST, ClaimRejectionReason.INVALID.name());
  }

  private static HttpStatus status(ClaimRejectionReason reason) {
    return switch (reason) {
      case INVALID -> HttpStatus.BAD_REQUEST;
      case ALREADY_CONSUMED -> HttpStatus.CONFLICT;
      case EXPIRED -> HttpStatus.GONE;
      case WRONG_AUDIENCE -> HttpStatus.UNPROCESSABLE_ENTITY;
      case DEPENDENCY_UNAVAILABLE -> HttpStatus.SERVICE_UNAVAILABLE;
    };
  }

  private static ResponseEntity<ErrorResponse> error(HttpStatus status, String code) {
    return ResponseEntity.status(status).body(new ErrorResponse(code));
  }

  public record ClaimRequest(String credential, String audience) {}

  public record IssueResponse(String credential, Instant expiresAt) {}

  public record ClaimedResponse(String accountId, String nickname) {}

  public record ErrorResponse(String code) {}
}
