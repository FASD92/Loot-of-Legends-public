package com.fasd92.lootoflegends.meta.identity.api;

public interface EvidenceAdmissionUseCase {
  IssuedMetaSession issueEvidenceSession(IssueEvidenceSessionCommand command);

  enum RejectionReason {
    REPLAYED,
    DEPENDENCY_UNAVAILABLE
  }

  final class Rejected extends RuntimeException {
    private final RejectionReason reason;

    public Rejected(RejectionReason reason) {
      this(reason, null);
    }

    public Rejected(RejectionReason reason, Throwable cause) {
      super(reason.name(), cause);
      this.reason = reason;
    }

    public RejectionReason reason() {
      return reason;
    }
  }
}
