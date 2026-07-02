package com.lol.meta.admission;

import com.lol.meta.auth.AuthService;
import com.lol.meta.auth.CurrentAuthAccountProvider;
import com.lol.meta.firewall.TrustedClientAddressResolver;
import jakarta.servlet.http.HttpServletRequest;
import java.time.Instant;
import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;
import org.springframework.web.bind.annotation.DeleteMapping;
import org.springframework.web.bind.annotation.ExceptionHandler;
import org.springframework.web.bind.annotation.GetMapping;
import org.springframework.web.bind.annotation.PathVariable;
import org.springframework.web.bind.annotation.PostMapping;
import org.springframework.web.bind.annotation.RequestBody;
import org.springframework.web.bind.annotation.RequestMapping;
import org.springframework.web.bind.annotation.RestController;

@RestController
@RequestMapping("/api/release0/admission")
public final class AdmissionController {

  private final AdmissionService admissionService;
  private final CurrentAuthAccountProvider currentAuthAccountProvider;
  private final Release0InviteGate release0InviteGate;
  private final TrustedClientAddressResolver trustedClientAddressResolver;

  public AdmissionController(
      AdmissionService admissionService,
      CurrentAuthAccountProvider currentAuthAccountProvider,
      Release0InviteGate release0InviteGate,
      TrustedClientAddressResolver trustedClientAddressResolver) {
    this.admissionService = admissionService;
    this.currentAuthAccountProvider = currentAuthAccountProvider;
    this.release0InviteGate = release0InviteGate;
    this.trustedClientAddressResolver = trustedClientAddressResolver;
  }

  @PostMapping("/enter")
  public AdmissionResponse enter(
      @RequestBody(required = false) AdmissionEnterRequest request,
      HttpServletRequest httpRequest) {
    String inviteCode = request == null ? "" : request.inviteCode();
    if (!release0InviteGate.accepts(inviteCode)) {
      throw new InviteCodeRequiredException();
    }
    return admissionService.enter(currentAccountId(), clientAddress(httpRequest), Instant.now());
  }

  @GetMapping("/status")
  public AdmissionResponse status(HttpServletRequest httpRequest) {
    return admissionService.status(currentAccountId(), clientAddress(httpRequest), Instant.now());
  }

  @GetMapping("/queue/{queueToken}")
  public AdmissionResponse status(
      @PathVariable String queueToken, HttpServletRequest httpRequest) {
    return admissionService.status(queueToken, clientAddress(httpRequest), Instant.now());
  }

  @DeleteMapping("/queue")
  public AdmissionResponse cancel() {
    return admissionService.cancel(currentAccountId(), Instant.now());
  }

  @DeleteMapping("/queue/{queueToken}")
  public AdmissionResponse cancel(@PathVariable String queueToken) {
    return admissionService.cancel(queueToken, Instant.now());
  }

  @ExceptionHandler(AuthService.AuthenticationRequiredException.class)
  public ResponseEntity<ErrorResponse> handleAuthenticationRequired() {
    return ResponseEntity.status(HttpStatus.UNAUTHORIZED)
        .body(new ErrorResponse("로그인이 필요합니다"));
  }

  @ExceptionHandler(AdmissionNicknameUnavailableException.class)
  public ResponseEntity<ErrorResponse> handleNicknameUnavailable() {
    return ResponseEntity.status(HttpStatus.CONFLICT)
        .body(new ErrorResponse("닉네임 설정이 필요합니다"));
  }

  @ExceptionHandler(InviteCodeRequiredException.class)
  public ResponseEntity<ErrorResponse> handleInviteCodeRequired() {
    return ResponseEntity.status(HttpStatus.FORBIDDEN)
        .body(new ErrorResponse("포트폴리오 첫 페이지의 초대 코드를 확인해주세요"));
  }

  private long currentAccountId() {
    return currentAuthAccountProvider
        .currentAccountId()
        .orElseThrow(AuthService.AuthenticationRequiredException::new);
  }

  private String clientAddress(HttpServletRequest request) {
    return trustedClientAddressResolver.resolve(request).orElse("");
  }

  private record AdmissionEnterRequest(String inviteCode) {}

  private record ErrorResponse(String message) {}

  private static final class InviteCodeRequiredException extends RuntimeException {}
}
