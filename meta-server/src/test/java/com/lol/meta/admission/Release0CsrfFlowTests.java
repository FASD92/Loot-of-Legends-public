package com.lol.meta.admission;

import static org.hamcrest.Matchers.is;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.ArgumentMatchers.eq;
import static org.mockito.Mockito.when;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.fasterxml.jackson.core.type.TypeReference;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.lol.meta.auth.AuthController;
import com.lol.meta.auth.AuthService;
import com.lol.meta.auth.CurrentAuthAccountProvider;
import com.lol.meta.auth.Release0SecurityConfig;
import com.lol.meta.firewall.TrustedClientAddressResolver;
import com.lol.meta.internal.InternalTokenVerifier;
import jakarta.servlet.http.HttpServletRequest;
import java.time.Instant;
import java.util.Map;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.WebMvcTest;
import org.springframework.context.annotation.Import;
import org.springframework.mock.web.MockHttpSession;
import org.springframework.test.context.bean.override.mockito.MockitoBean;
import org.springframework.test.web.servlet.MockMvc;
import org.springframework.test.web.servlet.MvcResult;

@WebMvcTest({AuthController.class, AdmissionController.class})
@Import(Release0SecurityConfig.class)
class Release0CsrfFlowTests {

  private static final TypeReference<Map<String, String>> STRING_MAP =
      new TypeReference<>() {};

  private final MockMvc mockMvc;
  private final ObjectMapper objectMapper;

  @MockitoBean private AuthService authService;
  @MockitoBean private AdmissionService admissionService;
  @MockitoBean private CurrentAuthAccountProvider currentAuthAccountProvider;
  @MockitoBean private InternalTokenVerifier internalTokenVerifier;
  @MockitoBean private Release0InviteGate release0InviteGate;
  @MockitoBean private TrustedClientAddressResolver trustedClientAddressResolver;

  @Autowired
  Release0CsrfFlowTests(MockMvc mockMvc, ObjectMapper objectMapper) {
    this.mockMvc = mockMvc;
    this.objectMapper = objectMapper;
  }

  @Test
  void fetchedCsrfTokenAllowsPublicAdmissionEnter() throws Exception {
    MvcResult csrfResult =
        mockMvc
            .perform(get("/api/release0/auth/csrf"))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.headerName", is("X-CSRF-TOKEN")))
            .andExpect(jsonPath("$.parameterName", is("_csrf")))
            .andReturn();
    Map<String, String> csrf =
        objectMapper.readValue(csrfResult.getResponse().getContentAsString(), STRING_MAP);
    MockHttpSession session = (MockHttpSession) csrfResult.getRequest().getSession(false);

    when(currentAuthAccountProvider.currentAccountId()).thenReturn(Optional.of(123L));
    when(release0InviteGate.accepts("portfolio-2026")).thenReturn(true);
    when(trustedClientAddressResolver.resolve(any(HttpServletRequest.class)))
        .thenReturn(Optional.of("203.0.113.34"));
    when(admissionService.enter(eq(123L), eq("203.0.113.34"), any(Instant.class)))
        .thenReturn(AdmissionResponse.queued(1, "queue-token-1"));

    mockMvc
        .perform(
            post("/api/release0/admission/enter")
                .session(session)
                .header(csrf.get("headerName"), csrf.get("token"))
                .contentType("application/json")
                .content("{\"inviteCode\":\"portfolio-2026\"}"))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.status", is("Queued")))
        .andExpect(jsonPath("$.queueToken", is("queue-token-1")));
  }

  @Test
  void fetchedCsrfSessionWithoutHeaderIsForbidden() throws Exception {
    MvcResult csrfResult =
        mockMvc.perform(get("/api/release0/auth/csrf")).andExpect(status().isOk()).andReturn();
    MockHttpSession session = (MockHttpSession) csrfResult.getRequest().getSession(false);

    mockMvc
        .perform(post("/api/release0/admission/enter").session(session))
        .andExpect(status().isForbidden());
  }

  @Test
  void fetchedCsrfHeaderWithoutSessionIsForbidden() throws Exception {
    MvcResult csrfResult =
        mockMvc.perform(get("/api/release0/auth/csrf")).andExpect(status().isOk()).andReturn();
    Map<String, String> csrf =
        objectMapper.readValue(csrfResult.getResponse().getContentAsString(), STRING_MAP);

    mockMvc
        .perform(
            post("/api/release0/admission/enter")
                .header(csrf.get("headerName"), csrf.get("token")))
        .andExpect(status().isForbidden());
  }
}
