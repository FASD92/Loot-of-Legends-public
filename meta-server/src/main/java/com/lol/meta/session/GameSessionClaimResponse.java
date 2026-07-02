package com.lol.meta.session;

import com.fasterxml.jackson.annotation.JsonInclude;

@JsonInclude(JsonInclude.Include.NON_NULL)
public record GameSessionClaimResponse(
    GameSessionClaimStatus status,
    Long accountId,
    String nickname,
    String replacedSessionId,
    Long reservationExpiresAt) {

  static GameSessionClaimResponse accepted(ClaimedGameSession session) {
    return new GameSessionClaimResponse(
        GameSessionClaimStatus.ACCEPTED,
        session.accountId(),
        session.nickname(),
        session.replacedSessionId(),
        session.reservationExpiresAt().toEpochMilli());
  }

  static GameSessionClaimResponse rejected() {
    return new GameSessionClaimResponse(GameSessionClaimStatus.REJECTED, null, null, null, null);
  }
}
