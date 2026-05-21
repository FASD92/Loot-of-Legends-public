package com.lol.meta.settlement.repository;

public record SettlementRecordRow(
    String settlementId,
    long accountId,
    long sessionId,
    long roomId,
    String status,
    long goldDelta,
    String requestHash) {}
