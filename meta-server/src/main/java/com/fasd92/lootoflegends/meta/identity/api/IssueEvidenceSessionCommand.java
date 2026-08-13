package com.fasd92.lootoflegends.meta.identity.api;

import java.time.Instant;

public record IssueEvidenceSessionCommand(String runId, int participantIndex, Instant expiresAt) {}
