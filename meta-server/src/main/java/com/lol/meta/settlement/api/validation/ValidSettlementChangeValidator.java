package com.lol.meta.settlement.api.validation;

import com.lol.meta.settlement.api.SettlementRequest;
import jakarta.validation.ConstraintValidator;
import jakarta.validation.ConstraintValidatorContext;

public final class ValidSettlementChangeValidator
    implements ConstraintValidator<ValidSettlementChange, SettlementRequest> {

  @Override
  public boolean isValid(SettlementRequest value, ConstraintValidatorContext context) {
    if (value == null || value.goldDelta() == null || value.inventoryDeltas() == null) {
      return true;
    }
    if (value.goldDelta() != 0L) {
      return true;
    }
    return value.inventoryDeltas().stream()
        .anyMatch(
            delta ->
                delta != null && delta.quantityDelta() != null && delta.quantityDelta() != 0);
  }
}
