package com.fasd92.lootoflegends.meta.platform.security;

import com.fasd92.lootoflegends.meta.identity.api.AuthenticatedPrincipal;
import com.fasd92.lootoflegends.meta.identity.api.DesktopAuthUseCase;
import jakarta.servlet.FilterChain;
import jakarta.servlet.ServletException;
import jakarta.servlet.http.HttpServletRequest;
import jakarta.servlet.http.HttpServletResponse;
import java.io.IOException;
import java.util.List;
import java.util.Optional;
import org.springframework.http.HttpHeaders;
import org.springframework.security.authentication.AnonymousAuthenticationToken;
import org.springframework.security.authentication.UsernamePasswordAuthenticationToken;
import org.springframework.security.core.context.SecurityContextHolder;
import org.springframework.web.filter.OncePerRequestFilter;

final class MetaSessionAuthenticationFilter extends OncePerRequestFilter {
  private static final String PREFIX = "Bearer ";
  private final DesktopAuthUseCase auth;

  MetaSessionAuthenticationFilter(DesktopAuthUseCase auth) {
    this.auth = auth;
  }

  @Override
  protected boolean shouldNotFilter(HttpServletRequest request) {
    String path = request.getRequestURI();
    return !path.startsWith("/api/") || path.startsWith("/api/v1/desktop-auth/");
  }

  @Override
  protected void doFilterInternal(
      HttpServletRequest request, HttpServletResponse response, FilterChain chain)
      throws ServletException, IOException {
    var current = SecurityContextHolder.getContext().getAuthentication();
    if (current != null
        && current.isAuthenticated()
        && !(current instanceof AnonymousAuthenticationToken)) {
      chain.doFilter(request, response);
      return;
    }

    String authorization = request.getHeader(HttpHeaders.AUTHORIZATION);
    if (authorization != null && authorization.startsWith(PREFIX)) {
      Optional<AuthenticatedPrincipal> principal =
          auth.authenticate(authorization.substring(PREFIX.length()));
      if (principal.isPresent()) {
        var authentication =
            UsernamePasswordAuthenticationToken.authenticated(
                principal.orElseThrow(), null, List.of());
        var context = SecurityContextHolder.createEmptyContext();
        context.setAuthentication(authentication);
        SecurityContextHolder.setContext(context);
      }
    }
    chain.doFilter(request, response);
  }
}
