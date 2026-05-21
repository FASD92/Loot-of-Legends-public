package com.lol.meta.internal;

import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.WebMvcTest;
import org.springframework.context.annotation.Import;
import org.springframework.http.ResponseEntity;
import org.springframework.test.context.TestPropertySource;
import org.springframework.test.web.servlet.MockMvc;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.RestController;

@WebMvcTest(controllers = InternalCredentialInterceptorTests.ProbeController.class)
@Import({
  InternalWebMvcConfig.class,
  InternalTokenInterceptor.class,
  InternalTokenVerifier.class,
  InternalCredentialInterceptorTests.ProbeController.class
})
@TestPropertySource(properties = "meta.internal.token=test-internal-token")
class InternalCredentialInterceptorTests {

  private final MockMvc mockMvc;

  @Autowired
  InternalCredentialInterceptorTests(MockMvc mockMvc) {
    this.mockMvc = mockMvc;
  }

  @Test
  void internalPathAllowsMatchingInternalToken() throws Exception {
    mockMvc
        .perform(get("/internal/probe").header(InternalTokenInterceptor.HEADER_NAME, "test-internal-token"))
        .andExpect(status().isOk());
  }

  @Test
  void internalPathRejectsMissingInternalToken() throws Exception {
    mockMvc.perform(get("/internal/probe")).andExpect(status().isUnauthorized());
  }

  @Test
  void internalPathRejectsWrongInternalToken() throws Exception {
    mockMvc
        .perform(get("/internal/probe").header(InternalTokenInterceptor.HEADER_NAME, "wrong-token"))
        .andExpect(status().isUnauthorized());
  }

  @Test
  void publicPathDoesNotRequireInternalToken() throws Exception {
    mockMvc.perform(get("/public/probe")).andExpect(status().isOk());
  }

  @RestController
  public static class ProbeController {

    @GetMapping("/internal/probe")
    public ResponseEntity<Void> internalProbe() {
      return ResponseEntity.ok().build();
    }

    @GetMapping("/public/probe")
    public ResponseEntity<Void> publicProbe() {
      return ResponseEntity.ok().build();
    }
  }
}
