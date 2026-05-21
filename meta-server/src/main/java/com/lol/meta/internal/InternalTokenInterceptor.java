package com.lol.meta.internal;

import jakarta.servlet.http.HttpServletRequest;
import jakarta.servlet.http.HttpServletResponse;
import org.springframework.stereotype.Component;
import org.springframework.web.servlet.HandlerInterceptor;

@Component
public final class InternalTokenInterceptor implements HandlerInterceptor {

  public static final String HEADER_NAME = "X-Internal-Token";

  private final InternalTokenVerifier tokenVerifier;

  public InternalTokenInterceptor(InternalTokenVerifier tokenVerifier) {
    this.tokenVerifier = tokenVerifier;
  }

  @Override
  public boolean preHandle(HttpServletRequest request, HttpServletResponse response, Object handler)
      throws Exception {
    if (tokenVerifier.matches(request.getHeader(HEADER_NAME))) {
      return true;
    }

    response.sendError(HttpServletResponse.SC_UNAUTHORIZED);
    return false;
  }
}
