package com.lol.meta.settlement;

import com.lol.meta.settlement.api.SettlementRequest;
import com.lol.meta.settlement.api.SettlementResponse;
import jakarta.validation.Valid;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestHeader;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

@RestController
@RequestMapping("/internal/settlements")
public final class SettlementController {

  private final SettlementProcessingGuard settlementProcessingGuard;
  private final SettlementService settlementService;
  private final SettlementSessionValidator settlementSessionValidator;

  public SettlementController(
      SettlementProcessingGuard settlementProcessingGuard,
      SettlementService settlementService,
      SettlementSessionValidator settlementSessionValidator) {
    this.settlementProcessingGuard = settlementProcessingGuard;
    this.settlementService = settlementService;
    this.settlementSessionValidator = settlementSessionValidator;
  }

  @PostMapping
  public ResponseEntity<SettlementResponse> applySettlement(
      @RequestHeader(value = SettlementSessionValidator.HEADER_NAME, required = false)
          String sessionToken,
      @Valid @RequestBody SettlementRequest request) {
    settlementSessionValidator.validate(sessionToken, request.accountId());
    SettlementProcessingGuard.Lease lease =
        settlementProcessingGuard
            .acquire(request.settlementId())
            .orElseThrow(() -> new SettlementProcessingConflictException(request.settlementId()));
    try {
      return ResponseEntity.ok(settlementService.apply(request));
    } finally {
      settlementProcessingGuard.release(lease);
    }
  }
}
