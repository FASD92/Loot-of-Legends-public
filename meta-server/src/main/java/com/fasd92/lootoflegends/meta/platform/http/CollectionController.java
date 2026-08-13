package com.fasd92.lootoflegends.meta.platform.http;

import com.fasd92.lootoflegends.meta.assets.api.CollectionUseCase;
import com.fasd92.lootoflegends.meta.identity.api.AuthenticatedPrincipal;
import java.util.List;
import org.springframework.http.HttpStatus;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.security.core.Authentication;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RestController;

@RestController
public final class CollectionController {
  private final CollectionUseCase useCase;

  public CollectionController(CollectionUseCase useCase) {
    this.useCase = useCase;
  }

  @GetMapping(path = "/api/v1/collection", produces = MediaType.APPLICATION_JSON_VALUE)
  ResponseEntity<?> collection(Authentication authentication) {
    if (!(authentication.getPrincipal() instanceof AuthenticatedPrincipal principal)) {
      return error(HttpStatus.UNAUTHORIZED, "UNAUTHORIZED");
    }
    CollectionUseCase.CollectionSnapshot snapshot = useCase.collection(principal.accountId());
    List<ItemResponse> items =
        snapshot.items().stream()
            .map(item -> new ItemResponse(item.itemId(), item.quantity(), item.value()))
            .toList();
    return ResponseEntity.ok(
        new CollectionResponse(
            items, snapshot.wallet(), snapshot.pendingSettlementCount(), "Fresh"));
  }

  @ExceptionHandler(CollectionUseCase.DependencyUnavailable.class)
  ResponseEntity<ErrorResponse> dependencyUnavailable() {
    return error(HttpStatus.SERVICE_UNAVAILABLE, "DEPENDENCY_UNAVAILABLE");
  }

  private static ResponseEntity<ErrorResponse> error(HttpStatus status, String code) {
    return ResponseEntity.status(status).body(new ErrorResponse(code));
  }

  public record ItemResponse(String itemId, String quantity, String value) {}

  public record CollectionResponse(
      List<ItemResponse> items, String wallet, long pendingSettlementCount, String freshness) {}

  public record ErrorResponse(String code) {}
}
