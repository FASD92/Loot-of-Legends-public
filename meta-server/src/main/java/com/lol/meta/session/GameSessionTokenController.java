package com.lol.meta.session;

import java.time.Instant;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

@RestController
@RequestMapping("/internal/release0/game-session-tokens")
public final class GameSessionTokenController {

  private final GameSessionTokenService gameSessionTokenService;

  public GameSessionTokenController(GameSessionTokenService gameSessionTokenService) {
    this.gameSessionTokenService = gameSessionTokenService;
  }

  @PostMapping("/claim")
  public GameSessionClaimResponse claim(
      @RequestBody(required = false) GameSessionClaimRequest request) {
    return gameSessionTokenService.claim(request, Instant.now());
  }

  @PostMapping("/release")
  public ResponseEntity<Void> release(
      @RequestBody(required = false) GameSessionReleaseRequest request) {
    gameSessionTokenService.release(request);
    return ResponseEntity.noContent().build();
  }

  @PostMapping("/renew")
  public ResponseEntity<Void> renew(
      @RequestBody(required = false) GameSessionRenewRequest request) {
    gameSessionTokenService.renew(request);
    return ResponseEntity.noContent().build();
  }
}
