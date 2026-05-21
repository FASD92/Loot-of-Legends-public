package com.lol.meta.settlement.api.validation;

import jakarta.validation.ConstraintValidator;
import jakarta.validation.ConstraintValidatorContext;

public final class NonZeroIntegerValidator implements ConstraintValidator<NonZeroInteger, Integer> {

  @Override
  public boolean isValid(Integer value, ConstraintValidatorContext context) {
    return value == null || value != 0;
  }
}
