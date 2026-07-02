package com.lol.meta.auth;

import jakarta.servlet.http.HttpServletRequest;
import jakarta.servlet.http.HttpServletResponse;
import jakarta.servlet.http.HttpSession;
import java.net.URI;
import java.util.List;
import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;
import org.springframework.security.authentication.UsernamePasswordAuthenticationToken;
import org.springframework.security.core.Authentication;
import org.springframework.security.core.authority.SimpleGrantedAuthority;
import org.springframework.security.core.context.SecurityContext;
import org.springframework.security.core.context.SecurityContextHolder;
import org.springframework.security.web.context.HttpSessionSecurityContextRepository;
import org.springframework.security.web.context.SecurityContextRepository;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RequestParam;
import org.springframework.web.bind.annotation.RestController;
import org.springframework.web.util.UriComponentsBuilder;

@RestController
@RequestMapping("/api/release0/auth/standalone")
public final class StandaloneOAuthHandoffController {

  static final String CALLBACK_SESSION_ATTRIBUTE = "release0StandaloneCallback";
  static final String STATE_SESSION_ATTRIBUTE = "release0StandaloneState";

  private static final URI GOOGLE_OAUTH_START = URI.create("/oauth2/authorization/google");
  private static final URI BROWSER_SESSION_FALLBACK = URI.create("/api/release0/auth/session");

  private final StandaloneOAuthHandoffService handoffService;
  private final AuthService authService;
  private final SecurityContextRepository securityContextRepository =
      new HttpSessionSecurityContextRepository();

  public StandaloneOAuthHandoffController(
      StandaloneOAuthHandoffService handoffService, AuthService authService) {
    this.handoffService = handoffService;
    this.authService = authService;
  }

  @GetMapping("/start")
  public ResponseEntity<Void> start(
      @RequestParam("callback") String callback,
      @RequestParam("state") String state,
      HttpServletRequest request) {
    if (!handoffService.isAllowedCallback(callback) || !handoffService.isAllowedState(state)) {
      return ResponseEntity.badRequest().build();
    }

    HttpSession session = request.getSession(true);
    session.setAttribute(CALLBACK_SESSION_ATTRIBUTE, callback);
    session.setAttribute(STATE_SESSION_ATTRIBUTE, state);
    return ResponseEntity.status(HttpStatus.FOUND).location(GOOGLE_OAUTH_START).build();
  }

  @GetMapping("/complete")
  public ResponseEntity<Void> complete(HttpServletRequest request) {
    HttpSession session = request.getSession(false);
    if (session == null) {
      return ResponseEntity.status(HttpStatus.FOUND).location(BROWSER_SESSION_FALLBACK).build();
    }

    String callback = (String) session.getAttribute(CALLBACK_SESSION_ATTRIBUTE);
    String state = (String) session.getAttribute(STATE_SESSION_ATTRIBUTE);
    if (callback == null || state == null) {
      return ResponseEntity.status(HttpStatus.FOUND).location(BROWSER_SESSION_FALLBACK).build();
    }

    Authentication authentication = SecurityContextHolder.getContext().getAuthentication();
    if (authentication == null
        || !(authentication.getPrincipal() instanceof Release0AuthenticatedPrincipal principal)) {
      return ResponseEntity.status(HttpStatus.UNAUTHORIZED).build();
    }

    String code = handoffService.issueCode(principal.release0AccountId(), state);
    session.removeAttribute(CALLBACK_SESSION_ATTRIBUTE);
    session.removeAttribute(STATE_SESSION_ATTRIBUTE);
    URI redirect =
        UriComponentsBuilder.fromUriString(callback)
            .queryParam("code", code)
            .queryParam("state", state)
            .build()
            .encode()
            .toUri();
    return ResponseEntity.status(HttpStatus.FOUND).location(redirect).build();
  }

  @PostMapping("/exchange")
  public AuthService.AuthSession exchange(
      @RequestBody(required = false) ExchangeRequest exchangeRequest,
      HttpServletRequest request,
      HttpServletResponse response) {
    if (exchangeRequest == null) {
      throw new StandaloneOAuthHandoffService.InvalidHandoffException();
    }

    StandaloneOAuthHandoffService.ClaimedStandaloneHandoff claimed =
        handoffService.claimCode(exchangeRequest.code(), exchangeRequest.state());
    SecurityContext context = SecurityContextHolder.createEmptyContext();
    Release0StandalonePrincipal principal =
        new Release0StandalonePrincipal(claimed.accountId());
    context.setAuthentication(
        new UsernamePasswordAuthenticationToken(
            principal, "n/a", List.of(new SimpleGrantedAuthority("ROLE_USER"))));
    SecurityContextHolder.setContext(context);
    HttpSession session = request.getSession(false);
    if (session == null) {
      request.getSession(true);
    } else {
      request.changeSessionId();
    }
    securityContextRepository.saveContext(context, request, response);
    return authService.currentSession();
  }

  @ExceptionHandler(StandaloneOAuthHandoffService.InvalidHandoffException.class)
  public ResponseEntity<Void> handleInvalidHandoff() {
    return ResponseEntity.status(HttpStatus.UNAUTHORIZED).build();
  }

  private record ExchangeRequest(String code, String state) {}
}
