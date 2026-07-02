package com.lol.meta.auth;

import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;
import org.springframework.security.web.csrf.CsrfToken;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

@RestController
@RequestMapping("/api/release0/auth")
public final class AuthController {

  private final AuthService authService;

  public AuthController(AuthService authService) {
    this.authService = authService;
  }

  @GetMapping("/session")
  public AuthService.AuthSession currentSession() {
    return authService.currentSession();
  }

  @GetMapping("/csrf")
  public CsrfResponse csrf(CsrfToken token) {
    return new CsrfResponse(token.getHeaderName(), token.getParameterName(), token.getToken());
  }

  @PostMapping("/nickname")
  public AuthService.AuthSession updateNickname(
      @RequestBody(required = false) NicknameRequest request) {
    String nickname = request == null ? null : request.nickname();
    return authService.updateNickname(nickname);
  }

  @PostMapping("/nickname/check")
  public AuthService.NicknameAvailability checkNickname(
      @RequestBody(required = false) NicknameRequest request) {
    String nickname = request == null ? null : request.nickname();
    return authService.checkNickname(nickname);
  }

  @ExceptionHandler(PlayerNickname.InvalidNicknameException.class)
  public ResponseEntity<ErrorResponse> handleInvalidNickname(
      PlayerNickname.InvalidNicknameException exception) {
    return ResponseEntity.badRequest()
        .body(new ErrorResponse(toKoreanMessage(exception.reason())));
  }

  @ExceptionHandler(AuthService.DuplicateNicknameException.class)
  public ResponseEntity<ErrorResponse> handleDuplicateNickname() {
    return ResponseEntity.badRequest().body(new ErrorResponse("이미 사용 중인 닉네임입니다"));
  }

  @ExceptionHandler(AuthService.NicknameAlreadySetException.class)
  public ResponseEntity<ErrorResponse> handleNicknameAlreadySet() {
    return ResponseEntity.badRequest().body(new ErrorResponse("이미 닉네임이 설정되어 있습니다"));
  }

  @ExceptionHandler(AuthService.AuthenticationRequiredException.class)
  public ResponseEntity<ErrorResponse> handleAuthenticationRequired() {
    return ResponseEntity.status(HttpStatus.UNAUTHORIZED)
        .body(new ErrorResponse("로그인이 필요합니다"));
  }

  private String toKoreanMessage(PlayerNickname.FailureReason reason) {
    return switch (reason) {
      case MISSING -> "닉네임을 입력해주세요";
      case LENGTH -> "닉네임은 2자 이상 12자 이하로 입력해주세요";
      case UTF8_BYTE_LIMIT -> "닉네임이 너무 깁니다";
      case UNSUPPORTED_CHARACTERS -> "닉네임은 한글, 영문, 숫자만 사용할 수 있습니다";
    };
  }

  private record NicknameRequest(String nickname) {}

  private record CsrfResponse(String headerName, String parameterName, String token) {}

  private record ErrorResponse(String message) {}
}
