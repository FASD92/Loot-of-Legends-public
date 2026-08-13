package com.fasd92.lootoflegends.meta.platform.http;

import com.fasd92.lootoflegends.meta.identity.api.CompleteProviderCallbackCommand;
import com.fasd92.lootoflegends.meta.identity.api.DesktopAuthUseCase;
import com.fasd92.lootoflegends.meta.identity.api.ExchangeDesktopAuthCommand;
import com.fasd92.lootoflegends.meta.identity.api.IssuedMetaSession;
import com.fasd92.lootoflegends.meta.identity.api.StartDesktopAuthCommand;
import com.fasd92.lootoflegends.meta.identity.api.StartDesktopAuthResult;
import java.net.URI;
import java.time.Instant;
import org.springframework.http.HttpStatus;
import org.springframework.http.MediaType;
import org.springframework.http.ResponseEntity;
import org.springframework.http.converter.HttpMessageNotReadableException;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;

@RestController
public final class DesktopAuthController {
  private final DesktopAuthUseCase useCase;

  public DesktopAuthController(DesktopAuthUseCase useCase) {
    this.useCase = useCase;
  }

  @PostMapping(
      path = "/api/v1/desktop-auth/attempts",
      consumes = MediaType.APPLICATION_JSON_VALUE,
      produces = MediaType.APPLICATION_JSON_VALUE)
  ResponseEntity<StartResponse> start(@RequestBody StartRequest request) {
    StartDesktopAuthResult result =
        useCase.start(
            new StartDesktopAuthCommand(
                request.loopbackRedirectUri(), request.state(), request.codeChallenge()));
    return ResponseEntity.status(HttpStatus.CREATED)
        .body(new StartResponse(result.authorizationUrl(), result.expiresAt()));
  }

  @GetMapping(path = "/v1/identity/google/callback")
  ResponseEntity<Void> providerCallback(
      @RequestParam("state") String state, @RequestParam("code") String code) {
    URI loopback =
        useCase.completeProviderCallback(new CompleteProviderCallbackCommand(state, code));
    return ResponseEntity.status(HttpStatus.FOUND).location(loopback).build();
  }

  @PostMapping(
      path = "/api/v1/desktop-auth/exchanges",
      consumes = MediaType.APPLICATION_JSON_VALUE,
      produces = MediaType.APPLICATION_JSON_VALUE)
  MetaSessionResponse exchange(@RequestBody ExchangeRequest request) {
    IssuedMetaSession result =
        useCase.exchange(
            new ExchangeDesktopAuthCommand(
                request.handoffCode(), request.state(), request.codeVerifier()));
    return new MetaSessionResponse(result.metaSession(), result.expiresAt());
  }

  @ExceptionHandler(DesktopAuthUseCase.Rejected.class)
  ResponseEntity<ErrorResponse> rejected(DesktopAuthUseCase.Rejected rejected) {
    return switch (rejected.reason()) {
      case EXPIRED_OR_USED -> error(HttpStatus.GONE, "EXPIRED");
      case STATE_OR_PKCE_MISMATCH ->
          error(HttpStatus.UNPROCESSABLE_ENTITY, "STATE_OR_PKCE_MISMATCH");
      case PROVIDER_REJECTED -> error(HttpStatus.BAD_GATEWAY, "PROVIDER_REJECTED");
      case DEPENDENCY_UNAVAILABLE ->
          error(HttpStatus.SERVICE_UNAVAILABLE, "DEPENDENCY_UNAVAILABLE");
    };
  }

  @ExceptionHandler({IllegalArgumentException.class, HttpMessageNotReadableException.class})
  ResponseEntity<ErrorResponse> invalid() {
    return error(HttpStatus.BAD_REQUEST, "INVALID");
  }

  private static ResponseEntity<ErrorResponse> error(HttpStatus status, String code) {
    return ResponseEntity.status(status).body(new ErrorResponse(code));
  }

  public record StartRequest(URI loopbackRedirectUri, String state, String codeChallenge) {}

  public record StartResponse(URI authorizationUrl, Instant expiresAt) {}

  public record ExchangeRequest(String handoffCode, String state, String codeVerifier) {}

  public record MetaSessionResponse(String metaSession, Instant expiresAt) {}

  public record ErrorResponse(String code) {}
}
