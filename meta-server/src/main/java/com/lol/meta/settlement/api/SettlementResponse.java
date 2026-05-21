package com.lol.meta.settlement.api;

import jakarta.validation.constraints.NotBlank;
import jakarta.validation.constraints.NotNull;
import jakarta.validation.constraints.Size;

public record SettlementResponse(
    @NotBlank @Size(max = 64) String settlementId,
    @NotNull SettlementStatus status,
    boolean duplicate) {}
