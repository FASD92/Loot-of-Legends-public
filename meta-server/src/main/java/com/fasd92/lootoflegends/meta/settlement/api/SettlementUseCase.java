package com.fasd92.lootoflegends.meta.settlement.api;

import java.util.Optional;

public interface SettlementUseCase {
  Acceptance accept(Submission submission);

  Optional<Settlement> find(String settlementId);

  record Submission(String settlementId, String payloadHash, String canonicalPayload) {}

  record Settlement(String settlementId, Status status) {}

  sealed interface Acceptance permits Accepted, Conflict {}

  record Accepted(Settlement settlement) implements Acceptance {}

  record Conflict() implements Acceptance {}

  enum Status {
    ACCEPTED_PENDING("AcceptedPending"),
    APPLIED("Applied");

    private final String wireValue;

    Status(String wireValue) {
      this.wireValue = wireValue;
    }

    public String wireValue() {
      return wireValue;
    }

    public static Status fromStorage(String value) {
      return Status.valueOf(value);
    }
  }

  final class Invalid extends RuntimeException {
    public Invalid() {
      super("invalid settlement submission");
    }
  }

  final class DependencyUnavailable extends RuntimeException {
    public DependencyUnavailable(Throwable cause) {
      super(cause);
    }
  }
}
