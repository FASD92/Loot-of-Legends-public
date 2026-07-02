package com.lol.meta.session;

import com.fasterxml.jackson.annotation.JsonValue;

public enum GameSessionClaimStatus {
  ACCEPTED("Accepted"),
  REJECTED("Rejected");

  private final String jsonValue;

  GameSessionClaimStatus(String jsonValue) {
    this.jsonValue = jsonValue;
  }

  @JsonValue
  public String jsonValue() {
    return jsonValue;
  }
}
