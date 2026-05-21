package com.lol.meta.settlement.api.validation;

import jakarta.validation.Constraint;
import jakarta.validation.Payload;
import java.lang.annotation.Documented;
import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

@Documented
@Constraint(validatedBy = ValidSettlementChangeValidator.class)
@Target(ElementType.TYPE)
@Retention(RetentionPolicy.RUNTIME)
public @interface ValidSettlementChange {
  String message() default "settlement request must include non-zero gold or inventory delta";

  Class<?>[] groups() default {};

  Class<? extends Payload>[] payload() default {};
}
