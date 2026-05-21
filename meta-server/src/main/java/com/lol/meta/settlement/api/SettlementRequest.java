package com.lol.meta.settlement.api;

import com.lol.meta.settlement.api.validation.ValidSettlementChange;
import jakarta.validation.Valid;
import jakarta.validation.constraints.NotBlank;
import jakarta.validation.constraints.NotNull;
import jakarta.validation.constraints.Positive;
import jakarta.validation.constraints.Size;
import java.time.Instant;
import java.util.List;

@ValidSettlementChange
public record SettlementRequest(
    @NotBlank @Size(max = 64) String settlementId,
    @NotNull @Positive Long sessionId,
    @NotNull @Positive Long accountId,
    @NotNull @Positive Long roomId,
    @NotNull Instant startedAt,
    @NotNull Instant finishedAt,
    @NotNull Long goldDelta,
    @NotNull List<@NotNull @Valid InventoryDeltaRequest> inventoryDeltas,
    @NotNull SettlementReason reason) {}
