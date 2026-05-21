package com.lol.meta.settlement.repository;

import java.sql.SQLException;
import org.springframework.dao.DataIntegrityViolationException;
import org.springframework.jdbc.UncategorizedSQLException;

final class MySqlConstraintExceptionTranslator {

  private static final int CHECK_CONSTRAINT_VIOLATION = 3819;

  private MySqlConstraintExceptionTranslator() {}

  static RuntimeException translate(UncategorizedSQLException exception) {
    SQLException sqlException = exception.getSQLException();
    if (sqlException != null && sqlException.getErrorCode() == CHECK_CONSTRAINT_VIOLATION) {
      return new DataIntegrityViolationException(sqlException.getMessage(), exception);
    }

    return exception;
  }
}
