package com.lol.meta.settlement.api;

import com.lol.meta.settlement.api.validation.NonZeroInteger;
import jakarta.validation.constraints.NotNull;
import jakarta.validation.constraints.Positive;

public record InventoryDeltaRequest(
    @NotNull @Positive Long itemId,
    @NotNull @NonZeroInteger Integer quantityDelta,
    @NotNull @Positive Long sourceDropId) {}
