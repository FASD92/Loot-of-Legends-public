package com.fasd92.lootoflegends.meta.platform.http;

import com.fasd92.lootoflegends.meta.settlement.api.SettlementUseCase;
import com.fasd92.lootoflegends.meta.settlement.api.SettlementUseCase.Accepted;
import com.fasd92.lootoflegends.meta.settlement.api.SettlementUseCase.Settlement;
import com.fasd92.lootoflegends.meta.settlement.api.SettlementUseCase.Submission;
import org.springframework.http.HttpStatus;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.http.converter.HttpMessageNotReadableException;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RestController;

@RestController
public final class SettlementController {
  private final SettlementUseCase useCase;

  public SettlementController(SettlementUseCase useCase) {
    this.useCase = useCase;
  }

  @PostMapping(
      path = "/internal/v1/settlements",
      consumes = MediaType.APPLICATION_JSON_VALUE,
      produces = MediaType.APPLICATION_JSON_VALUE)
  ResponseEntity<?> accept(@RequestBody SubmissionRequest request) {
    SettlementUseCase.Acceptance acceptance =
        useCase.accept(
            new Submission(
                request.settlementId(), request.payloadHash(), request.canonicalPayload()));
    if (acceptance instanceof SettlementUseCase.Conflict) {
      return error(HttpStatus.CONFLICT, "CONFLICT");
    }
    return ResponseEntity.ok(response(((Accepted) acceptance).settlement()));
  }

  @GetMapping(
      path = "/internal/v1/settlements/{settlementId}",
      produces = MediaType.APPLICATION_JSON_VALUE)
  ResponseEntity<?> find(@PathVariable String settlementId) {
    return useCase
        .find(settlementId)
        .<ResponseEntity<?>>map(settlement -> ResponseEntity.ok(response(settlement)))
        .orElseGet(() -> error(HttpStatus.NOT_FOUND, "NOT_FOUND"));
  }

  @ExceptionHandler({SettlementUseCase.Invalid.class, HttpMessageNotReadableException.class})
  ResponseEntity<ErrorResponse> invalid() {
    return error(HttpStatus.BAD_REQUEST, "INVALID");
  }

  @ExceptionHandler(SettlementUseCase.DependencyUnavailable.class)
  ResponseEntity<ErrorResponse> dependencyUnavailable() {
    return error(HttpStatus.SERVICE_UNAVAILABLE, "DEPENDENCY_UNAVAILABLE");
  }

  private static SettlementResponse response(Settlement settlement) {
    return new SettlementResponse(settlement.settlementId(), settlement.status().wireValue());
  }

  private static ResponseEntity<ErrorResponse> error(HttpStatus status, String code) {
    return ResponseEntity.status(status).body(new ErrorResponse(code));
  }

  public record SubmissionRequest(
      String settlementId, String payloadHash, String canonicalPayload) {}

  public record SettlementResponse(String settlementId, String status) {}

  public record ErrorResponse(String code) {}
}
