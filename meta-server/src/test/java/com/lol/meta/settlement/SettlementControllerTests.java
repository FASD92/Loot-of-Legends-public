package com.lol.meta.settlement;

import static org.hamcrest.Matchers.is;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.ArgumentMatchers.eq;
import static org.mockito.ArgumentMatchers.isNull;
import static org.mockito.Mockito.doThrow;
import static org.mockito.Mockito.never;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.lol.meta.internal.InternalTokenInterceptor;
import com.lol.meta.internal.InternalTokenVerifier;
import com.lol.meta.internal.InternalWebMvcConfig;
import com.lol.meta.settlement.api.SettlementRequest;
import com.lol.meta.settlement.api.SettlementResponse;
import com.lol.meta.settlement.api.SettlementStatus;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.WebMvcTest;
import org.springframework.context.annotation.Import;
import org.springframework.http.MediaType;
import org.springframework.test.context.TestPropertySource;
import org.springframework.test.context.bean.override.mockito.MockitoBean;
import org.springframework.test.web.servlet.MockMvc;

@WebMvcTest(SettlementController.class)
@Import({
  InternalWebMvcConfig.class,
  InternalTokenInterceptor.class,
  InternalTokenVerifier.class
})
@TestPropertySource(properties = "meta.internal.token=test-internal-token")
class SettlementControllerTests {

  private static final String INTERNAL_TOKEN = "test-internal-token";
  private static final String SESSION_TOKEN = "test-session-token";
  private static final String SETTLEMENT_ID = "room-42-session-1001-finished-0001";
  private static final SettlementProcessingGuard.Lease GUARD_LEASE =
      new SettlementProcessingGuard.Lease(SETTLEMENT_ID, "owner-001");

  private final MockMvc mockMvc;

  @MockitoBean private SettlementService settlementService;
  @MockitoBean private SettlementProcessingGuard settlementProcessingGuard;
  @MockitoBean private SettlementSessionValidator settlementSessionValidator;

  @Autowired
  SettlementControllerTests(MockMvc mockMvc) {
    this.mockMvc = mockMvc;
  }

  @Test
  void validRequestWithInternalTokenReturnsAppliedResponse() throws Exception {
    stubAppliedResponse();
    stubGuardAcquired();

    mockMvc
        .perform(
            post("/internal/settlements")
                .header(InternalTokenInterceptor.HEADER_NAME, INTERNAL_TOKEN)
                .header(SettlementSessionValidator.HEADER_NAME, SESSION_TOKEN)
                .contentType(MediaType.APPLICATION_JSON)
                .content(validSettlementJson()))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.settlementId", is(SETTLEMENT_ID)))
        .andExpect(jsonPath("$.status", is("APPLIED")))
        .andExpect(jsonPath("$.duplicate", is(false)));
    verify(settlementProcessingGuard).release(GUARD_LEASE);
  }

  @Test
  void requestWithoutInternalTokenIsRejected() throws Exception {
    mockMvc
        .perform(
            post("/internal/settlements")
                .contentType(MediaType.APPLICATION_JSON)
                .content(validSettlementJson()))
        .andExpect(status().isUnauthorized());
  }

  @Test
  void requestWithWrongInternalTokenIsRejected() throws Exception {
    mockMvc
        .perform(
            post("/internal/settlements")
                .header(InternalTokenInterceptor.HEADER_NAME, "wrong-token")
                .contentType(MediaType.APPLICATION_JSON)
                .content(validSettlementJson()))
        .andExpect(status().isUnauthorized());
  }

  @Test
  void settlementConflictReturnsConflict() throws Exception {
    stubGuardAcquired();
    when(settlementService.apply(any(SettlementRequest.class)))
        .thenThrow(new SettlementConflictException(SETTLEMENT_ID));

    mockMvc
        .perform(
            post("/internal/settlements")
                .header(InternalTokenInterceptor.HEADER_NAME, INTERNAL_TOKEN)
                .header(SettlementSessionValidator.HEADER_NAME, SESSION_TOKEN)
                .contentType(MediaType.APPLICATION_JSON)
                .content(validSettlementJson()))
        .andExpect(status().isConflict());
    verify(settlementProcessingGuard).release(GUARD_LEASE);
  }

  @Test
  void settlementAssetConflictReturnsConflict() throws Exception {
    stubGuardAcquired();
    when(settlementService.apply(any(SettlementRequest.class)))
        .thenThrow(
            new SettlementAssetConflictException(
                SETTLEMENT_ID, "asset balance would become negative"));

    mockMvc
        .perform(
            post("/internal/settlements")
                .header(InternalTokenInterceptor.HEADER_NAME, INTERNAL_TOKEN)
                .header(SettlementSessionValidator.HEADER_NAME, SESSION_TOKEN)
                .contentType(MediaType.APPLICATION_JSON)
                .content(validSettlementJson()))
        .andExpect(status().isConflict());
    verify(settlementProcessingGuard).release(GUARD_LEASE);
  }

  @Test
  void processingGuardConflictReturnsConflict() throws Exception {
    when(settlementProcessingGuard.acquire(SETTLEMENT_ID)).thenReturn(Optional.empty());

    mockMvc
        .perform(
            post("/internal/settlements")
                .header(InternalTokenInterceptor.HEADER_NAME, INTERNAL_TOKEN)
                .header(SettlementSessionValidator.HEADER_NAME, SESSION_TOKEN)
                .contentType(MediaType.APPLICATION_JSON)
                .content(validSettlementJson()))
        .andExpect(status().isConflict());
    verify(settlementService, never()).apply(any(SettlementRequest.class));
    verify(settlementProcessingGuard, never()).release(any(SettlementProcessingGuard.Lease.class));
  }

  @Test
  void requestWithoutSessionTokenIsRejected() throws Exception {
    doThrow(new SettlementSessionUnauthorizedException())
        .when(settlementSessionValidator)
        .validate(isNull(), eq(77L));

    mockMvc
        .perform(
            post("/internal/settlements")
                .header(InternalTokenInterceptor.HEADER_NAME, INTERNAL_TOKEN)
                .contentType(MediaType.APPLICATION_JSON)
                .content(validSettlementJson()))
        .andExpect(status().isUnauthorized());
  }

  @Test
  void invalidSessionTokenIsRejected() throws Exception {
    doThrow(new SettlementSessionUnauthorizedException())
        .when(settlementSessionValidator)
        .validate(eq("wrong-session-token"), eq(77L));

    mockMvc
        .perform(
            post("/internal/settlements")
                .header(InternalTokenInterceptor.HEADER_NAME, INTERNAL_TOKEN)
                .header(SettlementSessionValidator.HEADER_NAME, "wrong-session-token")
                .contentType(MediaType.APPLICATION_JSON)
                .content(validSettlementJson()))
        .andExpect(status().isUnauthorized());
  }

  @Test
  void invalidRequestWithBlankSettlementIdReturnsBadRequest() throws Exception {
    mockMvc
        .perform(
            post("/internal/settlements")
                .header(InternalTokenInterceptor.HEADER_NAME, INTERNAL_TOKEN)
                .header(SettlementSessionValidator.HEADER_NAME, SESSION_TOKEN)
                .contentType(MediaType.APPLICATION_JSON)
                .content(validSettlementJson().replace(
                    "\"settlementId\": \"room-42-session-1001-finished-0001\"",
                    "\"settlementId\": \"   \"")))
        .andExpect(status().isBadRequest());
  }

  @Test
  void invalidRequestWithZeroDeltaReturnsBadRequest() throws Exception {
    mockMvc
        .perform(
            post("/internal/settlements")
                .header(InternalTokenInterceptor.HEADER_NAME, INTERNAL_TOKEN)
                .header(SettlementSessionValidator.HEADER_NAME, SESSION_TOKEN)
                .contentType(MediaType.APPLICATION_JSON)
                .content(
                    """
                    {
                      "settlementId": "room-42-session-1001-finished-0001",
                      "sessionId": 1001,
                      "accountId": 77,
                      "roomId": 42,
                      "startedAt": "2026-05-15T01:00:00Z",
                      "finishedAt": "2026-05-15T01:05:00Z",
                      "goldDelta": 0,
                      "inventoryDeltas": [],
                      "reason": "NORMAL"
                    }
                    """))
        .andExpect(status().isBadRequest());
  }

  @Test
  void invalidReasonReturnsBadRequest() throws Exception {
    mockMvc
        .perform(
            post("/internal/settlements")
                .header(InternalTokenInterceptor.HEADER_NAME, INTERNAL_TOKEN)
                .header(SettlementSessionValidator.HEADER_NAME, SESSION_TOKEN)
                .contentType(MediaType.APPLICATION_JSON)
                .content(
                    validSettlementJson()
                        .replace("\"reason\": \"NORMAL\"", "\"reason\": \"INVALID\"")))
        .andExpect(status().isBadRequest());
  }

  private String validSettlementJson() {
    return """
        {
          "settlementId": "room-42-session-1001-finished-0001",
          "sessionId": 1001,
          "accountId": 77,
          "roomId": 42,
          "startedAt": "2026-05-15T01:00:00Z",
          "finishedAt": "2026-05-15T01:05:00Z",
          "goldDelta": 120,
          "inventoryDeltas": [
            { "itemId": 30001, "quantityDelta": 1, "sourceDropId": 9001 }
          ],
          "reason": "NORMAL"
        }
        """;
  }

  private void stubAppliedResponse() {
    when(settlementService.apply(any(SettlementRequest.class)))
        .thenAnswer(
            invocation -> {
              SettlementRequest request = invocation.getArgument(0);
              return new SettlementResponse(
                  request.settlementId(), SettlementStatus.APPLIED, false);
            });
  }

  private void stubGuardAcquired() {
    when(settlementProcessingGuard.acquire(SETTLEMENT_ID)).thenReturn(Optional.of(GUARD_LEASE));
  }
}
