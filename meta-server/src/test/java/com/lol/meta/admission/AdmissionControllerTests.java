package com.lol.meta.admission;

import static org.hamcrest.Matchers.is;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.ArgumentMatchers.eq;
import static org.mockito.Mockito.never;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.delete;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;

import com.lol.meta.auth.CurrentAuthAccountProvider;
import com.lol.meta.auth.Release0SecurityConfig;
import com.lol.meta.firewall.TrustedClientAddressResolver;
import com.lol.meta.internal.InternalTokenVerifier;
import jakarta.servlet.http.HttpServletRequest;
import java.time.Instant;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.WebMvcTest;
import org.springframework.context.annotation.Import;
import org.springframework.mock.web.MockHttpServletResponse;
import org.springframework.security.web.csrf.CsrfToken;
import org.springframework.security.web.csrf.HttpSessionCsrfTokenRepository;
import org.springframework.security.web.csrf.XorCsrfTokenRequestAttributeHandler;
import org.springframework.test.context.bean.override.mockito.MockitoBean;
import org.springframework.test.web.servlet.MockMvc;
import org.springframework.test.web.servlet.request.RequestPostProcessor;

@WebMvcTest(AdmissionController.class)
@Import(Release0SecurityConfig.class)
class AdmissionControllerTests {

  private final MockMvc mockMvc;

  @MockitoBean private AdmissionService admissionService;
  @MockitoBean private CurrentAuthAccountProvider currentAuthAccountProvider;
  @MockitoBean private InternalTokenVerifier internalTokenVerifier;
  @MockitoBean private Release0InviteGate release0InviteGate;
  @MockitoBean private TrustedClientAddressResolver trustedClientAddressResolver;

  @Autowired
  AdmissionControllerTests(MockMvc mockMvc) {
    this.mockMvc = mockMvc;
  }

  @Test
  void enterWithoutCsrfIsForbidden() throws Exception {
    mockMvc.perform(post("/api/release0/admission/enter")).andExpect(status().isForbidden());
  }

  @Test
  void enterWithCsrfIsAllowed() throws Exception {
    when(currentAuthAccountProvider.currentAccountId()).thenReturn(Optional.of(123L));
    when(release0InviteGate.accepts("portfolio-2026")).thenReturn(true);
    when(trustedClientAddressResolver.resolve(any(HttpServletRequest.class)))
        .thenReturn(Optional.of("203.0.113.31"));
    when(admissionService.enter(eq(123L), eq("203.0.113.31"), any(Instant.class)))
        .thenReturn(AdmissionResponse.queued(1, "queue-token-1"));

    mockMvc
        .perform(
            post("/api/release0/admission/enter")
                .with(validCsrf())
                .contentType("application/json")
                .content("{\"inviteCode\":\"portfolio-2026\"}"))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.status", is("Queued")))
        .andExpect(jsonPath("$.queueToken", is("queue-token-1")));
  }

  @Test
  void enterWithWrongInviteCodeIsForbidden() throws Exception {
    when(release0InviteGate.accepts("wrong-code")).thenReturn(false);

    mockMvc
        .perform(
            post("/api/release0/admission/enter")
                .with(validCsrf())
                .contentType("application/json")
                .content("{\"inviteCode\":\"wrong-code\"}"))
        .andExpect(status().isForbidden())
        .andExpect(jsonPath("$.message", is("포트폴리오 첫 페이지의 초대 코드를 확인해주세요")));

    verify(admissionService, never()).enter(any(Long.class), any(Instant.class));
  }

  @Test
  void enterUsesCurrentAuthAccountProviderAccountId() throws Exception {
    when(currentAuthAccountProvider.currentAccountId()).thenReturn(Optional.of(456L));
    when(release0InviteGate.accepts("portfolio-2026")).thenReturn(true);
    when(trustedClientAddressResolver.resolve(any(HttpServletRequest.class)))
        .thenReturn(Optional.of("203.0.113.32"));
    when(admissionService.enter(eq(456L), eq("203.0.113.32"), any(Instant.class)))
        .thenReturn(AdmissionResponse.queued(1, "queue-token-456"));

    mockMvc
        .perform(
            post("/api/release0/admission/enter")
                .with(validCsrf())
                .contentType("application/json")
                .content("{\"inviteCode\":\"portfolio-2026\"}"))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.status", is("Queued")))
        .andExpect(jsonPath("$.queueToken", is("queue-token-456")));
  }

  @Test
  void tokenQueueStatusAllowsQueuedPayloadWithoutQueueTokenLeak() throws Exception {
    when(trustedClientAddressResolver.resolve(any(HttpServletRequest.class)))
        .thenReturn(Optional.of("203.0.113.33"));
    when(admissionService.status(eq("queue-token-1"), eq("203.0.113.33"), any(Instant.class)))
        .thenReturn(AdmissionResponse.queued(7));

    mockMvc
        .perform(get("/api/release0/admission/queue/queue-token-1"))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.status", is("Queued")))
        .andExpect(jsonPath("$.position", is(7)))
        .andExpect(jsonPath("$.queueToken").doesNotExist());
  }

  @Test
  void tokenQueueCancelWithoutCsrfIsForbidden() throws Exception {
    mockMvc
        .perform(delete("/api/release0/admission/queue/queue-token-1"))
        .andExpect(status().isForbidden());
  }

  @Test
  void tokenQueueCancelIsAllowed() throws Exception {
    when(admissionService.cancel(eq("queue-token-1"), any(Instant.class)))
        .thenReturn(AdmissionResponse.notQueued());

    mockMvc
        .perform(delete("/api/release0/admission/queue/queue-token-1").with(validCsrf()))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.status", is("NotQueued")));
  }

  private static RequestPostProcessor validCsrf() {
    return request -> {
      HttpSessionCsrfTokenRepository tokenRepository = new HttpSessionCsrfTokenRepository();
      MockHttpServletResponse response = new MockHttpServletResponse();
      CsrfToken token = tokenRepository.generateToken(request);
      tokenRepository.saveToken(token, request, response);
      XorCsrfTokenRequestAttributeHandler handler = new XorCsrfTokenRequestAttributeHandler();
      handler.handle(request, response, () -> token);
      CsrfToken maskedToken = (CsrfToken) request.getAttribute(token.getParameterName());
      request.setParameter(maskedToken.getParameterName(), maskedToken.getToken());
      return request;
    };
  }
}
