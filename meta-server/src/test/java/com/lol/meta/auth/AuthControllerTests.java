package com.lol.meta.auth;

import static org.hamcrest.Matchers.is;
import static org.hamcrest.Matchers.nullValue;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.ArgumentMatchers.anyLong;
import static org.mockito.Mockito.never;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.when;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

import com.lol.meta.internal.InternalTokenVerifier;
import java.util.Optional;
import java.util.stream.Stream;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.Arguments;
import org.junit.jupiter.params.provider.MethodSource;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.WebMvcTest;
import org.springframework.boot.test.context.TestConfiguration;
import org.springframework.context.annotation.Bean;
import org.springframework.context.annotation.Import;
import org.springframework.http.MediaType;
import org.springframework.mock.web.MockHttpServletResponse;
import org.springframework.security.web.csrf.CsrfToken;
import org.springframework.security.web.csrf.HttpSessionCsrfTokenRepository;
import org.springframework.security.web.csrf.XorCsrfTokenRequestAttributeHandler;
import org.springframework.test.context.bean.override.mockito.MockitoBean;
import org.springframework.test.web.servlet.MockMvc;
import org.springframework.test.web.servlet.request.RequestPostProcessor;

@WebMvcTest(AuthController.class)
@Import({
  AuthService.class,
  Release0SecurityConfig.class,
  AuthControllerTests.CurrentAuthAccountProviderTestConfig.class
})
class AuthControllerTests {

  private final MockMvc mockMvc;
  private final TestCurrentAuthAccountProvider currentAuthAccountProvider;

  @MockitoBean private InternalTokenVerifier internalTokenVerifier;
  @MockitoBean private PlayerAccountRepository playerAccountRepository;

  @Autowired
  AuthControllerTests(MockMvc mockMvc, TestCurrentAuthAccountProvider currentAuthAccountProvider) {
    this.mockMvc = mockMvc;
    this.currentAuthAccountProvider = currentAuthAccountProvider;
  }

  @BeforeEach
  void resetCurrentAuthAccount() {
    currentAuthAccountProvider.clear();
  }

  @Test
  void sessionWithoutOAuthPrincipalReturnsUnauthenticated() throws Exception {
    mockMvc
        .perform(get("/api/release0/auth/session"))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.authenticated", is(false)))
        .andExpect(jsonPath("$.accountId", nullValue()))
        .andExpect(jsonPath("$.nicknameRequired", is(false)))
        .andExpect(jsonPath("$.nickname", nullValue()));
  }

  @Test
  void sessionForAuthenticatedAccountWithoutNicknameReturnsNicknameRequired() throws Exception {
    currentAuthAccountProvider.authenticateAs(123L);
    when(playerAccountRepository.findAccount(123L))
        .thenReturn(Optional.of(new PlayerAccount(123L, null)));

    mockMvc
        .perform(get("/api/release0/auth/session"))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.authenticated", is(true)))
        .andExpect(jsonPath("$.accountId", is(123)))
        .andExpect(jsonPath("$.nicknameRequired", is(true)))
        .andExpect(jsonPath("$.nickname", nullValue()));
  }

  @Test
  void sessionForAuthenticatedAccountWithNicknameReturnsNickname() throws Exception {
    currentAuthAccountProvider.authenticateAs(123L);
    when(playerAccountRepository.findAccount(123L))
        .thenReturn(Optional.of(new PlayerAccount(123L, "Player123")));

    mockMvc
        .perform(get("/api/release0/auth/session"))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.authenticated", is(true)))
        .andExpect(jsonPath("$.accountId", is(123)))
        .andExpect(jsonPath("$.nicknameRequired", is(false)))
        .andExpect(jsonPath("$.nickname", is("Player123")));
  }

  @Test
  void sessionForMissingAuthenticatedAccountReturnsUnauthenticated() throws Exception {
    currentAuthAccountProvider.authenticateAs(123L);
    when(playerAccountRepository.findAccount(123L)).thenReturn(Optional.empty());

    mockMvc
        .perform(get("/api/release0/auth/session"))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.authenticated", is(false)))
        .andExpect(jsonPath("$.accountId", nullValue()))
        .andExpect(jsonPath("$.nicknameRequired", is(false)))
        .andExpect(jsonPath("$.nickname", nullValue()));
  }

  @Test
  void nicknameCheckReturnsAvailableForUnusedNicknameWithoutAuthentication() throws Exception {
    when(playerAccountRepository.existsByNickname(PlayerNickname.parse("Ready26"))).thenReturn(false);

    mockMvc
        .perform(
            post("/api/release0/auth/nickname/check")
                .with(validCsrf())
                .contentType(MediaType.APPLICATION_JSON)
                .content(
                    """
                    { "nickname": "Ready26" }
                    """))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.available", is(true)))
        .andExpect(jsonPath("$.message", is("사용 가능한 플레이어 이름입니다")));

    verify(playerAccountRepository).existsByNickname(PlayerNickname.parse("Ready26"));
    verify(playerAccountRepository, never())
        .updateNickname(anyLong(), any(PlayerNickname.class));
  }

  @Test
  void nicknameCheckReturnsUnavailableForDuplicateNicknameWithoutAuthentication() throws Exception {
    when(playerAccountRepository.existsByNickname(PlayerNickname.parse("Ready26"))).thenReturn(true);

    mockMvc
        .perform(
            post("/api/release0/auth/nickname/check")
                .with(validCsrf())
                .contentType(MediaType.APPLICATION_JSON)
                .content(
                    """
                    { "nickname": "Ready26" }
                    """))
        .andExpect(status().isOk())
        .andExpect(jsonPath("$.available", is(false)))
        .andExpect(jsonPath("$.message", is("이미 사용 중인 플레이어 이름입니다")));

    verify(playerAccountRepository).existsByNickname(PlayerNickname.parse("Ready26"));
    verify(playerAccountRepository, never())
        .updateNickname(anyLong(), any(PlayerNickname.class));
  }

  @Test
  void nicknameCheckRejectsInvalidNicknameWithExistingKoreanMessage() throws Exception {
    mockMvc
        .perform(
            post("/api/release0/auth/nickname/check")
                .with(validCsrf())
                .contentType(MediaType.APPLICATION_JSON)
                .content(
                    """
                    { "nickname": "bad_name" }
                    """))
        .andExpect(status().isBadRequest())
        .andExpect(jsonPath("$.message", is("닉네임은 한글, 영문, 숫자만 사용할 수 있습니다")));

    verify(playerAccountRepository, never()).existsByNickname(any(PlayerNickname.class));
    verify(playerAccountRepository, never())
        .updateNickname(anyLong(), any(PlayerNickname.class));
  }

  @Test
  void duplicateNicknameReturnsKoreanCauseMessageForAuthenticatedAccount() throws Exception {
    currentAuthAccountProvider.authenticateAs(123L);
    when(playerAccountRepository.existsByNickname(any(PlayerNickname.class))).thenReturn(true);

    mockMvc
        .perform(
            post("/api/release0/auth/nickname")
                .with(validCsrf())
                .contentType(MediaType.APPLICATION_JSON)
                .content(
                    """
                    { "nickname": "player123" }
                    """))
        .andExpect(status().isBadRequest())
        .andExpect(jsonPath("$.message", is("이미 사용 중인 닉네임입니다")));

    verify(playerAccountRepository).existsByNickname(PlayerNickname.parse("player123"));
    verify(playerAccountRepository, never())
        .updateNickname(anyLong(), any(PlayerNickname.class));
  }

  @Test
  void alreadySetNicknameCannotBeChanged() throws Exception {
    currentAuthAccountProvider.authenticateAs(123L);
    when(playerAccountRepository.existsByNickname(any(PlayerNickname.class))).thenReturn(false);
    when(playerAccountRepository.updateNickname(123L, PlayerNickname.parse("player123")))
        .thenReturn(false);

    mockMvc
        .perform(
            post("/api/release0/auth/nickname")
                .with(validCsrf())
                .contentType(MediaType.APPLICATION_JSON)
                .content(
                    """
                    { "nickname": "player123" }
                    """))
        .andExpect(status().isBadRequest())
        .andExpect(jsonPath("$.message", is("이미 닉네임이 설정되어 있습니다")));

    verify(playerAccountRepository).existsByNickname(PlayerNickname.parse("player123"));
    verify(playerAccountRepository).updateNickname(123L, PlayerNickname.parse("player123"));
  }

  @Test
  void validNicknameWithoutAuthenticatedAccountReturnsUnauthorizedWithoutRepositoryUpdate()
      throws Exception {
    mockMvc
        .perform(
            post("/api/release0/auth/nickname")
                .with(validCsrf())
                .contentType(MediaType.APPLICATION_JSON)
                .content(
                    """
                    { "nickname": "player123" }
                    """))
        .andExpect(status().isUnauthorized())
        .andExpect(jsonPath("$.message", is("로그인이 필요합니다")));

    verify(playerAccountRepository, never()).existsByNickname(any(PlayerNickname.class));
    verify(playerAccountRepository, never())
        .updateNickname(anyLong(), any(PlayerNickname.class));
  }

  @Test
  void unknownPublicRouteIsDeniedBeforeHandlerLookup() throws Exception {
    mockMvc.perform(get("/api/release0/unknown")).andExpect(status().isForbidden());
  }

  @Test
  void nicknameWithoutCsrfIsForbidden() throws Exception {
    currentAuthAccountProvider.authenticateAs(123L);

    mockMvc
        .perform(
            post("/api/release0/auth/nickname")
                .contentType(MediaType.APPLICATION_JSON)
                .content(
                    """
                    { "nickname": "player123" }
                    """))
        .andExpect(status().isForbidden());

    verify(playerAccountRepository, never()).existsByNickname(any(PlayerNickname.class));
    verify(playerAccountRepository, never())
        .updateNickname(anyLong(), any(PlayerNickname.class));
  }

  @ParameterizedTest
  @MethodSource("invalidNicknameRequests")
  void invalidNicknameReturnsKoreanCauseMessage(String requestJson, String expectedMessage)
      throws Exception {
    mockMvc
        .perform(
            post("/api/release0/auth/nickname")
                .with(validCsrf())
                .contentType(MediaType.APPLICATION_JSON)
                .content(requestJson))
        .andExpect(status().isBadRequest())
        .andExpect(jsonPath("$.message", is(expectedMessage)));
  }

  static Stream<Arguments> invalidNicknameRequests() {
    return Stream.of(
        Arguments.of("{}", "닉네임을 입력해주세요"),
        Arguments.of("{ \"nickname\": null }", "닉네임을 입력해주세요"),
        Arguments.of("{ \"nickname\": \"   \" }", "닉네임을 입력해주세요"),
        Arguments.of("{ \"nickname\": \"a\" }", "닉네임은 2자 이상 12자 이하로 입력해주세요"),
        Arguments.of(
            "{ \"nickname\": \"abcdefghijklmnopqrstu\" }",
            "닉네임은 2자 이상 12자 이하로 입력해주세요"),
        Arguments.of("{ \"nickname\": \"가나다라마바사아자차카\" }", "닉네임이 너무 깁니다"),
        Arguments.of(
            "{ \"nickname\": \"bad_name\" }",
            "닉네임은 한글, 영문, 숫자만 사용할 수 있습니다"));
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

  @TestConfiguration
  static class CurrentAuthAccountProviderTestConfig {

    @Bean
    TestCurrentAuthAccountProvider testCurrentAuthAccountProvider() {
      return new TestCurrentAuthAccountProvider();
    }
  }

  static final class TestCurrentAuthAccountProvider implements CurrentAuthAccountProvider {

    private Optional<Long> accountId = Optional.empty();

    void authenticateAs(long accountId) {
      this.accountId = Optional.of(accountId);
    }

    void clear() {
      accountId = Optional.empty();
    }

    @Override
    public Optional<Long> currentAccountId() {
      return accountId;
    }
  }
}
