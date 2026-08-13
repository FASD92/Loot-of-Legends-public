package com.fasd92.lootoflegends.meta.platform.security;

import jakarta.servlet.FilterChain;
import jakarta.servlet.ServletException;
import jakarta.servlet.http.HttpServletRequest;
import jakarta.servlet.http.HttpServletResponse;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.List;
import org.springframework.http.HttpHeaders;
import org.springframework.http.MediaType;
import org.springframework.security.authentication.UsernamePasswordAuthenticationToken;
import org.springframework.security.core.authority.SimpleGrantedAuthority;
import org.springframework.security.core.context.SecurityContextHolder;
import org.springframework.web.filter.OncePerRequestFilter;

final class InternalServiceAuthenticationFilter extends OncePerRequestFilter {
  private static final String PREFIX = "Bearer ";
  private final byte[] expectedToken;

  InternalServiceAuthenticationFilter(String expectedToken) {
    if (expectedToken == null || expectedToken.isBlank()) {
      throw new IllegalArgumentException("loot.internal.service-token must be configured");
    }
    this.expectedToken = expectedToken.getBytes(StandardCharsets.UTF_8);
  }

  @Override
  protected boolean shouldNotFilter(HttpServletRequest request) {
    return !request.getRequestURI().startsWith("/internal/");
  }

  @Override
  protected void doFilterInternal(
      HttpServletRequest request, HttpServletResponse response, FilterChain chain)
      throws ServletException, IOException {
    String authorization = request.getHeader(HttpHeaders.AUTHORIZATION);
    if (authorization == null
        || !authorization.startsWith(PREFIX)
        || !MessageDigest.isEqual(
            expectedToken,
            authorization.substring(PREFIX.length()).getBytes(StandardCharsets.UTF_8))) {
      unauthorized(response);
      return;
    }

    var authentication =
        UsernamePasswordAuthenticationToken.authenticated(
            "loot-game-server", null, List.of(new SimpleGrantedAuthority("ROLE_GAME_SERVER")));
    var context = SecurityContextHolder.createEmptyContext();
    context.setAuthentication(authentication);
    SecurityContextHolder.setContext(context);
    chain.doFilter(request, response);
  }

  static void unauthorized(HttpServletResponse response) throws IOException {
    response.setStatus(HttpServletResponse.SC_UNAUTHORIZED);
    response.setContentType(MediaType.APPLICATION_JSON_VALUE);
    response.getWriter().write("{\"code\":\"UNAUTHORIZED\"}");
  }
}
