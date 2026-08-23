// Copyright 2026 the adbcbridge authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

// Parameter binding (Arrow -> SQLBindParameter) and bulk ingest.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odbc_internal.h"

// Per-parameter scratch storage for one row.
struct ParamSlot {
  SQLSMALLINT c_type;
  SQLSMALLINT sql_type;
  SQLULEN column_size;
  SQLSMALLINT decimal_digits;
  SQLLEN indicator;
  SQLLEN buffer_length;
  const void* data;  // points into fixed or into Arrow buffers
  struct ArrowBuffer wbuf;  // UTF-16 conversion of string parameters
  union {
    unsigned char bit;
    SQLINTEGER i32;
    SQLBIGINT i64;
    SQLUBIGINT u64;
    SQLDOUBLE f64;
    DATE_STRUCT date;
    TIME_STRUCT time;
    TIMESTAMP_STRUCT ts;
    char text[64];
  } fixed;
};

static void CivilFromDays(int64_t z, int* y, unsigned* m, unsigned* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = (int64_t)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = (int)(yy + (*m <= 2));
}

// UTF-8 -> UTF-16 (SQLWCHAR units) into buf; returns number of units.
static ArrowErrorCode Utf8ToUtf16(struct ArrowBuffer* buf, const char* s, int64_t n, int64_t* units) {
  buf->size_bytes = 0;
  NANOARROW_RETURN_NOT_OK(ArrowBufferReserve(buf, (n + 1) * (int64_t)sizeof(SQLWCHAR)));
  SQLWCHAR* o = (SQLWCHAR*)buf->data;
  int64_t k = 0;
  for (int64_t i = 0; i < n;) {
    unsigned char c = (unsigned char)s[i];
    uint32_t cp;
    int len;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c & 0xE0) == 0xC0 && i + 1 < n) { cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); len = 2; }
    else if ((c & 0xF0) == 0xE0 && i + 2 < n) { cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); len = 3; }
    else if ((c & 0xF8) == 0xF0 && i + 3 < n) { cp = ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F); len = 4; }
    else { cp = 0xFFFD; len = 1; }
    i += len;
    if (cp >= 0x10000) {
      cp -= 0x10000;
      o[k++] = (SQLWCHAR)(0xD800 + (cp >> 10));
      o[k++] = (SQLWCHAR)(0xDC00 + (cp & 0x3FF));
    } else {
      o[k++] = (SQLWCHAR)cp;
    }
  }
  o[k] = 0;
  buf->size_bytes = k * (int64_t)sizeof(SQLWCHAR);
  *units = k;
  return NANOARROW_OK;
}

static AdbcStatusCode SlotFromArrow(struct ParamSlot* p, const struct ArrowSchemaView* sv,
                                    const struct ArrowArrayView* av, int64_t row,
                                    const struct OdbcReaderOptions* opts,
                                    struct AdbcError* error) {
  if (ArrowArrayViewIsNull(av, row)) {
    p->indicator = SQL_NULL_DATA;
    p->data = &p->fixed;
    p->buffer_length = 0;
  } else {
    p->indicator = 0;
  }
  if (p->indicator == SQL_NULL_DATA && opts->null_param_as_varchar) {
    p->c_type = SQL_C_CHAR; p->sql_type = SQL_VARCHAR; p->column_size = 1;
    p->decimal_digits = 0; p->data = p->fixed.text; p->buffer_length = 0;
    return ADBC_STATUS_OK;
  }
  switch (sv->type) {
    case NANOARROW_TYPE_NA:
      // Untyped NULL (e.g. Python None): send as a NULL varchar.
      p->c_type = SQL_C_CHAR; p->sql_type = SQL_VARCHAR; p->column_size = 1;
      p->indicator = SQL_NULL_DATA;
      p->data = p->fixed.text; p->buffer_length = 0;
      break;
    case NANOARROW_TYPE_BOOL:
      if (opts->bool_param_as_int) {
        p->c_type = SQL_C_SBIGINT; p->sql_type = SQL_INTEGER;
        p->fixed.i64 = ArrowArrayViewGetIntUnsafe(av, row) ? 1 : 0;
        p->data = &p->fixed.i64; p->buffer_length = sizeof(SQLBIGINT);
      } else {
        p->c_type = SQL_C_BIT; p->sql_type = SQL_BIT;
        p->fixed.bit = (unsigned char)ArrowArrayViewGetIntUnsafe(av, row);
        p->data = &p->fixed.bit; p->buffer_length = 1;
      }
      break;
    case NANOARROW_TYPE_INT8: case NANOARROW_TYPE_INT16:
    case NANOARROW_TYPE_INT32: case NANOARROW_TYPE_INT64:
    case NANOARROW_TYPE_UINT8: case NANOARROW_TYPE_UINT16:
    case NANOARROW_TYPE_UINT32: case NANOARROW_TYPE_UINT64: {
      bool is_unsigned = sv->type == NANOARROW_TYPE_UINT8 || sv->type == NANOARROW_TYPE_UINT16 ||
                         sv->type == NANOARROW_TYPE_UINT32 || sv->type == NANOARROW_TYPE_UINT64;
      uint64_t u = 0; int64_t v = 0;
      if (p->indicator != SQL_NULL_DATA) {
        if (is_unsigned) u = ArrowArrayViewGetUIntUnsafe(av, row);
        else v = ArrowArrayViewGetIntUnsafe(av, row);
      }
      bool fits32 = is_unsigned ? (u <= INT32_MAX) : (v >= INT32_MIN && v <= INT32_MAX);
      if (fits32) {
        // SQL_C_SLONG is the most universally supported integer binding.
        p->c_type = SQL_C_SLONG; p->sql_type = SQL_INTEGER;
        p->fixed.i32 = is_unsigned ? (SQLINTEGER)u : (SQLINTEGER)v;
        p->data = &p->fixed.i32; p->buffer_length = sizeof(SQLINTEGER);
      } else if (opts->bigint_param_as_string) {
        int n = is_unsigned ? snprintf(p->fixed.text, sizeof(p->fixed.text), "%llu", (unsigned long long)u)
                            : snprintf(p->fixed.text, sizeof(p->fixed.text), "%lld", (long long)v);
        p->c_type = SQL_C_CHAR; p->sql_type = SQL_NUMERIC; p->column_size = 20; p->decimal_digits = 0;
        p->data = p->fixed.text; p->buffer_length = n + 1;
        if (p->indicator != SQL_NULL_DATA) p->indicator = n;
      } else if (is_unsigned) {
        p->c_type = SQL_C_UBIGINT; p->sql_type = SQL_BIGINT;
        p->fixed.u64 = u; p->data = &p->fixed.u64; p->buffer_length = sizeof(SQLUBIGINT);
      } else {
        p->c_type = SQL_C_SBIGINT; p->sql_type = SQL_BIGINT;
        p->fixed.i64 = v; p->data = &p->fixed.i64; p->buffer_length = sizeof(SQLBIGINT);
      }
      break;
    }
    case NANOARROW_TYPE_HALF_FLOAT: case NANOARROW_TYPE_FLOAT: case NANOARROW_TYPE_DOUBLE:
      p->c_type = SQL_C_DOUBLE; p->sql_type = SQL_DOUBLE;
      p->fixed.f64 = ArrowArrayViewGetDoubleUnsafe(av, row);
      p->data = &p->fixed.f64; p->buffer_length = sizeof(SQLDOUBLE);
      break;
    case NANOARROW_TYPE_STRING: case NANOARROW_TYPE_LARGE_STRING: {
      int64_t units = 0;
      if (p->indicator != SQL_NULL_DATA) {
        struct ArrowStringView s = ArrowArrayViewGetStringUnsafe(av, row);
        CHECK_NA(INTERNAL, Utf8ToUtf16(&p->wbuf, s.data, s.size_bytes, &units), error);
      }
      p->c_type = SQL_C_WCHAR;
      p->sql_type = units > 4000 ? SQL_WLONGVARCHAR : SQL_WVARCHAR;
      p->column_size = (SQLULEN)(units > 0 ? units : 1);
      p->data = p->wbuf.data; p->buffer_length = units * (int64_t)sizeof(SQLWCHAR);
      if (p->indicator != SQL_NULL_DATA) p->indicator = p->buffer_length;
      break;
    }
    case NANOARROW_TYPE_BINARY: case NANOARROW_TYPE_LARGE_BINARY:
    case NANOARROW_TYPE_FIXED_SIZE_BINARY: {
      struct ArrowBufferView b = {{NULL}, 0};
      if (p->indicator != SQL_NULL_DATA) b = ArrowArrayViewGetBytesUnsafe(av, row);
      p->c_type = SQL_C_BINARY;
      p->sql_type = b.size_bytes > 4000 ? SQL_LONGVARBINARY : SQL_VARBINARY;
      p->column_size = (SQLULEN)(b.size_bytes > 0 ? b.size_bytes : 1);
      p->data = b.data.as_uint8; p->buffer_length = b.size_bytes;
      if (p->indicator != SQL_NULL_DATA) p->indicator = b.size_bytes;
      break;
    }
    case NANOARROW_TYPE_DATE32: {
      int y; unsigned m, d;
      CivilFromDays(ArrowArrayViewGetIntUnsafe(av, row), &y, &m, &d);
      p->fixed.date.year = (SQLSMALLINT)y; p->fixed.date.month = (SQLUSMALLINT)m;
      p->fixed.date.day = (SQLUSMALLINT)d;
      p->c_type = SQL_C_TYPE_DATE; p->sql_type = SQL_TYPE_DATE;
      p->data = &p->fixed.date; p->buffer_length = sizeof(DATE_STRUCT);
      break;
    }
    case NANOARROW_TYPE_TIMESTAMP: {
      int64_t v = ArrowArrayViewGetIntUnsafe(av, row);
      int64_t per_sec = 1, frac_mul = 1;
      switch (sv->time_unit) {
        case NANOARROW_TIME_UNIT_SECOND: per_sec = 1; frac_mul = 1000000000; break;
        case NANOARROW_TIME_UNIT_MILLI: per_sec = 1000; frac_mul = 1000000; break;
        case NANOARROW_TIME_UNIT_MICRO: per_sec = 1000000; frac_mul = 1000; break;
        case NANOARROW_TIME_UNIT_NANO: per_sec = 1000000000; frac_mul = 1; break;
      }
      int64_t secs = v / per_sec, frac = v % per_sec;
      if (frac < 0) { frac += per_sec; secs -= 1; }
      int64_t days = secs / 86400, sod = secs % 86400;
      if (sod < 0) { sod += 86400; days -= 1; }
      int y; unsigned m, d;
      CivilFromDays(days, &y, &m, &d);
      TIMESTAMP_STRUCT* ts = &p->fixed.ts;
      ts->year = (SQLSMALLINT)y; ts->month = (SQLUSMALLINT)m; ts->day = (SQLUSMALLINT)d;
      ts->hour = (SQLUSMALLINT)(sod / 3600); ts->minute = (SQLUSMALLINT)((sod % 3600) / 60);
      ts->second = (SQLUSMALLINT)(sod % 60); ts->fraction = (SQLUINTEGER)(frac * frac_mul);
      p->c_type = SQL_C_TYPE_TIMESTAMP; p->sql_type = SQL_TYPE_TIMESTAMP;
      // Fractional digits by unit; capped at 7 (SQL Server's DATETIME2 maximum).
      {
        int digits = sv->time_unit == NANOARROW_TIME_UNIT_SECOND ? 0
                     : sv->time_unit == NANOARROW_TIME_UNIT_MILLI ? 3
                     : sv->time_unit == NANOARROW_TIME_UNIT_MICRO ? 6 : 7;
        p->decimal_digits = (SQLSMALLINT)digits;
        p->column_size = (SQLULEN)(digits ? 20 + digits : 19);
      }
      p->data = ts; p->buffer_length = sizeof(TIMESTAMP_STRUCT);
      break;
    }
    case NANOARROW_TYPE_DECIMAL128: case NANOARROW_TYPE_DECIMAL256: {
      if (p->indicator != SQL_NULL_DATA) {
        struct ArrowDecimal dec;
        ArrowDecimalInit(&dec, sv->type == NANOARROW_TYPE_DECIMAL128 ? 128 : 256,
                         sv->decimal_precision, sv->decimal_scale);
        ArrowArrayViewGetDecimalUnsafe(av, row, &dec);
        struct ArrowBuffer buf;
        ArrowBufferInit(&buf);
        CHECK_NA(INTERNAL, ArrowDecimalAppendStringToBuffer(&dec, &buf), error);
        size_t n = (size_t)buf.size_bytes < sizeof(p->fixed.text) - 1 ? (size_t)buf.size_bytes
                                                                        : sizeof(p->fixed.text) - 1;
        memcpy(p->fixed.text, buf.data, n);
        p->fixed.text[n] = '\0';
        ArrowBufferReset(&buf);
        p->indicator = (SQLLEN)n;
      }
      p->c_type = SQL_C_CHAR;
      if (opts->decimal_param_as_varchar) {
        p->sql_type = SQL_VARCHAR; p->column_size = sizeof(p->fixed.text); p->decimal_digits = 0;
      } else {
        p->sql_type = SQL_DECIMAL;
        p->column_size = (SQLULEN)sv->decimal_precision; p->decimal_digits = (SQLSMALLINT)sv->decimal_scale;
      }
      p->data = p->fixed.text; p->buffer_length = (SQLLEN)sizeof(p->fixed.text);
      break;
    }
    default:
      InternalAdbcSetError(error, "Unsupported Arrow type for parameter binding: %s",
                           ArrowTypeString(sv->type));
      return ADBC_STATUS_NOT_IMPLEMENTED;
  }
  return ADBC_STATUS_OK;
}

// Execute hstmt once per row of the bound stream, returning total rows affected.
// If `out` is non-NULL, the result set of the (single) execution is exposed.
static AdbcStatusCode ExecuteRows(struct OdbcStatement* stmt, struct ArrowArrayStream* out,
                                  int64_t* rows_affected, struct AdbcError* error) {
  SQLHSTMT hstmt = stmt->ref->hstmt;
  struct ArrowArrayStream* stream = &stmt->bind_stream;
  struct ArrowSchema schema;
  schema.release = NULL;
  int rc = stream->get_schema(stream, &schema);
  if (rc != 0) {
    InternalAdbcSetError(error, "Bound stream get_schema failed: %s", stream->get_last_error(stream));
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  struct ArrowError na_error;
  struct ArrowArrayView view;
  AdbcStatusCode status = ADBC_STATUS_OK;
  int64_t total = 0;
  bool have_result = false;

  int64_t ncols = schema.n_children;
  struct ArrowSchemaView* svs = calloc((size_t)(ncols > 0 ? ncols : 1), sizeof(*svs));
  struct ParamSlot* slots = calloc((size_t)(ncols > 0 ? ncols : 1), sizeof(*slots));
  for (int64_t i = 0; i < ncols; i++) ArrowBufferInit(&slots[i].wbuf);
  CHECK_NA_DETAIL(INTERNAL, ArrowArrayViewInitFromSchema(&view, &schema, &na_error), &na_error, error);
  for (int64_t i = 0; i < ncols; i++) {
    CHECK_NA_DETAIL(INTERNAL, ArrowSchemaViewInit(&svs[i], schema.children[i], &na_error), &na_error, error);
  }

  for (;;) {
    struct ArrowArray batch;
    batch.release = NULL;
    rc = stream->get_next(stream, &batch);
    if (rc != 0) {
      InternalAdbcSetError(error, "Bound stream get_next failed: %s", stream->get_last_error(stream));
      status = ADBC_STATUS_INVALID_ARGUMENT;
      break;
    }
    if (!batch.release) break;
    if (ArrowArrayViewSetArray(&view, &batch, &na_error) != NANOARROW_OK) {
      InternalAdbcSetError(error, "Invalid bound batch: %s", na_error.message);
      status = ADBC_STATUS_INVALID_ARGUMENT;
      batch.release(&batch);
      break;
    }
    for (int64_t row = 0; row < batch.length && status == ADBC_STATUS_OK; row++) {
      if (have_result) {
        InternalAdbcSetError(error, "Cannot bind more than one row to a query that returns a result set");
        status = ADBC_STATUS_NOT_IMPLEMENTED;
        break;
      }
      SQLFreeStmt(hstmt, SQL_CLOSE);
      for (int64_t i = 0; i < ncols; i++) {
        status = SlotFromArrow(&slots[i], &svs[i], view.children[i], row, &stmt->reader_opts, error);
        if (status != ADBC_STATUS_OK) break;
        struct ParamSlot* p = &slots[i];
        SQLRETURN r = SQLBindParameter(hstmt, (SQLUSMALLINT)(i + 1), SQL_PARAM_INPUT, p->c_type,
                                       p->sql_type, p->column_size, p->decimal_digits,
                                       (SQLPOINTER)p->data, p->buffer_length, &p->indicator);
        if (!SQL_SUCCEEDED(r)) { status = OdbcSetError(SQL_HANDLE_STMT, hstmt, "SQLBindParameter", error); break; }
      }
      if (status != ADBC_STATUS_OK) break;
      SQLRETURN r = stmt->prepared ? SQLExecute(hstmt)
                                   : SQLExecDirect(hstmt, (SQLCHAR*)stmt->query, SQL_NTS);
      if (!SQL_SUCCEEDED(r) && r != SQL_NO_DATA) {
        status = OdbcSetError(SQL_HANDLE_STMT, hstmt, stmt->prepared ? "SQLExecute" : "SQLExecDirect", error);
        break;
      }
      SQLSMALLINT nres = 0;
      SQLNumResultCols(hstmt, &nres);
      if (nres > 0 && out) {
        have_result = true;
      } else {
        SQLLEN count = 0;
        if (SQL_SUCCEEDED(SQLRowCount(hstmt, &count)) && count > 0) total += count;
        if (nres > 0) SQLCloseCursor(hstmt);
      }
    }
    batch.release(&batch);
    if (status != ADBC_STATUS_OK) break;
  }
  ArrowArrayViewReset(&view);
  free(svs);
  if (schema.release) schema.release(&schema);
  SQLFreeStmt(hstmt, SQL_RESET_PARAMS);
  for (int64_t i = 0; i < ncols; i++) ArrowBufferReset(&slots[i].wbuf);
  // The stream is consumed; drop it.
  stream->release(stream);
  memset(stream, 0, sizeof(*stream));
  stmt->has_bind = false;
  if (status != ADBC_STATUS_OK) { free(slots); return status; }
  if (rows_affected) *rows_affected = have_result ? -1 : total;
  if (out) {
    if (have_result) {
      status = OdbcReaderInit(stmt->ref, &stmt->reader_opts, out, error);
    } else {
      struct ArrowSchema empty;
      ArrowSchemaInit(&empty);
      ArrowSchemaSetTypeStruct(&empty, 0);
      ArrowBasicArrayStreamInit(out, &empty, 0);
    }
  }
  free(slots);
  return status;
}

AdbcStatusCode OdbcStatementEnsureHandle(struct OdbcStatement* stmt, struct AdbcError* error);

AdbcStatusCode OdbcStatementExecuteBound(struct OdbcStatement* stmt, struct ArrowArrayStream* out,
                                         int64_t* rows_affected, struct AdbcError* error) {
  RAISE_ADBC(OdbcStatementEnsureHandle(stmt, error));
  if (!stmt->prepared) {
    ODBC_CHECK(SQLPrepare(stmt->ref->hstmt, (SQLCHAR*)stmt->query, SQL_NTS), SQL_HANDLE_STMT,
               stmt->ref->hstmt, "SQLPrepare", error);
    stmt->prepared = true;
  }
  return ExecuteRows(stmt, out, rows_affected, error);
}

// ---------------------------------------------------------------------------
// Bulk ingest

// Ask the driver for its name of one SQL type via SQLGetTypeInfo. Returns false if the
// driver has no such type.
static bool TypeNameOne(SQLHDBC hdbc, SQLSMALLINT sql_type, int64_t length, int32_t precision,
                        int32_t scale, char* out, size_t out_size) {
  SQLHSTMT hstmt = NULL;
  bool done = false;
  if (!SQL_SUCCEEDED(SQLAllocHandle(SQL_HANDLE_STMT, hdbc, &hstmt))) return false;
  if (SQL_SUCCEEDED(SQLGetTypeInfo(hstmt, sql_type)) && SQL_SUCCEEDED(SQLFetch(hstmt))) {
    char name[256] = {0}, params[256] = {0};
    SQLLEN ind1 = 0, ind2 = 0;
    SQLGetData(hstmt, 1, SQL_C_CHAR, name, sizeof(name), &ind1);
    SQLGetData(hstmt, 6, SQL_C_CHAR, params, sizeof(params), &ind2);  // CREATE_PARAMS
    if (ind1 > 0) {
      // Some drivers return names with embedded parameters, e.g. "NUMBER(19,0)" or
      // "decimal(p,s)"; strip anything from '(' so we can apply our own parameters.
      char* paren = strchr(name, '(');
      if (paren) *paren = '\0';
      bool has_params = ind2 > 0;
      if (has_params && strstr(params, "precision") && precision > 0) {
        snprintf(out, out_size, "%s(%d,%d)", name, precision, scale);
      } else if (has_params && strstr(params, "length") && length > 0) {
        snprintf(out, out_size, "%s(%lld)", name, (long long)length);
      } else {
        snprintf(out, out_size, "%s", name);
      }
      done = true;
    }
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
  return done;
}

// Try a chain of candidate SQL types (e.g. BIGINT, then NUMERIC(19,0) for Oracle).
static void TypeNameFor(SQLHDBC hdbc, const SQLSMALLINT* candidates, int n, int64_t length,
                        int32_t precision, int32_t scale, const char* fallback, char* out,
                        size_t out_size) {
  for (int i = 0; i < n; i++) {
    if (TypeNameOne(hdbc, candidates[i], length, precision, scale, out, out_size)) return;
  }
  snprintf(out, out_size, "%s", fallback);
}

static AdbcStatusCode ColumnTypeSql(SQLHDBC hdbc, const struct ArrowSchemaView* sv, char* out,
                                    size_t out_size, struct AdbcError* error) {
#define TYPES(...) ((const SQLSMALLINT[]){__VA_ARGS__})
#define CHAIN(fallback, ...)                                                              \
  TypeNameFor(hdbc, TYPES(__VA_ARGS__), (int)(sizeof(TYPES(__VA_ARGS__)) / sizeof(SQLSMALLINT)), \
              0, 0, 0, fallback, out, out_size)
  switch (sv->type) {
    case NANOARROW_TYPE_BOOL: CHAIN("BOOLEAN", SQL_BIT, SQL_TINYINT, SQL_SMALLINT); break;
    case NANOARROW_TYPE_INT8: case NANOARROW_TYPE_UINT8:
    case NANOARROW_TYPE_INT16: CHAIN("SMALLINT", SQL_SMALLINT, SQL_INTEGER); break;
    case NANOARROW_TYPE_UINT16:
    case NANOARROW_TYPE_INT32: CHAIN("INTEGER", SQL_INTEGER, SQL_BIGINT); break;
    case NANOARROW_TYPE_UINT32: case NANOARROW_TYPE_INT64: case NANOARROW_TYPE_UINT64:
      if (!TypeNameOne(hdbc, SQL_BIGINT, 0, 0, 0, out, out_size) &&
          !TypeNameOne(hdbc, SQL_DECIMAL, 0, 19, 0, out, out_size) &&
          !TypeNameOne(hdbc, SQL_NUMERIC, 0, 19, 0, out, out_size)) {
        snprintf(out, out_size, "BIGINT");
      }
      break;
    case NANOARROW_TYPE_HALF_FLOAT:
    case NANOARROW_TYPE_FLOAT: CHAIN("REAL", SQL_REAL, SQL_FLOAT, SQL_DOUBLE); break;
    case NANOARROW_TYPE_DOUBLE: CHAIN("DOUBLE PRECISION", SQL_DOUBLE, SQL_FLOAT); break;
    case NANOARROW_TYPE_STRING: case NANOARROW_TYPE_LARGE_STRING:
      CHAIN("TEXT", SQL_LONGVARCHAR, SQL_WLONGVARCHAR, SQL_VARCHAR); break;
    case NANOARROW_TYPE_BINARY: case NANOARROW_TYPE_LARGE_BINARY: case NANOARROW_TYPE_FIXED_SIZE_BINARY:
      CHAIN("BLOB", SQL_LONGVARBINARY, SQL_VARBINARY); break;
    case NANOARROW_TYPE_DATE32: CHAIN("DATE", SQL_TYPE_DATE, SQL_TYPE_TIMESTAMP); break;
    case NANOARROW_TYPE_TIMESTAMP: CHAIN("TIMESTAMP", SQL_TYPE_TIMESTAMP); break;
    case NANOARROW_TYPE_DECIMAL128: case NANOARROW_TYPE_DECIMAL256:
      if (!TypeNameOne(hdbc, SQL_DECIMAL, 0, sv->decimal_precision, sv->decimal_scale, out, out_size) &&
          !TypeNameOne(hdbc, SQL_NUMERIC, 0, sv->decimal_precision, sv->decimal_scale, out, out_size)) {
        snprintf(out, out_size, "DECIMAL(%d,%d)", sv->decimal_precision, sv->decimal_scale);
      }
      break;
    default:
      InternalAdbcSetError(error, "Unsupported Arrow type for ingest: %s", ArrowTypeString(sv->type));
      return ADBC_STATUS_NOT_IMPLEMENTED;
  }
#undef CHAIN
#undef TYPES
  return ADBC_STATUS_OK;
}

static void AppendQualifiedName(struct InternalAdbcStringBuilder* sb, const char* q,
                                const char* catalog, const char* schema, const char* table) {
  if (catalog && *catalog) InternalAdbcStringBuilderAppend(sb, "%s%s%s.", q, catalog, q);
  if (schema && *schema) InternalAdbcStringBuilderAppend(sb, "%s%s%s.", q, schema, q);
  InternalAdbcStringBuilderAppend(sb, "%s%s%s", q, table, q);
}

static AdbcStatusCode ExecSimple(struct OdbcConnection* conn, const char* sql, bool ignore_error,
                                 struct AdbcError* error) {
  SQLHSTMT hstmt = NULL;
  ODBC_CHECK(SQLAllocHandle(SQL_HANDLE_STMT, conn->hdbc, &hstmt), SQL_HANDLE_DBC, conn->hdbc,
             "SQLAllocHandle", error);
  SQLRETURN ret = SQLExecDirect(hstmt, (SQLCHAR*)sql, SQL_NTS);
  AdbcStatusCode s = ADBC_STATUS_OK;
  if (!SQL_SUCCEEDED(ret) && ret != SQL_NO_DATA && !ignore_error) {
    s = OdbcSetError(SQL_HANDLE_STMT, hstmt, sql, error);
  }
  SQLFreeHandle(SQL_HANDLE_STMT, hstmt);
  return s;
}

AdbcStatusCode OdbcStatementIngest(struct OdbcStatement* stmt, int64_t* rows_affected,
                                   struct AdbcError* error) {
  if (!stmt->has_bind) {
    InternalAdbcSetError(error, "Must bind data before bulk ingest");
    return ADBC_STATUS_INVALID_STATE;
  }
  struct OdbcConnection* conn = stmt->conn;
  const char* mode = stmt->ingest_mode ? stmt->ingest_mode : ADBC_INGEST_OPTION_MODE_CREATE;
  struct ArrowArrayStream* stream = &stmt->bind_stream;
  struct ArrowSchema schema;
  if (stream->get_schema(stream, &schema) != 0) {
    InternalAdbcSetError(error, "Bound stream get_schema failed: %s", stream->get_last_error(stream));
    return ADBC_STATUS_INVALID_ARGUMENT;
  }
  char q[8];
  OdbcQuoteChar(conn->hdbc, q);
  struct InternalAdbcStringBuilder sb;
  InternalAdbcStringBuilderInit(&sb, 256);
  AdbcStatusCode status = ADBC_STATUS_OK;

  bool do_create = strcmp(mode, ADBC_INGEST_OPTION_MODE_APPEND) != 0;
  if (strcmp(mode, ADBC_INGEST_OPTION_MODE_REPLACE) == 0) {
    InternalAdbcStringBuilderAppend(&sb, "DROP TABLE ");
    AppendQualifiedName(&sb, q, stmt->ingest_catalog, stmt->ingest_schema, stmt->ingest_table);
    ExecSimple(conn, sb.buffer, /*ignore_error=*/true, error);
    sb.size = 0;
  }
  if (do_create) {
    InternalAdbcStringBuilderAppend(&sb, "CREATE %sTABLE ", stmt->ingest_temporary ? "TEMPORARY " : "");
    AppendQualifiedName(&sb, q, stmt->ingest_catalog, stmt->ingest_schema, stmt->ingest_table);
    InternalAdbcStringBuilderAppend(&sb, " (");
    struct ArrowError na_error;
    for (int64_t i = 0; i < schema.n_children && status == ADBC_STATUS_OK; i++) {
      struct ArrowSchemaView sv;
      if (ArrowSchemaViewInit(&sv, schema.children[i], &na_error) != NANOARROW_OK) {
        InternalAdbcSetError(error, "Bad schema: %s", na_error.message);
        status = ADBC_STATUS_INVALID_ARGUMENT;
        break;
      }
      char tname[300];
      status = ColumnTypeSql(conn->hdbc, &sv, tname, sizeof(tname), error);
      const char* name = schema.children[i]->name ? schema.children[i]->name : "";
      InternalAdbcStringBuilderAppend(&sb, "%s%s%s%s %s", i ? ", " : "", q, name, q, tname);
    }
    InternalAdbcStringBuilderAppend(&sb, ")");
    if (status == ADBC_STATUS_OK) {
      bool ignore = strcmp(mode, ADBC_INGEST_OPTION_MODE_CREATE_APPEND) == 0;
      status = ExecSimple(conn, sb.buffer, ignore, error);
      if (status == ADBC_STATUS_UNKNOWN || status == ADBC_STATUS_INVALID_ARGUMENT) {
        status = ADBC_STATUS_ALREADY_EXISTS;
      }
    }
    sb.size = 0;
  }
  if (status != ADBC_STATUS_OK) {
    InternalAdbcStringBuilderReset(&sb);
    schema.release(&schema);
    return status;
  }

  // INSERT INTO t ("a", "b") VALUES (?, ?)
  InternalAdbcStringBuilderAppend(&sb, "INSERT INTO ");
  AppendQualifiedName(&sb, q, stmt->ingest_catalog, stmt->ingest_schema, stmt->ingest_table);
  InternalAdbcStringBuilderAppend(&sb, " (");
  for (int64_t i = 0; i < schema.n_children; i++) {
    const char* name = schema.children[i]->name ? schema.children[i]->name : "";
    InternalAdbcStringBuilderAppend(&sb, "%s%s%s%s", i ? ", " : "", q, name, q);
  }
  InternalAdbcStringBuilderAppend(&sb, ") VALUES (");
  for (int64_t i = 0; i < schema.n_children; i++) InternalAdbcStringBuilderAppend(&sb, i ? ", ?" : "?");
  InternalAdbcStringBuilderAppend(&sb, ")");
  schema.release(&schema);

  free(stmt->query);
  stmt->query = strdup(sb.buffer);
  InternalAdbcStringBuilderReset(&sb);
  stmt->prepared = false;
  return OdbcStatementExecuteBound(stmt, NULL, rows_affected, error);
}
